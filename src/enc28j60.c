/* ENC28J60 driver on SPI2 (#12). Datasheet: docs/datasheets/enc28j60.txt
 * (Microchip DS39662E). Line numbers below refer to that file unless another
 * file is named.
 *
 * SILICON ERRATA: docs/datasheets/enc28j60-errata.txt (Microchip DS80349C,
 * written against data sheet DS39662C; the repo has DS39662E). Revisions by
 * EREVID: B1 = 02h, B4 = 04h, B5 = 05h, B7 = 06h (errata.txt:19-21); the
 * affected revisions per issue are in Table 2 (errata.txt:28-59). Every
 * workaround below is applied whatever the revision, since each is cheap:
 *   #1  MAC registers unreliable below 8 MHz SCK (B1, B4; errata.txt:86-112):
 *       not met: the board limit is 6 MHz (SCK_MAX_HZ); only warned on B1/B4.
 *   #2  CLKRDY set early after a reset (all; :115-133): wait at least 1 ms
 *       after a reset instead of trusting CLKRDY.
 *   #5  Receive buffer must start at 0000h (all; :112-137): RX 0000h-19FFh,
 *       TX above it.
 *   #6  PKTIF unreliable (all; :144-170): pending frames are counted with
 *       EPKTCNT; INT itself stays reliable (:158-164).
 *   #9  Half-duplex loopback unreliable (all; :166-184): PHCON2.HDLDIS set.
 *   #12 A transmit abort may stall the transmit logic (all; :231-258): TXRST
 *       is pulsed before every transmission, then TXIF and TXERIF cleared.
 *   #13 In half duplex a received link pulse can look like a collision; past
 *       MACLCON2 bytes it becomes a late collision, the frame is aborted
 *       without retry, and the abort may leave TXRTS set with no TXIF (B5, B7;
 *       :206-254, Example 1 :265-289): completion is TXIF or TXERIF; TXRTS is
 *       cleared by software; a late collision is retransmitted up to 16 times.
 *   #14 Even ERXRDPT may corrupt the receive buffer (all; :293-325): ERXND is
 *       odd and ERXRDPT is set as in Example 2 (:315-320).
 *   #15 ESTAT.LATECOL unreliable (all; :293-309): the late collision bit 29 of
 *       the transmit status vector is read instead.
 *   #16 LED polarity misdetected at reset, which also resets PDPXMD wrongly
 *       (all; :331-353): PHCON1.PDPXMD is always written.
 *   #17 DMA checksum aborts reception (all; :356-381): the DMA is not used.
 *   #19 The SPI reset command does nothing in power save (all; :350-385):
 *       the soft-reset fallback follows the sequence given there.
 * Not applicable here: #3 and #8 (B1/B4 temperature range and RBIAS value,
 * board hardware; logged if such a revision is read), #4 (CLKOUT), #7
 * (polarity correction: PHSTAT2.PLRITY is logged), #10 (full-duplex
 * loopback), #11 (PHLCON 1110; PHLCON is not changed), #18 (pattern match).
 *
 * SPI (section 4, :1316-1615): mode 0,0 only, SCK idle low, MSB first
 * (:1316-1326). Opcodes :1379-1395. A read of a MAC or MII register returns a
 * dummy byte first (:1411-1418). Bit field set/clear work on ETH registers
 * only (:1556-1558). CS timing (:4143-4150): setup 50 ns, hold 10 ns (ETH and
 * buffer) or 210 ns (MAC and MII registers), disable 50 ns; SCK at most
 * 20 MHz (:17, :4144). */
#include "enc28j60.h"

#include "board.h"
#include "clock.h"
#include "log.h"
#include "timebase.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/spi.h>

/* --- Registers (Table 3-1, :712-746; Table 3-2, :753-855) ------------------ */
/* Register id: bits 4:0 address, bits 6:5 bank, bit 7 = MAC or MII register
 * (dummy byte on read, 210 ns CS hold). Addresses 0x1B-0x1F are the common
 * registers in every bank (:704-708). */
#define REG(bank, addr) ((uint8_t)(((bank) << 5) | (addr)))
#define MREG            0x80u
#define REG_ADDR(r)     ((uint8_t)((r) & 0x1Fu))
#define REG_BANK(r)     ((uint8_t)(((r) >> 5) & 0x03u))

#define EIE   REG(0, 0x1B)
#define EIR   REG(0, 0x1C)
#define ESTAT REG(0, 0x1D)
#define ECON2 REG(0, 0x1E)
#define ECON1 REG(0, 0x1F)

#define ERDPTL   REG(0, 0x00)
#define EWRPTL   REG(0, 0x02)
#define ETXSTL   REG(0, 0x04)
#define ETXNDL   REG(0, 0x06)
#define ERXSTL   REG(0, 0x08)
#define ERXNDL   REG(0, 0x0A)
#define ERXRDPTL REG(0, 0x0C)
#define ERXRDPTH REG(0, 0x0D)

#define ERXFCON REG(1, 0x18)
#define EPKTCNT REG(1, 0x19)

#define MACON1   (REG(2, 0x00) | MREG)
#define MACON3   (REG(2, 0x02) | MREG)
#define MACON4   (REG(2, 0x03) | MREG)
#define MABBIPG  (REG(2, 0x04) | MREG)
#define MAIPGL   (REG(2, 0x06) | MREG)
#define MAIPGH   (REG(2, 0x07) | MREG)
#define MAMXFLL  (REG(2, 0x0A) | MREG)
#define MICMD    (REG(2, 0x12) | MREG)
#define MIREGADR (REG(2, 0x14) | MREG)
#define MIWRL    (REG(2, 0x16) | MREG)
#define MIWRH    (REG(2, 0x17) | MREG)
#define MIRDL    (REG(2, 0x18) | MREG)
#define MIRDH    (REG(2, 0x19) | MREG)

/* MAADR1 holds the first byte of the address (:840-845). */
#define MAADR5 (REG(3, 0x00) | MREG)
#define MAADR6 (REG(3, 0x01) | MREG)
#define MAADR3 (REG(3, 0x02) | MREG)
#define MAADR4 (REG(3, 0x03) | MREG)
#define MAADR1 (REG(3, 0x04) | MREG)
#define MAADR2 (REG(3, 0x05) | MREG)
#define MISTAT (REG(3, 0x0A) | MREG)
#define EREVID REG(3, 0x12)

/* Bits: EIE :3323-3355, EIR :3363-3393, ESTAT :3283-3313, ECON2 :917-944,
 * ECON1 :872-905, ERXFCON :2486-2542, MACON1 :1837-1861, MACON3 :1868-1908,
 * MACON4 :1915-1942, MICMD :1149-1166, MISTAT :1171-1192. */
#define EIE_INTIE      0x80u
#define EIE_PKTIE      0x40u
#define EIE_LINKIE     0x10u
#define EIE_TXIE       0x08u
#define EIE_TXERIE     0x02u
#define EIE_RXERIE     0x01u
#define EIR_DMAIF      0x20u
#define EIR_LINKIF     0x10u
#define EIR_TXIF       0x08u
#define EIR_TXERIF     0x02u
#define EIR_RXERIF     0x01u
#define ESTAT_LATECOL  0x10u
#define ESTAT_TXABRT   0x02u
#define ESTAT_CLKRDY   0x01u
#define ECON2_AUTOINC  0x80u
#define ECON2_PKTDEC   0x40u
#define ECON2_PWRSV    0x20u
#define ECON1_TXRST    0x80u
#define ECON1_TXRTS    0x08u
#define ECON1_RXEN     0x04u
#define ECON1_BSEL     0x03u
#define ERXFCON_UCEN   0x80u
#define ERXFCON_CRCEN  0x20u
#define ERXFCON_BCEN   0x01u
#define MACON1_MARXEN  0x01u
#define MACON3_PADCFG0 0x20u
#define MACON3_TXCRCEN 0x10u
#define MACON3_FRMLNEN 0x02u
#define MACON3_FULDPX  0x01u
#define MACON4_DEFER   0x40u
#define MICMD_MIIRD    0x01u
#define MISTAT_BUSY    0x01u

/* PHY registers (Table 3-3, :1124-1134). */
#define PHCON1         0x00u
#define PHID1          0x02u
#define PHID2          0x03u
#define PHCON2         0x10u
#define PHSTAT2        0x11u
#define PHIE           0x12u
#define PHIR           0x13u
#define PHLCON         0x14u
#define PHCON1_PLOOPBK 0x4000u /* :3186-3188 */
#define PHCON1_PPWRSV  0x0800u /* :3190-3192 */
#define PHCON1_PDPXMD  0x0100u /* :3195-3197 */
#define PHCON2_FRCLNK  0x4000u /* :2010-2012 */
#define PHCON2_TXDIS   0x2000u /* :2013-2015 */
#define PHCON2_HDLDIS  0x0100u /* :2021-2026 */
#define PHIE_PLNKIE    0x0010u /* :3415-3421 */
#define PHIE_PGEIE     0x0002u
#define PHID1_VALUE    0x0083u /* :1128 */
#define PHID2_VALUE    0x1400u /* :1129: OUI bits 000101, part 00h, revision 0h */

/* SPI opcodes (:1379-1395). */
#define OP_RCR 0x00u
#define OP_RBM 0x3Au
#define OP_WCR 0x40u
#define OP_WBM 0x7Au
#define OP_BFS 0x80u
#define OP_BFC 0xA0u
#define OP_SRC 0xFFu

/* --- Buffer layout -------------------------------------------------------- */
/* 8 KB buffer, 0x0000-0x1FFF (:953-959). Receive first (errata #5), with an odd
 * end for errata #14: 6.5 KB, four full-size frames plus their 6-byte headers.
 * Transmit holds exactly one frame: control byte, at most ENC_TX_MAX bytes,
 * then the 7-byte status vector the chip writes after it (:1760-1766,
 * :2136-2141). ERXST and ETXST even, as recommended (:1770-1771, :2129-2131). */
#define RXST       0x0000u
#define RXND       0x19FFu
#define TXST       0x1A00u
#define BUF_LAST   0x1FFFu
#define RX_SIZE    (RXND - RXST + 1u)
#define RXST_RESET 0x05FAu /* ERXST after any reset (:771-772) */
_Static_assert(RXST == 0u, "errata #5: receive buffer at 0x0000");
_Static_assert((RXND & 1u) == 1u, "errata #14: ERXND odd so ERXRDPT can be odd");
_Static_assert(TXST == RXND + 1u && (TXST & 1u) == 0u, "transmit right after receive, even");
_Static_assert(TXST + 1u + ENC_TX_MAX + 7u - 1u <= BUF_LAST, "one full frame + status vector");

/* --- MAC settings (section 6.5, :1806-1834) ------------------------------- */
#define BBIPG_HALF 0x12u /* :1814-1815, :1966-1969 */
#define IPGL       0x12u /* :1816-1818 */
#define IPGH_HALF  0x0Cu /* :1819-1822 */

/* --- Timing --------------------------------------------------------------- */
/* The chip allows 20 MHz (:17, :4144), but this board does not: on the bench
 * unit (B7) 12 MHz reads garbage whatever the pin speed (medium, fast or
 * high), while 6, 3, 0.75 and 0.19 MHz read the reset values 20 of 20 times
 * (debugger test 2026-11-13) [V]. So 6 MHz from APB1 48 MHz, 4 MHz on the
 * HSI-only fallback. */
#define SCK_MAX_HZ   6500000u
#define SCK_MIN_HZ   8000000u /* errata #1, B1/B4 only (errata.txt:86-95) */
#define SPI_SPIN_MAX 100000u  /* a byte takes 1.3 us at 6 MHz; this is >1 ms */
#define RESET_LOW_US 10u      /* tRSTLOW >= 400 ns (:4058-4059) */
#define RESET_WAIT_MS \
    5u /* errata #2 >= 1 ms (errata.txt:124-127); OST 300 us (:299-300); PHY 50 us (:3111-3112) */
#define CLKRDY_TIMEOUT_MS 100u
#define MII_WAIT_US       11u /* 10.24 us per MIIM operation (:1063-1065, :1082) */
#define MII_TIMEOUT_US    1000u

#define INT_IRQ_PRIO (5u << 4) /* FromISR needs >= configMAX_SYSCALL (FreeRTOSConfig.h) */
_Static_assert(INT_IRQ_PRIO >= configMAX_SYSCALL_INTERRUPT_PRIORITY, "FromISR needs prio >= 5");
#define INT_STORM_WINDOW_US 1000u
#define INT_STORM_MAX_EDGES 32u

static uint32_t sck_hz;
static uint32_t spin_cs_setup, spin_hold_eth, spin_hold_mac, spin_cs_gap;
static uint8_t  cur_bank = 0xFFu; /* unknown: force a switch */
static uint8_t  revid;
static bool     link_last;

static uint16_t next_pkt; /* start of the next unread frame */
static uint16_t rx_next;  /* next-packet pointer of the open frame */
static uint16_t rx_left;  /* bytes of the open frame still readable */
static bool     rx_open;

static uint16_t tx_len;  /* bytes written since tx_begin() */
static uint16_t tx_sent; /* length of the frame in flight */
static bool     tx_open, tx_overflow, tx_in_flight;
static uint8_t  tx_retries; /* late-collision retransmissions of the frame in flight */
static uint8_t  tx_done_ok, tx_done_err, tx_done_retry, tx_last_estat;
static uint16_t tx_last_status;

static TaskHandle_t int_task;
static uint32_t     int_bits, storm_bits;
static uint32_t     storm_start, storm_edges;

/* --- Short delays ---------------------------------------------------------- */
/* At least 2 core cycles per iteration: SUBS and a taken BNE, and the
 * Cortex-M4 issues at most one instruction per cycle [I]. */
static void spin(uint32_t n) {
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
}

static uint32_t spin_for_ns(uint32_t ns) {
    uint32_t cycles = (ns * (clock_hz / 1000000u) + 999u) / 1000u;
    return cycles / 2u + 1u;
}

/* --- SPI2 ----------------------------------------------------------------- */
static void spi_wait(uint32_t mask, uint32_t want, const char *what) {
    for (uint32_t n = SPI_SPIN_MAX; n; n--) {
        if ((SPI_SR(SPI2) & mask) == want) {
            return;
        }
    }
    fw_panic(what, __FILE__, __LINE__);
}

static uint8_t xfer(uint8_t out) {
    spi_wait(SPI_SR_TXE, SPI_SR_TXE, "SPI2 TXE timeout");
    SPI_DR(SPI2) = out;
    spi_wait(SPI_SR_RXNE, SPI_SR_RXNE, "SPI2 RXNE timeout");
    return (uint8_t)SPI_DR(SPI2);
}

static void cs_low(void) {
    gpio_clear(GPIOB, BOARD_ENC_CS_PIN);
    spin(spin_cs_setup); /* TCSS 50 ns */
}

static void cs_high(bool mac_mii) {
    /* Last bit out of the shifter: TXE, then BSY clear (rm0383.txt:31740-31745). */
    spi_wait(SPI_SR_TXE, SPI_SR_TXE, "SPI2 TXE timeout");
    spi_wait(SPI_SR_BSY, 0, "SPI2 BSY timeout");
    spin(mac_mii ? spin_hold_mac : spin_hold_eth); /* TCSH 210 / 10 ns */
    gpio_set(GPIOB, BOARD_ENC_CS_PIN);
    spin(spin_cs_gap); /* TCSD 50 ns before the next CS low */
}

static void op_bit(uint8_t op, uint8_t reg, uint8_t bits);

static void set_bank(uint8_t reg) {
    if (REG_ADDR(reg) >= 0x1Bu) {
        return; /* common register */
    }
    uint8_t b = REG_BANK(reg);
    if (b != cur_bank) {
        op_bit(OP_BFC, ECON1, ECON1_BSEL);
        if (b) {
            op_bit(OP_BFS, ECON1, b);
        }
        cur_bank = b;
    }
}

/* BFS/BFC: ETH registers only (:1556-1558). */
static void op_bit(uint8_t op, uint8_t reg, uint8_t bits) {
    configASSERT((reg & MREG) == 0u);
    set_bank(reg);
    cs_low();
    xfer((uint8_t)(op | REG_ADDR(reg)));
    xfer(bits);
    cs_high(false);
}

static void bfs(uint8_t reg, uint8_t bits) {
    op_bit(OP_BFS, reg, bits);
}

static void bfc(uint8_t reg, uint8_t bits) {
    op_bit(OP_BFC, reg, bits);
}

static uint8_t rd(uint8_t reg) {
    set_bank(reg);
    cs_low();
    xfer((uint8_t)(OP_RCR | REG_ADDR(reg)));
    if (reg & MREG) {
        (void)xfer(0); /* dummy byte (:1411-1418) */
    }
    uint8_t v = xfer(0);
    cs_high((reg & MREG) != 0u);
    return v;
}

static void wr(uint8_t reg, uint8_t v) {
    set_bank(reg);
    cs_low();
    xfer((uint8_t)(OP_WCR | REG_ADDR(reg)));
    xfer(v);
    cs_high((reg & MREG) != 0u);
}

/* Register pairs: low byte at reg, high byte at reg + 1. Low byte written
 * first, which ERXRDPT requires (:1796-1797, :2384-2392). */
static uint16_t rd16(uint8_t reg) {
    uint16_t lo = rd(reg);
    return (uint16_t)(lo | (uint16_t)rd((uint8_t)(reg + 1u)) << 8);
}

static void wr16(uint8_t reg, uint16_t v) {
    wr(reg, (uint8_t)v);
    wr((uint8_t)(reg + 1u), (uint8_t)(v >> 8));
}

/* Buffer memory: ERDPT/EWRPT advance by themselves with ECON2.AUTOINC, and
 * reads wrap from ERXND to ERXST (:1469-1481, :1525-1532). */
static void rbm(uint8_t *buf, size_t n) {
    cs_low();
    xfer(OP_RBM);
    while (n--) {
        *buf++ = xfer(0);
    }
    cs_high(false);
}

static void wbm(const uint8_t *buf, size_t n) {
    cs_low();
    xfer(OP_WBM);
    while (n--) {
        xfer(*buf++);
    }
    cs_high(false);
}

/* --- PHY through MII (section 3.3, :1052-1109) ---------------------------- */
static bool mii_wait(void) {
    delay_us(MII_WAIT_US);
    uint32_t t0 = timebase_us();
    while (rd(MISTAT) & MISTAT_BUSY) {
        if ((uint32_t)(timebase_us() - t0) > MII_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

static bool phy_read(uint8_t addr, uint16_t *v) {
    wr(MIREGADR, addr);
    wr(MICMD, MICMD_MIIRD);
    bool ok = mii_wait();
    wr(MICMD, 0);
    if (!ok) {
        return false;
    }
    uint16_t lo = rd(MIRDL);
    *v          = (uint16_t)(lo | (uint16_t)rd(MIRDH) << 8);
    return true;
}

static bool phy_write(uint8_t addr, uint16_t v) {
    wr(MIREGADR, addr);
    wr(MIWRL, (uint8_t)v);
    wr(MIWRH, (uint8_t)(v >> 8)); /* starts the MIIM write (:1058-1062) */
    return mii_wait();
}

/* --- INT on PB2 / EXTI2 ----------------------------------------------------- */
/* INT is driven low while an enabled flag is set and EIE.INTIE = 1
 * (:3216-3241); falling edge. A dead or browned-out chip must not flood the
 * MCU: more than INT_STORM_MAX_EDGES edges per millisecond masks the line
 * and wakes the net task to re-initialise. Real edges come at most once per
 * service pass, because INT stays low until the task rearms it. */
void exti2_isr(void) {
    exti_reset_request(EXTI2);
    uint32_t bits = int_bits;
    uint32_t now  = timebase_us();
    if ((uint32_t)(now - storm_start) >= INT_STORM_WINDOW_US) {
        storm_start = now;
        storm_edges = 0;
    }
    if (++storm_edges > INT_STORM_MAX_EDGES) {
        exti_disable_request(EXTI2);
        bits |= storm_bits;
    }
    BaseType_t woken = pdFALSE;
    if (int_task) {
        xTaskNotifyFromISR(int_task, bits, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void int_enable(bool on) {
    /* EXTI_IMR is also read-modify-written by the radio (task and GDO ISR at
     * the same priority 5): mask both meanwhile. */
    taskENTER_CRITICAL();
    if (on) {
        exti_reset_request(EXTI2);
        nvic_clear_pending_irq(NVIC_EXTI2_IRQ);
        storm_edges = 0;
        exti_enable_request(EXTI2);
    } else {
        exti_disable_request(EXTI2);
    }
    taskEXIT_CRITICAL();
}

/* --- Setup ---------------------------------------------------------------- */
void enc28j60_setup(void *notify_task, uint32_t notify_bits, uint32_t notify_storm_bits) {
    int_task   = (TaskHandle_t)notify_task;
    int_bits   = notify_bits;
    storm_bits = notify_storm_bits;

    /* CS is already a high output (board_init). PB13 SCK, PB14 MISO, PB15 MOSI
     * on AF5 (stm32f411ce.txt:2910-2925). The ENC28J60 drives SO only while CS
     * is low (:1342); MISO gets the pull-up so it idles defined. */
    configASSERT(gpio_get(GPIOB, BOARD_ENC_CS_PIN));
    gpio_set_af(GPIOB, GPIO_AF5, BOARD_SPI2_SCK_PIN | BOARD_SPI2_MISO_PIN | BOARD_SPI2_MOSI_PIN);
    gpio_set_output_options(
        GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_SPI2_SCK_PIN | BOARD_SPI2_MOSI_PIN);
    gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, BOARD_SPI2_SCK_PIN | BOARD_SPI2_MOSI_PIN);
    gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, BOARD_SPI2_MISO_PIN);

    /* SPI2EN is RCC_APB1ENR bit 14 (rm0383.txt:6039-6042); RCC_APB1ENR and
     * the SYSCFG/EXTI registers below are read-modify-written, also by the
     * radio task (SYSCFG_EXTICR1 holds EXTI0-3, rm0383.txt:7095-7121). */
    taskENTER_CRITICAL();
    rcc_periph_clock_enable(RCC_SPI2);
    rcc_periph_clock_enable(RCC_SYSCFG);
    taskEXIT_CRITICAL();

    /* SPI2 runs from APB1 (48 MHz; 16 MHz on the HSI-only fallback,
     * docs/hardware.md "Clock tree"). Smallest divider at or below the board
     * limit SCK_MAX_HZ: /8 = 6 MHz, /4 = 4 MHz on the fallback. BR encoding
     * rm0383.txt:33096-33105. */
    uint32_t br = 0; /* fPCLK / 2^(br+1) */
    while (br < 7 && clock_apb1_hz / (2u << br) > SCK_MAX_HZ) {
        br++;
    }
    sck_hz = clock_apb1_hz / (2u << br);

    /* Master, software NSS held high (SSM = SSI = 1; PB12 is a GPIO), 8-bit,
     * MSB first, full duplex, CPOL = CPHA = 0 (:1316-1326;
     * rm0383.txt:33022-33130). */
    SPI_CR1(SPI2) = 0;
    SPI_CR2(SPI2) = 0;
    SPI_CR1(SPI2) = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (br << 3);
    SPI_CR1(SPI2) |= SPI_CR1_SPE;

    spin_cs_setup = spin_for_ns(50);  /* TCSS (:4145) */
    spin_hold_eth = spin_for_ns(10);  /* TCSH ETH and buffer (:4146) */
    spin_hold_mac = spin_for_ns(210); /* TCSH MAC and MII (:4147) */
    spin_cs_gap   = spin_for_ns(50);  /* TCSD (:4148) */

    /* EXTI2 from PB2, falling edge, masked until enc28j60_init() succeeds.
     * SYSCFG_EXTICR1 EXTI2 = 0001 selects port B (rm0383.txt:7105-7121);
     * EXTI2 is vector position 8 (rm0383.txt:10801). */
    taskENTER_CRITICAL();
    exti_disable_request(EXTI2);
    exti_select_source(EXTI2, GPIOB);
    exti_set_trigger(EXTI2, EXTI_TRIGGER_FALLING);
    taskEXIT_CRITICAL();
    nvic_set_priority(NVIC_EXTI2_IRQ, INT_IRQ_PRIO);
    nvic_enable_irq(NVIC_EXTI2_IRQ);
}

uint32_t enc28j60_sck_hz(void) {
    return sck_hz;
}

/* --- Init ----------------------------------------------------------------- */
static void hw_reset(void) {
    /* RESET low for longer than tRSTLOW (400 ns, :4058-4059), then released;
     * the chip's pull-up and the MCU pull-up take it high. PC14: never driven
     * high (board.c). trl, the high time between resets, is 2 us (:4055-4056),
     * far below the retry period. */
    gpio_clear(GPIOC, BOARD_ENC_RESET_PIN);
    gpio_mode_setup(GPIOC, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_ENC_RESET_PIN);
    delay_us(RESET_LOW_US);
    gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, BOARD_ENC_RESET_PIN);
    cur_bank = 0xFFu;
}

/* Errata #19 sequence (errata.txt:360-382): leave power save, wait 300 us,
 * reset command (:1587-1592), wait 1 ms (#2); the caller then confirms the
 * reset by reading reset values. */
static void soft_reset(void) {
    bfc(ECON2, ECON2_PWRSV);
    delay_us(300);
    cs_low();
    xfer(OP_SRC);
    cs_high(false);
    cur_bank = 0xFFu;
}

/* Silicon revision by EREVID, errata Table 1 (errata.txt:19-21). */
static const char *rev_name(uint8_t r) {
    switch (r) {
        case 0x02:
            return "B1";
        case 0x04:
            return "B4";
        case 0x05:
            return "B5";
        case 0x06:
            return "B7";
        default:
            return "unknown";
    }
}

struct check {
    uint8_t     reg;
    uint8_t     want;
    const char *name;
};

bool enc28j60_init(const uint8_t mac[6], struct enc28j60_info *info) {
    int_enable(false);
    rx_open = tx_open = tx_in_flight = false;
    tx_done_ok = tx_done_err = tx_done_retry = 0;
    *info                                    = (struct enc28j60_info){.rev_name = "unknown"};

    /* PC14 is OSC32_IN when LSEON is set, and the GPIO setting then has no
     * effect (stm32f411ce.txt:2738-2740; LSEON rm0383.txt:6529). The register
     * survives an MCU reset.
     * The pair button on PC15 works, so it is expected clear [I]; the reset
     * check below falls back to the soft reset either way. */
    if (RCC_BDCR & RCC_BDCR_LSEON) {
        LOG("eth: WARNING RCC_BDCR LSEON set: PC14 is the LSE pin, RESET line not usable");
    }

    hw_reset();
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS) + 1);

    /* Proof that the reset happened and the bus works: ERXST reads its reset
     * value, which init below overwrites with 0x0000. */
    uint16_t erxst = rd16(ERXSTL);
    if (erxst != RXST_RESET) {
        LOG("eth: ERXST %04x after the RESET pin pulse, expected %04x; trying the SPI soft reset",
            erxst,
            RXST_RESET);
        soft_reset();
        vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS) + 1); /* errata #2, #19 step 4 */
        info->soft_reset = true;
        erxst            = rd16(ERXSTL);
        if (erxst != RXST_RESET) {
            LOG("eth: ERXST %04x after soft reset: no ENC28J60 response on SPI2", erxst);
            return false;
        }
    }

    /* CLKRDY: set once the oscillator start-up timer has run after a
     * power-on reset (:309-321, :1777-1783); unaffected by other resets
     * (:804). Bounded. */
    TickType_t t0 = xTaskGetTickCount();
    while (!(rd(ESTAT) & ESTAT_CLKRDY)) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(CLKRDY_TIMEOUT_MS)) {
            LOG("eth: ESTAT.CLKRDY not set after %u ms", CLKRDY_TIMEOUT_MS);
            return false;
        }
        vTaskDelay(1);
    }

    revid          = rd(EREVID) & 0x1Fu; /* :851 */
    info->erevid   = revid;
    info->rev_name = rev_name(revid);
    if (revid == 0u || revid == 0x1Fu) {
        LOG("eth: EREVID %02x is not plausible (bus stuck?)", revid);
        return false;
    }
    if (revid == 0x02u || revid == 0x04u) {
        /* Board hardware, nothing firmware can do: commercial range only
         * (#3, errata.txt:77-85), RBIAS should be 2.7 kOhm (#8, :144-162). */
        LOG("eth: WARNING silicon %s: 0-70 C only and needs RBIAS 2.7 kOhm (errata #3, #8)",
            info->rev_name);
        if (sck_hz < SCK_MIN_HZ) {
            /* errata #1: MAC registers unreliable below 8 MHz on B1/B4; the
             * board limit above does not allow more. */
            LOG("eth: WARNING SPI2 SCK %lu Hz below 8 MHz on silicon %s (errata #1)",
                (unsigned long)sck_hz,
                info->rev_name);
        }
    } else if (revid != 0x05u && revid != 0x06u) {
        LOG("eth: WARNING EREVID %02x not in the errata sheet; all workarounds applied anyway",
            revid);
    }
    if (!phy_read(PHID1, &info->phid1) || !phy_read(PHID2, &info->phid2)
        || !phy_read(PHLCON, &info->phlcon)) {
        LOG("eth: MII read timed out (MISTAT.BUSY stuck)");
        return false;
    }
    if (info->phid1 != PHID1_VALUE || info->phid2 != PHID2_VALUE) {
        LOG("eth: PHID1 %04x PHID2 %04x, expected %04x %04x",
            info->phid1,
            info->phid2,
            PHID1_VALUE,
            PHID2_VALUE);
        return false;
    }

    /* Buffers (section 6.1-6.2, :1756-1799). Reception is off after reset
     * (ECON1 = 0, :762), as the pointers require (:980-983). */
    bfc(ECON1, ECON1_TXRTS | ECON1_RXEN);
    bfs(ECON2, ECON2_AUTOINC);
    wr16(ERXSTL, RXST);
    wr16(ERXNDL, RXND);
    wr16(ERXRDPTL, RXND); /* errata #14: odd, "next packet (RXST) - 1" wrapped */
    wr16(ETXSTL, TXST);
    wr16(ETXNDL, TXST);
    next_pkt = RXST;

    /* Receive filter, OR logic: unicast to our address, broadcast, and only
     * frames with a valid CRC (:2486-2542). This is also the reset value
     * (:822), written so the intent is explicit. Multicast stays off until
     * #13 needs it (IPv6 neighbour discovery, mDNS); see docs/hardware.md. */
    wr(ERXFCON, ERXFCON_UCEN | ERXFCON_CRCEN | ERXFCON_BCEN);

    /* MAC for half duplex (section 6.5, :1806-1834; section 9.1, :2875-2885).
     * MACON1: receive on, no pause frames (those are for full duplex,
     * :1811-1815). MACON3: pad to 60 bytes and append the CRC, frame length
     * check, FULDPX = 0. MACON4: DEFER for IEEE 802.3 (:1829-1831, :1927-1931).
     * MACLCON1/2 keep their reset values (:1824-1832). */
    wr(MACON1, MACON1_MARXEN);
    wr(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
    wr(MACON4, MACON4_DEFER);
    wr(MABBIPG, BBIPG_HALF);
    wr(MAIPGL, IPGL);
    wr(MAIPGH, IPGH_HALF);
    wr16(MAMXFLL, ENC_FRAME_MAX);
    wr(MAADR1, mac[0]);
    wr(MAADR2, mac[1]);
    wr(MAADR3, mac[2]);
    wr(MAADR4, mac[3]);
    wr(MAADR5, mac[4]);
    wr(MAADR6, mac[5]);

    /* PHY. PDPXMD = 0, half duplex, to match FULDPX = 0 (:1906-1908,
     * :1989-1991); its reset value depends on how an LED is wired to LEDB
     * (:498-507), which is unknown here and can be misdetected (errata #16,
     * errata.txt:331-344), so it is written. HDLDIS stops the PHY looping our
     * own transmissions back into the receive buffer (:2021-2026,
     * :2876-2882; errata #9, errata.txt:166-178). PLNKIE + PGEIE enable the link change
     * interrupt (:3552-3565); reading PHIR clears it (:3584-3591). PHLCON
     * (LEDs) stays at its reset value. */
    uint16_t phir = 0;
    if (!phy_write(PHCON1, 0) || !phy_write(PHCON2, PHCON2_HDLDIS)
        || !phy_write(PHIE, PHIE_PGEIE | PHIE_PLNKIE) || !phy_read(PHIR, &phir)) {
        LOG("eth: MII write timed out (MISTAT.BUSY stuck)");
        return false;
    }

    /* Read back everything written. */
    const struct check checks[] = {
        {ERXFCON, ERXFCON_UCEN | ERXFCON_CRCEN | ERXFCON_BCEN, "ERXFCON"},
        {MACON1, MACON1_MARXEN, "MACON1"},
        {MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN, "MACON3"},
        {MACON4, MACON4_DEFER, "MACON4"},
        {MABBIPG, BBIPG_HALF, "MABBIPG"},
        {MAIPGL, IPGL, "MAIPGL"},
        {MAIPGH, IPGH_HALF, "MAIPGH"},
        {MAADR1, mac[0], "MAADR1"},
        {MAADR2, mac[1], "MAADR2"},
        {MAADR3, mac[2], "MAADR3"},
        {MAADR4, mac[3], "MAADR4"},
        {MAADR5, mac[4], "MAADR5"},
        {MAADR6, mac[5], "MAADR6"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        uint8_t got = rd(checks[i].reg);
        if (got != checks[i].want) {
            LOG("eth: %s reads %02x, wrote %02x", checks[i].name, got, checks[i].want);
            ok = false;
        }
    }
    const struct {
        uint8_t     reg;
        uint16_t    want;
        const char *name;
    } checks16[] = {
        {ERXSTL, RXST, "ERXST"},
        {ERXNDL, RXND, "ERXND"},
        {ERXRDPTL, RXND, "ERXRDPT"},
        {ETXSTL, TXST, "ETXST"},
        {MAMXFLL, ENC_FRAME_MAX, "MAMXFL"},
    };
    for (size_t i = 0; i < sizeof checks16 / sizeof checks16[0]; i++) {
        uint16_t got = rd16(checks16[i].reg);
        if (got != checks16[i].want) {
            LOG("eth: %s reads %04x, wrote %04x", checks16[i].name, got, checks16[i].want);
            ok = false;
        }
    }
    uint16_t phcon2 = 0, phie = 0;
    if (!phy_read(PHCON1, &info->phcon1) || !phy_read(PHCON2, &phcon2) || !phy_read(PHIE, &phie)) {
        LOG("eth: MII read timed out (MISTAT.BUSY stuck)");
        return false;
    }
    /* Only the bits that matter; the rest are reserved (:3167-3199, :1993-2027). */
    if ((info->phcon1 & (PHCON1_PDPXMD | PHCON1_PLOOPBK | PHCON1_PPWRSV))
        || (phcon2 & (PHCON2_HDLDIS | PHCON2_FRCLNK | PHCON2_TXDIS)) != PHCON2_HDLDIS
        || (phie & (PHIE_PGEIE | PHIE_PLNKIE)) != (PHIE_PGEIE | PHIE_PLNKIE)) {
        LOG("eth: PHY read-back PHCON1 %04x PHCON2 %04x PHIE %04x", info->phcon1, phcon2, phie);
        ok = false;
    }
    if (!ok) {
        return false;
    }

    uint16_t st = 0;
    if (!phy_read(PHSTAT2, &st)) {
        return false;
    }
    link_last = (st & 0x0400u) != 0u; /* LSTAT (:1296-1298) */

    /* Interrupts: stale flags cleared first (:3231-3237), then reception on
     * (:2240-2248). PKTIE drives INT, but frames are counted with EPKTCNT
     * (errata #6). DMA is unused. */
    bfc(EIR, EIR_DMAIF | EIR_TXIF | EIR_TXERIF | EIR_RXERIF);
    wr(EIE, EIE_INTIE | EIE_PKTIE | EIE_LINKIE | EIE_TXIE | EIE_TXERIE | EIE_RXERIE);
    bfs(ECON1, ECON1_RXEN);
    int_enable(true);
    return true;
}

uint16_t enc28j60_phstat2(void) {
    uint16_t v = 0;
    (void)phy_read(PHSTAT2, &v);
    return v;
}

/* --- Service -------------------------------------------------------------- */
#define TX_LATE_RETRIES 16u   /* errata Example 1: retrycount 0 to 15 (errata.txt:275) */
#define TSV_LATECOL     0x20u /* TSV bit 29 = byte 3 bit 5 (Table 7-1, :2171) */

/* Start (or restart) the frame already in the transmit buffer: errata #12
 * and Example 1 (errata.txt:240-253, :266-270): TXRST pulse, clear TXERIF and
 * TXIF, then TXRTS. ETXST/ETXND and the frame survive a transmit-only reset
 * (:3117-3129, :2141-2142). */
static void tx_start(void) {
    bfs(ECON1, ECON1_TXRST);
    bfc(ECON1, ECON1_TXRST);
    bfc(EIR, EIR_TXIF | EIR_TXERIF);
    bfs(ECON1, ECON1_TXRTS);
}

/* Account for a finished transmission. Errata #13 (errata.txt:236-245):
 * finished means TXIF or TXERIF, not TXRTS clear, and TXRTS is then cleared
 * by software. Errata #15 (:293-303): a late collision is read from the
 * transmit status vector at ETXND + 1 (:2136-2151), not from ESTAT.LATECOL;
 * it is retransmitted up to TX_LATE_RETRIES times (Example 1, :275-289).
 * TXABRT and LATECOL are cleared for the next frame (:3485-3488). The status
 * vector is read through ERDPT, so no receive frame may be open. */
static void tx_harvest(void) {
    if (!tx_in_flight) {
        return;
    }
    uint8_t eir = rd(EIR);
    if (!(eir & (EIR_TXIF | EIR_TXERIF))) {
        return; /* still transmitting, deferring or backing off */
    }
    bfc(ECON1, ECON1_TXRTS);
    uint8_t estat = rd(ESTAT);
    uint8_t tsv[7];
    configASSERT(!rx_open);
    wr16(ERDPTL, (uint16_t)(TXST + tx_sent + 1u));
    rbm(tsv, sizeof tsv);
    bfc(ESTAT, ESTAT_TXABRT | ESTAT_LATECOL);

    if ((eir & EIR_TXERIF) && (tsv[3] & TSV_LATECOL) && tx_retries < TX_LATE_RETRIES) {
        tx_retries++;
        tx_done_retry++;
        tx_start();
        return;
    }
    tx_in_flight   = false;
    tx_last_estat  = estat;
    tx_last_status = (uint16_t)(tsv[2] | tsv[3] << 8);
    if ((eir & EIR_TXERIF) || (estat & ESTAT_TXABRT)) {
        tx_done_err++;
    } else {
        tx_done_ok++;
    }
    bfc(EIR, EIR_TXIF | EIR_TXERIF);
}

void enc28j60_poll(struct enc28j60_events *ev) {
    *ev = (struct enc28j60_events){0};
    bfc(EIE, EIE_INTIE);

    uint8_t eir = rd(EIR);
    if (eir & EIR_LINKIF) {
        uint16_t phir;
        (void)phy_read(PHIR, &phir); /* clears LINKIF (:3584-3591) */
    }
    if (eir & EIR_RXERIF) {
        ev->rx_overflow = true; /* :3456-3469 */
        bfc(EIR, EIR_RXERIF);
    }

    tx_harvest();
    ev->tx_ok     = tx_done_ok;
    ev->tx_err    = tx_done_err;
    ev->tx_retry  = tx_done_retry;
    ev->tx_estat  = tx_last_estat;
    ev->tx_status = tx_last_status;
    tx_done_ok    = 0;
    tx_done_err   = 0;
    tx_done_retry = 0;

    uint16_t st = 0;
    if (!phy_read(PHSTAT2, &st)) {
        ev->bus_error = true;
    }
    bool up          = (st & 0x0400u) != 0u; /* LSTAT (:1296-1298) */
    ev->link_irq     = (eir & EIR_LINKIF) != 0u;
    ev->link_changed = up != link_last;
    ev->link_up      = up;
    link_last        = up;

    ev->pending = rd(EPKTCNT); /* errata #6 */

    /* A chip that browned out or reset itself has lost RXEN and its pointers;
     * a bus fault changes EREVID. */
    if ((rd(EREVID) & 0x1Fu) != revid || !(rd(ECON1) & ECON1_RXEN)) {
        ev->bus_error = true;
    }
}

void enc28j60_rearm(void) {
    bfs(EIE, EIE_INTIE); /* a still-set flag gives a new falling edge (:3237-3241) */
}

/* --- Receive (section 7.2, :2230-2427) ------------------------------------ */
uint16_t enc28j60_rx_begin(uint32_t *rsv) {
    configASSERT(!rx_open);
    if (rd(EPKTCNT) == 0u) {
        return 0;
    }
    /* Header: next packet pointer, then the receive status vector, both
     * little-endian (Figure 7-3, :2258-2289; Table 7-3, :2295-2325). */
    uint8_t h[6];
    wr16(ERDPTL, next_pkt);
    rbm(h, sizeof h);
    uint16_t nxt = (uint16_t)(h[0] | h[1] << 8);
    uint32_t v = (uint32_t)h[2] | (uint32_t)h[3] << 8 | (uint32_t)h[4] << 16 | (uint32_t)h[5] << 24;
    uint16_t cnt = (uint16_t)v; /* whole frame incl. CRC */

    /* Consistency: frames shorter than 18 bytes never reach the buffer
     * (:1715-1725), longer than MAMXFL are aborted (:1899-1901), and the next
     * frame starts right after this one, padded to an even address
     * (:2252-2255). */
    uint32_t want = (uint32_t)next_pkt + 6u + cnt;
    want          = (want + 1u) & ~1u;
    if (want > RXND) {
        want -= RX_SIZE;
    }
    if (cnt < 18u || cnt > ENC_FRAME_MAX || nxt != want) {
        LOG("eth: receive buffer header inconsistent at %04x: next %04x (expected %04lx), "
            "status %08lx",
            next_pkt,
            nxt,
            (unsigned long)want,
            (unsigned long)v);
        return ENC_RX_CORRUPT;
    }
    rx_next = nxt;
    rx_left = (uint16_t)(cnt - 4u);
    rx_open = true;
    *rsv    = v;
    return rx_left;
}

void enc28j60_rx_read(void *buf, size_t n) {
    configASSERT(rx_open);
    if (n > rx_left) {
        n = rx_left;
    }
    rbm(buf, n);
    rx_left = (uint16_t)(rx_left - n);
}

void enc28j60_rx_end(void) {
    configASSERT(rx_open);
    /* errata #14: ERXRDPT = next - 1 (odd, since next is even), or ERXND when
     * next is ERXST. Low byte first (:2384-2392). Then PKTDEC (:2398-2416). */
    uint16_t rdpt = (rx_next == RXST) ? (uint16_t)RXND : (uint16_t)(rx_next - 1u);
    wr(ERXRDPTL, (uint8_t)rdpt);
    wr(ERXRDPTH, (uint8_t)(rdpt >> 8));
    bfs(ECON2, ECON2_PKTDEC);
    next_pkt = rx_next;
    rx_open  = false;
}

/* --- Transmit (section 7.1, :2041-2190) ----------------------------------- */
bool enc28j60_tx_begin(void) {
    configASSERT(!tx_open);
    TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        tx_harvest();
        if (!tx_in_flight) {
            break;
        }
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(ENC_TX_WAIT_MS)) {
            /* Cancel it (:2133-2134) and count it as an error; the new frame
             * is refused. The next tx_start() resets the logic (errata #12). */
            bfc(ECON1, ECON1_TXRTS);
            bfc(ESTAT, ESTAT_TXABRT | ESTAT_LATECOL);
            bfc(EIR, EIR_TXIF | EIR_TXERIF);
            tx_in_flight = false;
            tx_done_err++;
            return false;
        }
        vTaskDelay(1);
    }

    /* Per-packet control byte 0: MACON3 decides padding and CRC (:2054-2085). */
    static const uint8_t control = 0x00;
    wr16(EWRPTL, TXST);
    wbm(&control, 1);
    tx_len      = 0;
    tx_overflow = false;
    tx_open     = true;
    return true;
}

void enc28j60_tx_write(const void *buf, size_t n) {
    configASSERT(tx_open);
    if (tx_overflow || n > (size_t)(ENC_TX_MAX - tx_len)) {
        tx_overflow = true; /* never write past the transmit area */
        return;
    }
    wbm(buf, n);
    tx_len = (uint16_t)(tx_len + n);
}

bool enc28j60_tx_end(void) {
    configASSERT(tx_open);
    tx_open = false;
    if (tx_overflow || tx_len < ENC_ETH_HDR_LEN) {
        return false;
    }
    /* ETXND points at the last data byte (:2138-2141). */
    wr16(ETXSTL, TXST);
    wr16(ETXNDL, (uint16_t)(TXST + tx_len));
    tx_sent      = tx_len;
    tx_retries   = 0;
    tx_in_flight = true;
    tx_start();
    return true;
}
