#include "board.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#include <stdint.h>

static const uint16_t led_pin[LED_COUNT] = {
    [LED_ROUTER] = GPIO0, [LED_POWER] = GPIO1, [LED_CLOUD] = GPIO5, [LED_PAIRING] = GPIO8};

#define RESET_BTN_PIN GPIO10 /* PB10, active low */
#define PAIR_BTN_PIN  GPIO15 /* PC15, active high */

/* GPIOA pins this firmware may configure. PA13/PA14 (SWD) must never appear in
 * any GPIOA mask (CLAUDE.md, docs/test-bench.md rule 5). libopencm3's
 * gpio_mode_setup/gpio_set_af/gpio_set_output_options read-modify-write only
 * the pins in the mask, so the SWD pins keep their reset configuration. */
#define GPIOA_ALLOWED (GPIO0 | GPIO1 | GPIO4 | GPIO5 | GPIO6 | GPIO7)
_Static_assert((GPIOA_ALLOWED & (GPIO13 | GPIO14)) == 0, "never touch PA13/PA14 (SWD)");
_Static_assert(((BOARD_RADIO_CS_PIN | BOARD_GDO0_PIN | BOARD_GDO2_PIN | BOARD_SPI1_SCK_PIN
                 | BOARD_SPI1_MISO_PIN | BOARD_SPI1_MOSI_PIN)
                & ~GPIOA_ALLOWED)
                   == 0,
               "GPIOA pin outside the allowed set");

/* Pin numbers of the two chip selects, for the 2-bit MODER fields. */
#define RADIO_CS_PIN_NO 4u /* PA4 */
#define FLASH_CS_PIN_NO 9u /* PB9 */
_Static_assert(BOARD_RADIO_CS_PIN == (1u << RADIO_CS_PIN_NO), "PA4 radio CSn");
_Static_assert(BOARD_FLASH_CS_PIN == (1u << FLASH_CS_PIN_NO), "PB9 flash CS#");
_Static_assert(RADIO_CS_PIN_NO != 13u && RADIO_CS_PIN_NO != 14u, "never touch PA13/PA14 (SWD)");
_Static_assert(FLASH_CS_PIN_NO != 6u && FLASH_CS_PIN_NO != 7u, "never touch PB6/PB7 (HomeKit I2C)");

void board_cs_early(void) {
    /* GPIOAEN = bit 0, GPIOBEN = bit 1 of RCC_AHB1ENR (rm0383.txt:5900-5960).
     * The read-back makes the enable complete before the first GPIO access
     * (STM32F4 errata "delay after an RCC peripheral clock enabling"; not in
     * RM0383) [I]. */
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    (void)RCC_AHB1ENR;

    /* Level first: BSy sets ODRy (rm0383.txt:8222-8245), so the pin leaves
     * input mode already high. */
    GPIO_BSRR(GPIOA) = BOARD_RADIO_CS_PIN;
    GPIO_BSRR(GPIOB) = BOARD_FLASH_CS_PIN;

    /* OTy = 0 push-pull, the reset value (rm0383.txt:8092-8110). */
    GPIO_OTYPER(GPIOA) &= ~(uint32_t)BOARD_RADIO_CS_PIN;
    GPIO_OTYPER(GPIOB) &= ~(uint32_t)BOARD_FLASH_CS_PIN;

    /* MODERy = 01 general purpose output (rm0383.txt:8064-8090). Read-modify-
     * write of the one 2-bit field: GPIOA's reset value 0xA8000000 (PA13,
     * PA14, PA15 in AF for SWD/JTAG) and GPIOB's 0x00000280 (PB3, PB4) are
     * kept. */
    GPIO_MODER(GPIOA) = (GPIO_MODER(GPIOA) & ~(3u << (2u * RADIO_CS_PIN_NO)))
                        | (1u << (2u * RADIO_CS_PIN_NO));
    GPIO_MODER(GPIOB) = (GPIO_MODER(GPIOB) & ~(3u << (2u * FLASH_CS_PIN_NO)))
                        | (1u << (2u * FLASH_CS_PIN_NO));
}

void board_init(void) {
    /* #28: both SPI1 chip selects high (deselected) as push-pull outputs
     * before anything else and before SPI1 is enabled. board_cs_early() has
     * already done this before the startup delay; repeating it here is
     * idempotent and also sets the output speed. ODR is set through BSRR
     * first, so the pins never glitch low. */
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    gpio_set(GPIOA, BOARD_RADIO_CS_PIN);
    gpio_set(GPIOB, BOARD_FLASH_CS_PIN);
    gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_RADIO_CS_PIN);
    gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_FLASH_CS_PIN);
    gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_RADIO_CS_PIN);
    gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_FLASH_CS_PIN);

    /* Radio GDO0 (PA0) and GDO2 (PA1): inputs. The CC110L drives both pins
     * (cc110l.txt:2885-2900); the pull-down only keeps them defined if the
     * chip is absent, so a dead radio cannot flood the EXTI. */
    gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, BOARD_GDO0_PIN | BOARD_GDO2_PIN);

    for (int i = 0; i < LED_COUNT; i++) {
        gpio_clear(GPIOB, led_pin[i]);
        gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, led_pin[i]);
        gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, led_pin[i]);
    }
    gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, RESET_BTN_PIN);
    gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, PAIR_BTN_PIN);
}

void board_led(enum led led, bool on) {
    if (on) {
        gpio_set(GPIOB, led_pin[led]);
    } else {
        gpio_clear(GPIOB, led_pin[led]);
    }
}

bool board_reset_button_raw(void) {
    return gpio_get(GPIOB, RESET_BTN_PIN) == 0;
}

bool board_pair_button_raw(void) {
    return gpio_get(GPIOC, PAIR_BTN_PIN) != 0;
}
