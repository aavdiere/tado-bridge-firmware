/* SPI1 master for the radio and flash bus (#9, #10).
 *
 * Clock: SPI1 runs from APB2 (96 MHz, docs/hardware.md "Clock tree"). The
 * slowest device limit on the bus is the CC110L burst access, 6.5 MHz with no
 * delay between bytes (cc110l.txt:1322-1332; single access 9 MHz). The
 * MX25R1635F allows 33 MHz for RDID/RDSR/READ even in its ultra-low-power mode
 * (mx25r1635f.txt:3268-3271). The divider is the smallest power of two that
 * keeps SCK at or below 6.5 MHz: /16 = 6 MHz at 96 MHz, /4 = 4 MHz on the
 * 16 MHz HSI-only fallback. BR encoding: rm0383.txt:33096-33105.
 *
 * Mode 0 (CPOL 0, CPHA 0): the CC110L samples SI on the rising SCLK edge and
 * shifts SO on the falling edge (cc110l.txt:1300-1318); the MX25R1635F
 * supports modes 0 and 3 (mx25r1635f.txt:124). MSB first for both
 * (cc110l.txt:1291). */
#include "spi1.h"

#include "board.h"
#include "log.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/spi.h>

#include "clock.h"

#define SCK_MAX_HZ   6500000u /* CC110L burst access limit, cc110l.txt:1330-1332 */
#define SPI_SPIN_MAX 100000u  /* a byte takes ~1.3 us at 6 MHz; this is >1 ms */

static SemaphoreHandle_t bus_lock;
static uint32_t          sck_hz;

void spi1_init(void) {
    /* Chip selects are already high outputs (board_init, #28). */
    configASSERT(gpio_get(GPIOA, BOARD_RADIO_CS_PIN) && gpio_get(GPIOB, BOARD_FLASH_CS_PIN));

    /* PA5 SCK, PA6 MISO, PA7 MOSI on AF5 (stm32f411ce.txt:2786-2799). MISO
     * gets a pull-up: when neither device drives it (CC110L GDO1 is 3-state
     * with IOCFG1 = 0x2E, cc110l.txt:2891-2893, 2901; flash SO is Hi-Z with CS#
     * high), it idles high, which also reads as "chip not ready" if the
     * radio is missing. */
    gpio_set_af(GPIOA, GPIO_AF5, BOARD_SPI1_SCK_PIN | BOARD_SPI1_MISO_PIN | BOARD_SPI1_MOSI_PIN);
    gpio_set_output_options(
        GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_SPI1_SCK_PIN | BOARD_SPI1_MOSI_PIN);
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, BOARD_SPI1_SCK_PIN | BOARD_SPI1_MOSI_PIN);
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, BOARD_SPI1_MISO_PIN);

    rcc_periph_clock_enable(RCC_SPI1);

    uint32_t br = 0; /* fPCLK / 2^(br+1) */
    while (br < 7 && clock_apb2_hz / (2u << br) > SCK_MAX_HZ) {
        br++;
    }
    sck_hz = clock_apb2_hz / (2u << br);

    /* Master, software NSS held high (SSM = SSI = 1, so the peripheral never
     * sees a mode fault from PA4, which is a GPIO here), 8-bit, MSB first,
     * full duplex, CPOL = CPHA = 0 (rm0383.txt:33022-33130). */
    SPI_CR1(SPI1) = 0;
    SPI_CR2(SPI1) = 0;
    SPI_CR1(SPI1) = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (br << 3);
    SPI_CR1(SPI1) |= SPI_CR1_SPE;

    bus_lock = xSemaphoreCreateMutex();
    configASSERT(bus_lock);
}

uint32_t spi1_sck_hz(void) {
    return sck_hz;
}

void spi1_lock(void) {
    if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        fw_panic("SPI1 bus lock timeout", __FILE__, __LINE__);
    }
}

void spi1_unlock(void) {
    xSemaphoreGive(bus_lock);
}

static void cs_write(enum spi1_dev dev, bool high) {
    uint32_t port = (dev == SPI1_DEV_RADIO) ? GPIOA : GPIOB;
    uint16_t pin  = (dev == SPI1_DEV_RADIO) ? BOARD_RADIO_CS_PIN : BOARD_FLASH_CS_PIN;
    if (high) {
        gpio_set(port, pin);
    } else {
        gpio_clear(port, pin);
    }
}

void spi1_select(enum spi1_dev dev) {
    /* Both devices share MISO: never select one while the other is selected. */
    bool other_high = (dev == SPI1_DEV_RADIO) ? gpio_get(GPIOB, BOARD_FLASH_CS_PIN)
                                              : gpio_get(GPIOA, BOARD_RADIO_CS_PIN);
    if (!other_high) {
        fw_panic("SPI1: both chip selects low", __FILE__, __LINE__);
    }
    cs_write(dev, false);
}

static void spin_until(uint32_t mask, uint32_t want, const char *what) {
    for (uint32_t n = SPI_SPIN_MAX; n; n--) {
        if ((SPI_SR(SPI1) & mask) == want) {
            return;
        }
    }
    fw_panic(what, __FILE__, __LINE__);
}

void spi1_deselect(enum spi1_dev dev) {
    spin_until(SPI_SR_TXE, SPI_SR_TXE, "SPI1 TXE timeout");
    spin_until(SPI_SR_BSY, 0, "SPI1 BSY timeout");
    cs_write(dev, true);
}

uint8_t spi1_xfer(uint8_t out) {
    spin_until(SPI_SR_TXE, SPI_SR_TXE, "SPI1 TXE timeout");
    SPI_DR(SPI1) = out;
    spin_until(SPI_SR_RXNE, SPI_SR_RXNE, "SPI1 RXNE timeout");
    return (uint8_t)SPI_DR(SPI1);
}

bool spi1_miso_high(void) {
    return gpio_get(GPIOA, BOARD_SPI1_MISO_PIN) != 0;
}
