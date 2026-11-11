/* Radio task, P2: receive only (#9), plus the flash ID check on the shared
 * SPI1 bus (#10). Flow follows docs/radio-runtime.md (freedo's working
 * receive path, freedo/firmware/cc1101_bridge/cc1101_bridge.ino:772-816).
 *
 * Interrupts (docs/design.md "Tasks": interrupts only timestamp and wake the
 * radio task):
 *   GDO0 (PA0, EXTI0, falling edge): IOCFG0 = 0x06 asserts on sync word and
 *     deasserts at the end of the packet, on a length-filtered packet, or when
 *     the RX FIFO overflows (cc110l.txt:2916-2918). So one edge covers end of
 *     frame and overflow, CRC good or bad.
 *   GDO2 (PA1, EXTI1, rising edge): IOCFG2 = 0x00 asserts when the RX FIFO
 *     reaches FIFOTHR = 16 bytes (cc110l.txt:2906-2907, 2611-2640), the only
 *     warning for a frame longer than the 64-byte FIFO.
 * A timeout pump (2 ms while a frame is being assembled, 100 ms otherwise)
 * covers a missed edge, as freedo's 8 ms tick does (radio-runtime.md). */
#include "radio.h"

#include "cc110l.h"
#include "log.h"
#include "mx25r.h"
#include "spi1.h"
#include "timebase.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "board.h"

/* --- Register table ---------------------------------------------------------
 * The stock IB01 burst write 0x00..0x2E from the logic capture
 * (docs/radio.md "Stock CC110L register table", wiki
 * CC110-Register-Configuration.md:43-95), with four changes:
 *   IOCFG2  0x04 -> 0x00  GDO2 = RX FIFO threshold, as freedo (cc1101.go:47)
 *   IOCFG0  0x01 -> 0x06  GDO0 = sync .. end of packet (see above)
 *   CHANNR  0x00/0x1A -> RADIO_RX_CHANNEL (26 = 0x1A, as stock ends up)
 *   FSCTRL1 0x18 -> 0x06  IF 152 kHz, owner decision (docs/radio.md Conflicts)
 * IOCFG1 stays 0x2E: GDO1 is the SO pin and must be 3-state while CSn is
 * high, because the flash shares MISO (cc110l.txt:2891-2893, 2901). */
#define PKTLEN_MAX 0x80u /* variable length: max length byte (cc110l.txt:3607-3612) */
#define MCSM1_VAL  0x3Fu /* RXOFF_MODE 11 stay in RX, TXOFF_MODE 11 RX, CCA_MODE 11 */
#define PKTCTRL0_VAL 0x05u

static const uint8_t cc_config[CC_NUM_CONFIG_REGS] = {
    [CC_IOCFG2] = 0x00,   [CC_IOCFG1] = 0x2E,   [CC_IOCFG0] = 0x06,   [CC_FIFOTHR] = 0x03,
    [CC_SYNC1] = 0xD3,    [CC_SYNC0] = 0x91,    [CC_PKTLEN] = PKTLEN_MAX,
    [CC_PKTCTRL1] = 0x04, /* APPEND_STATUS, no address check, no autoflush */
    [CC_PKTCTRL0] = PKTCTRL0_VAL, /* FIFO mode, CRC on, variable length, no whitening */
    [CC_ADDR] = 0x00,     [CC_CHANNR] = RADIO_RX_CHANNEL,
    [CC_FSCTRL1] = 0x06,  [CC_FSCTRL0] = 0x00,  [CC_FREQ2] = 0x21,    [CC_FREQ1] = 0x32,
    [CC_FREQ0] = 0x76,    [CC_MDMCFG4] = 0xAA,  [CC_MDMCFG3] = 0xF8,  [CC_MDMCFG2] = 0x13,
    [CC_MDMCFG1] = 0x22,  [CC_MDMCFG0] = 0xF8,  [CC_DEVIATN] = 0x40,  [CC_MCSM2] = 0x07,
    [CC_MCSM1] = MCSM1_VAL,    [CC_MCSM0] = 0x08,    [CC_FOCCFG] = 0x16,   [CC_BSCFG] = 0x6C,
    [CC_AGCCTRL2] = 0x43, [CC_AGCCTRL1] = 0x40, [CC_AGCCTRL0] = 0x91,
    [0x1E] = 0x87,        [0x1F] = 0x6B, /* "Not used" (cc110l.txt:3335); stock's dummy bytes */
    [CC_RSVD_20] = 0xFB,  [CC_FREND1] = 0x56,   [CC_FREND0] = 0x10,   [CC_FSCAL3] = 0xE9,
    [CC_FSCAL2] = 0x2A,   [CC_FSCAL1] = 0x00,   [CC_FSCAL0] = 0x1F,
    [0x27] = 0x41,        [0x28] = 0x00, /* "Not used" (cc110l.txt:3369) */
    [0x29] = 0x59,        [0x2A] = 0x7F,        [0x2B] = 0x3F, /* reserved, = reset values */
    [CC_TEST2] = 0x81,    [CC_TEST1] = 0x35,    [CC_TEST0] = 0x09,
};

/* Receive only: MCSM1 RXOFF_MODE = 11 (stay in RX after a packet) and
 * TXOFF_MODE = 11 (cc110l.txt:3929-3955). The runtime guard in cc110l.c
 * refuses any MCSM1 that leads to TX or FSTXON; this catches a bad table at
 * build time. */
_Static_assert(((MCSM1_VAL >> 2) & 3) == 3 && (MCSM1_VAL & 3) == 3, "MCSM1 must stay in RX");
_Static_assert(((PKTCTRL0_VAL >> 4) & 3) == 0, "PKTCTRL0 FIFO mode (no serial or random TX mode)");
_Static_assert(RADIO_RX_CHANNEL <= 49, "channel plan 0-49 (docs/radio.md)");

/* Frequency plan checks, all from the table above with fXOSC = 26 MHz:
 *   carrier = fXOSC / 2^16 x FREQ (cc110l.txt:3729-3736)
 *   spacing = fXOSC / 2^18 x (256 + CHANSPC_M) x 2^CHANSPC_E (cc110l.txt:3869-3880)
 *   IF      = fXOSC / 2^10 x FREQ_IF (cc110l.txt:3700-3711)
 *   rate    = (256 + DRATE_M) x 2^DRATE_E / 2^28 x fXOSC (cc110l.txt:3768-3778)
 *   BW      = fXOSC / (8 x (4 + CHANBW_M) x 2^CHANBW_E) (cc110l.txt:3755-3763)
 *   dev     = fXOSC / 2^17 x (8 + DEVIATION_M) x 2^DEVIATION_E (cc110l.txt:3882-3906) */
#define FXOSC_HZ 26000000ull
#define FREQ_WORD 0x213276ull
#define CH_SPACING_X2_18 (FXOSC_HZ * (256u + 0xF8u) * (1u << (0x22 & 3)))
#define CARRIER_HZ(ch) ((FXOSC_HZ * FREQ_WORD) / 65536u + ((ch) * CH_SPACING_X2_18) / 262144u)
_Static_assert(CARRIER_HZ(0) > 863124000ull && CARRIER_HZ(0) < 863126000ull, "base 863.125 MHz");
_Static_assert(CH_SPACING_X2_18 / 262144u == 199951u, "spacing 199.951 kHz");
_Static_assert(CARRIER_HZ(26) > 868322700ull && CARRIER_HZ(26) < 868324700ull, "ch 26 = 868.3237 MHz");
_Static_assert(CARRIER_HZ(10) > 865123500ull && CARRIER_HZ(10) < 865125500ull, "ch 10 = 865.1245 MHz");
_Static_assert(FXOSC_HZ * 6u / 1024u == 152343u, "IF 152 kHz with FSCTRL1 = 0x06");
_Static_assert((256u + 0xF8u) * (1ull << 10) * FXOSC_HZ / (1ull << 28) == 49987u, "49.99 kBaud");
_Static_assert(FXOSC_HZ / (8u * (4u + 2u) * 4u) == 135416u, "RX BW 135 kHz");
_Static_assert(FXOSC_HZ * 8u * 16u / (1u << 17) == 25390u, "deviation 25.39 kHz");

/* RSSI_dBm = RSSI_dec(signed) / 2 - RSSI_offset, offset 74 dB at 868 MHz
 * (cc110l.txt:2130-2144). Kept in half dB. */
#define RSSI_OFFSET_DB 74
/* FREQEST resolution fXOSC / 2^14 = 1586.9 Hz = 203125 / 128 Hz
 * (cc110l.txt:4429-4434). */
#define FREQEST_HZ(fe) ((int32_t)(int8_t)(fe) * 203125 / 128)

/* --- Interrupts -------------------------------------------------------------- */
#define NOTE_GDO0  (1u << 0)
#define NOTE_GDO2  (1u << 1)
#define NOTE_FLASH (1u << 2)

/* FreeRTOS FromISR calls need a numeric priority >= 5 (FreeRTOSConfig.h,
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50). */
#define GDO_IRQ_PRIO (5u << 4)
_Static_assert(GDO_IRQ_PRIO >= configMAX_SYSCALL_INTERRUPT_PRIORITY, "FromISR needs prio >= 5");

static TaskHandle_t self;

static void notify_from_isr(uint32_t bits) {
    BaseType_t woken = pdFALSE;
    if (self) {
        xTaskNotifyFromISR(self, bits, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

void exti0_isr(void) {
    exti_reset_request(EXTI0);
    notify_from_isr(NOTE_GDO0);
}

void exti1_isr(void) {
    exti_reset_request(EXTI1);
    notify_from_isr(NOTE_GDO2);
}

static void gdo_irq_enable(bool on) {
    if (on) {
        exti_reset_request(EXTI0 | EXTI1);
        nvic_clear_pending_irq(NVIC_EXTI0_IRQ);
        nvic_clear_pending_irq(NVIC_EXTI1_IRQ);
        exti_enable_request(EXTI0 | EXTI1);
    } else {
        /* After SRES GDO0 outputs a 135 kHz clock (cc110l.txt:2894-2896,
         * freedo .ino:143-146): keep the interrupt off until configured. */
        exti_disable_request(EXTI0 | EXTI1);
    }
}

static void gdo_irq_setup(void) {
    rcc_periph_clock_enable(RCC_SYSCFG); /* rm0383.txt:6122 */
    exti_disable_request(EXTI0 | EXTI1);
    exti_select_source(EXTI0 | EXTI1, GPIOA); /* SYSCFG_EXTICR1, rm0383.txt:11032-11050 */
    exti_set_trigger(EXTI0, EXTI_TRIGGER_FALLING);
    exti_set_trigger(EXTI1, EXTI_TRIGGER_RISING);
    nvic_set_priority(NVIC_EXTI0_IRQ, GDO_IRQ_PRIO);
    nvic_set_priority(NVIC_EXTI1_IRQ, GDO_IRQ_PRIO);
    nvic_enable_irq(NVIC_EXTI0_IRQ);
    nvic_enable_irq(NVIC_EXTI1_IRQ);
}

void radio_request_flash_check(void) {
    if (self) {
        xTaskNotify(self, NOTE_FLASH, eSetBits);
    }
}

/* --- Counters ---------------------------------------------------------------- */
struct counters {
    uint32_t frames, crc_ok, crc_bad, wake, overflow, bad_len, stale, rearm, reinit, spi_err;
    uint32_t flash_ok, flash_bad, flash_in_frame, status_glitch;
};
static struct counters minute, total;

#define COUNT(field) \
    do {             \
        minute.field++; \
        total.field++;  \
    } while (0)

/* --- Formatting helpers ------------------------------------------------------- */
static const char *half_db(char *b, size_t sz, int x2) {
    unsigned a = (unsigned)(x2 < 0 ? -x2 : x2);
    snprintf(b, sz, "%s%u.%u", x2 < 0 ? "-" : "", a / 2, (a & 1) ? 5u : 0u);
    return b;
}

static int rssi_x2(uint8_t raw) {
    return (int)(int8_t)raw - 2 * RSSI_OFFSET_DB;
}

/* --- Wake-up train summary ----------------------------------------------------
 * Wake-up frames (802.15.4 multipurpose, frame type 5 in FCF bits 2:0,
 * docs/radio.md "Frames") come in trains of up to 254 frames 4 ms apart
 * (docs/radio.md "Coordinator behaviour"). One log line per train instead of
 * one per frame keeps the RTT log readable. */
#define WAKE_GAP_US 30000u

static struct {
    bool     active;
    uint8_t  seq;
    uint16_t n, n_ok;
    int      rssi_min, rssi_max; /* half dB */
    int32_t  fe_sum;
    uint32_t last_us;
} wake;

static void wake_flush(void) {
    if (!wake.active) {
        return;
    }
    char a[12], b[12];
    int  fe_avg = (int)(wake.fe_sum / wake.n);
    LOG("rx: wake train seq %02x: %u frames (%u crc ok), rssi %s..%s dBm, freqest avg %d (%ld Hz)",
        wake.seq, wake.n, wake.n_ok, half_db(a, sizeof a, wake.rssi_min),
        half_db(b, sizeof b, wake.rssi_max), fe_avg, (long)(fe_avg * 203125 / 128));
    wake.active = false;
}

static void wake_add(uint8_t seq, bool crc_ok, int rssi, int8_t fe) {
    if (wake.active && seq != wake.seq) {
        wake_flush();
    }
    if (!wake.active) {
        wake.active   = true;
        wake.seq      = seq;
        wake.n        = 0;
        wake.n_ok     = 0;
        wake.rssi_min = rssi;
        wake.rssi_max = rssi;
        wake.fe_sum   = 0;
    }
    wake.n++;
    wake.n_ok += crc_ok;
    wake.rssi_min = rssi < wake.rssi_min ? rssi : wake.rssi_min;
    wake.rssi_max = rssi > wake.rssi_max ? rssi : wake.rssi_max;
    wake.fe_sum += fe;
    wake.last_us = timebase_us();
}

/* --- Receive ----------------------------------------------------------------- */
#define RX_BUF_MAX  (1u + PKTLEN_MAX + 2u) /* length byte, payload, RSSI, CRC_OK */
#define RX_STALE_US 60000u                 /* longest frame 131 B x 160 us = 21 ms; freedo uses 60 ms */

static struct {
    uint8_t  buf[RX_BUF_MAX];
    unsigned got, total;
    bool     assembling;
    uint32_t start_us;
} rx;

static bool wait_marcstate(uint8_t want, uint32_t timeout_us) {
    uint32_t start = timebase_us();
    uint8_t  m;
    do {
        if (cc110l_read_status_stable(CC_MARCSTATE, &m) && (m & 0x1F) == want) {
            return true;
        }
    } while ((uint32_t)(timebase_us() - start) < timeout_us);
    return false;
}

/* SIDLE, then SFRX once IDLE is reached (SFRX only in IDLE or overflow,
 * cc110l.txt:3252-3253, 1501; SIDLE drops strobes sent before IDLE,
 * cc110l.txt:1449-1451), then SRX. RX to IDLE takes ~0.1 us, IDLE to RX
 * without calibration 75 us (cc110l.txt:2538-2549). */
static bool rx_flush_and_arm(void) {
    rx.assembling = false;
    bool ok = radio_strobe(CC_SIDLE, NULL) && wait_marcstate(CC_MARC_IDLE, 1000)
              && radio_strobe(CC_SFRX, NULL) && radio_strobe(CC_SRX, NULL);
    if (!ok) {
        COUNT(spi_err);
    }
    return ok;
}

static void frame_done(uint8_t fe) {
    unsigned len    = rx.buf[0];
    const uint8_t *mac = &rx.buf[1];
    uint8_t  rssi_raw = rx.buf[1 + len];
    uint8_t  st2      = rx.buf[2 + len]; /* bit 7 CRC_OK, bits 6:0 reserved (cc110l.txt:1747-1757) */
    bool     crc_ok   = (st2 & 0x80) != 0;
    int      rssi     = rssi_x2(rssi_raw);

    COUNT(frames);
    if (crc_ok) {
        static bool cloud;
        COUNT(crc_ok);
        cloud = !cloud;
        board_led(LED_CLOUD, cloud); /* P2 test wiring: cloud LED toggles per CRC-OK frame */
    } else {
        COUNT(crc_bad);
    }

    if ((mac[0] & 0x07) == 5 && len >= 2) { /* wake-up: 25 <seq> CD AB ... */
        COUNT(wake);
        /* A bad-CRC frame's seq byte may be corrupt: count it in the open train. */
        wake_add((crc_ok || !wake.active) ? mac[1] : wake.seq, crc_ok, rssi, (int8_t)fe);
        return;
    }
    wake_flush();

    /* Header only: frame control and sequence number. No addresses or
     * payload (they identify the owner's devices). */
    char r[12];
    LOG("rx: t %lu len %u crc %s rssi %s dBm lqi(rsvd) %u freqest %d (%ld Hz) hdr %02x %02x %02x",
        (unsigned long)xTaskGetTickCount(), len, crc_ok ? "ok" : "BAD", half_db(r, sizeof r, rssi),
        st2 & 0x7F, (int)(int8_t)fe, (long)FREQEST_HZ(fe), mac[0], len > 1 ? mac[1] : 0,
        len > 2 ? mac[2] : 0);
}

/* Drain the RX FIFO (radio-runtime.md "Receive", freedo .ino:772-816). Never
 * empties the FIFO while a packet is still arriving: a read of the last byte
 * as a new one is written duplicates it (cc110l.txt:2593-2597), so one byte
 * stays until the rest of the packet is in. */
static void rx_pump(void) {
    if (rx.assembling && (uint32_t)(timebase_us() - rx.start_us) > RX_STALE_US) {
        COUNT(stale);
        rx_flush_and_arm();
        return;
    }
    for (int guard = 0; guard < 16; guard++) {
        uint8_t rb;
        if (!cc110l_read_status_stable(CC_RXBYTES, &rb)) {
            COUNT(spi_err);
            return;
        }
        if (rb & CC_RXBYTES_OVERFLOW) {
            COUNT(overflow);
            rx_flush_and_arm();
            return;
        }
        unsigned n = rb & CC_RXBYTES_NUM;
        if (n > CC_FIFO_SIZE) {
            continue; /* a corrupt read (cc110l.txt:1436-1441); read again */
        }
        if (!rx.assembling) {
            if (n < 2) {
                return;
            }
            if (!cc110l_read_rxfifo(rx.buf, 1)) {
                COUNT(spi_err);
                return;
            }
            unsigned len = rx.buf[0];
            if (len == 0 || len > PKTLEN_MAX) {
                COUNT(bad_len);
                rx_flush_and_arm();
                return;
            }
            rx.got        = 1;
            rx.total      = 1 + len + 2;
            rx.assembling = true;
            rx.start_us   = timebase_us();
            continue;
        }
        unsigned remaining = rx.total - rx.got;
        unsigned to_read   = 0;
        if (n >= remaining) {
            to_read = remaining;
        } else if (n > 1) {
            to_read = n - 1;
        }
        if (to_read == 0) {
            return;
        }
        if (!cc110l_read_rxfifo(rx.buf + rx.got, to_read)) {
            COUNT(spi_err);
            return;
        }
        rx.got += to_read;
        if (rx.got >= rx.total) {
            /* FREQEST now, before any strobe moves the demodulator off this
             * packet (docs/radio-runtime.md, freedo README.md:49-53). */
            uint8_t fe = 0;
            (void)cc110l_read_status(CC_FREQEST, &fe);
            rx.assembling = false;
            frame_done(fe);
        }
    }
}

/* --- Bring-up ---------------------------------------------------------------- */
static bool reg_is_unused(unsigned a) {
    return a == 0x1E || a == 0x1F || a == 0x27 || a == 0x28;
}

static bool radio_configure(void) {
    gdo_irq_enable(false);
    rx.assembling = false;

    if (!cc110l_reset()) {
        LOG("radio: no CHIP_RDYn after SRES (SO stayed high)");
        return false;
    }
    uint8_t part = 0xFF, ver = 0xFF;
    if (!cc110l_read_status(CC_PARTNUM, &part) || !cc110l_read_status(CC_VERSION, &ver)) {
        LOG("radio: PARTNUM/VERSION read failed");
        return false;
    }
    /* CC110L: PARTNUM 0x00, VERSION 0x17 "subject to change" (cc110l.txt:4419-4426). */
    LOG("radio: PARTNUM %02x VERSION %02x%s", part, ver,
        (part == 0x00 && ver == 0x17) ? " (CC110L as documented)" : " (not the documented 00/17)");
    if (ver == 0x00 || ver == 0xFF) {
        LOG("radio: VERSION %02x looks like a dead bus; not configuring", ver);
        return false;
    }

    if (!cc110l_write_burst(0x00, cc_config, CC_NUM_CONFIG_REGS)) {
        LOG("radio: register burst write failed");
        return false;
    }
    uint8_t back[CC_NUM_CONFIG_REGS];
    if (!cc110l_read_burst(0x00, back, sizeof back)) {
        LOG("radio: register read-back failed");
        return false;
    }
    unsigned bad = 0;
    for (unsigned a = 0; a < CC_NUM_CONFIG_REGS; a++) {
        if (!reg_is_unused(a) && back[a] != cc_config[a]) {
            LOG("radio: reg %02x wrote %02x read %02x", a, cc_config[a], back[a]);
            bad++;
        }
    }
    if (bad) {
        LOG("radio: %u registers did not verify; staying idle", bad);
        return false;
    }

    /* Manual calibration (MCSM0.FS_AUTOCAL = 0 in the table): SCAL from IDLE,
     * about 735 us (cc110l.txt:2550, 2560-2567). FSCAL1 = 0x3F after
     * calibration means the PLL did not lock (cc110l.txt:2756-2759). */
    if (!radio_strobe(CC_SCAL, NULL)) {
        LOG("radio: SCAL failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!wait_marcstate(CC_MARC_IDLE, 10000)) {
        LOG("radio: calibration did not return to IDLE");
        return false;
    }
    uint8_t fscal[3] = {0};
    (void)cc110l_read_burst(CC_FSCAL3, fscal, 3);
    LOG("radio: calibrated, FSCAL3 %02x FSCAL2 %02x FSCAL1 %02x%s", fscal[0], fscal[1], fscal[2],
        fscal[2] == 0x3F ? " (PLL NOT locked)" : "");

    if (!radio_strobe(CC_SFRX, NULL) || !radio_strobe(CC_SRX, NULL)
        || !wait_marcstate(CC_MARC_RX, 2000)) {
        LOG("radio: did not enter RX");
        return false;
    }
    gdo_irq_enable(true);
    LOG("radio: RX on channel %u (%lu Hz), receive only", RADIO_RX_CHANNEL,
        (unsigned long)CARRIER_HZ(RADIO_RX_CHANNEL));
    return true;
}

/* Every 10 s: the chip must still hold our configuration and be in RX. A
 * brown-out or reset of the radio leaves reset defaults (IOCFG0 = 0x3F). */
static void health_check(void) {
    uint8_t io0 = 0, m = 0;
    if (!cc110l_read_reg(CC_IOCFG0, &io0) || io0 != cc_config[CC_IOCFG0]) {
        LOG("radio: IOCFG0 reads %02x, configuration lost; re-initialising", io0);
        COUNT(reinit);
        (void)radio_configure();
        return;
    }
    if (!cc110l_read_status_stable(CC_MARCSTATE, &m)) {
        COUNT(spi_err);
        return;
    }
    m &= 0x1F;
    if (m >= CC_MARC_FSTXON && m <= CC_MARC_TXFIFO_UNF) {
        (void)radio_strobe(CC_SIDLE, NULL);
        fw_panic("radio: MARCSTATE in a TX state on a receive-only image", __FILE__, __LINE__);
    }
    if (m == CC_MARC_IDLE) {
        LOG("radio: found IDLE, re-arming RX");
        COUNT(rearm);
        rx_flush_and_arm();
    }
}

/* A status byte claimed TX/FSTXON: confirm with MARCSTATE (one status byte
 * can be corrupt, cc110l.txt:1994-1997). */
static void check_tx_state_flag(void) {
    if (!cc110l_take_tx_state_flag()) {
        return;
    }
    uint8_t m = 0;
    if (cc110l_read_status_stable(CC_MARCSTATE, &m)) {
        m &= 0x1F;
        if (m >= CC_MARC_FSTXON && m <= CC_MARC_TXFIFO_UNF) {
            (void)radio_strobe(CC_SIDLE, NULL);
            fw_panic("radio: chip in a TX state on a receive-only image", __FILE__, __LINE__);
        }
    }
    COUNT(status_glitch);
}

/* --- Flash on the shared bus (#10) ----------------------------------------- */
static void flash_boot(void) {
    bool    awake = mx25r_wake();
    uint32_t id   = 0;
    uint8_t sr    = 0;
    bool    ok    = mx25r_read_id(&id) && mx25r_read_status(&sr);
    LOG("flash: JEDEC ID %06lx%s, status register %02x%s", (unsigned long)id,
        id == MX25R_JEDEC_ID ? " (MX25R1635F)" : " (UNEXPECTED, want c22815)", sr,
        ok && awake ? "" : " (wake or read failed)");
}

static void flash_check(void) {
    bool     in_frame = rx.assembling || gpio_get(GPIOA, BOARD_GDO0_PIN);
    uint32_t id       = 0;
    if (mx25r_read_id(&id) && id == MX25R_JEDEC_ID) {
        COUNT(flash_ok);
    } else {
        COUNT(flash_bad);
        LOG("flash: JEDEC ID mismatch: %06lx", (unsigned long)id);
    }
    if (in_frame) {
        COUNT(flash_in_frame);
    }
}

static void log_minute(void) {
    LOG("radio: 60 s: frames %lu (crc ok %lu, bad %lu, wake %lu), overflow %lu, bad len %lu, stale %lu, "
        "rearm %lu, reinit %lu, spi err %lu, status glitch %lu",
        (unsigned long)minute.frames, (unsigned long)minute.crc_ok, (unsigned long)minute.crc_bad,
        (unsigned long)minute.wake, (unsigned long)minute.overflow, (unsigned long)minute.bad_len,
        (unsigned long)minute.stale, (unsigned long)minute.rearm, (unsigned long)minute.reinit,
        (unsigned long)minute.spi_err, (unsigned long)minute.status_glitch);
    LOG("flash: 60 s: JEDEC ID ok %lu, mismatch %lu (%lu during a frame); total frames %lu, "
        "flash mismatches %lu",
        (unsigned long)minute.flash_ok, (unsigned long)minute.flash_bad,
        (unsigned long)minute.flash_in_frame, (unsigned long)total.frames,
        (unsigned long)total.flash_bad);
    minute = (struct counters){0};
}

#define HEALTH_PERIOD_MS 10000u
#define STATS_PERIOD_MS  60000u
#define RETRY_PERIOD_MS  10000u

void radio_task_run(void) {
    self = xTaskGetCurrentTaskHandle();
    LOG("spi1: SCK %lu Hz, mode 0", (unsigned long)spi1_sck_hz());

    watchdog_checkin(WDG_RADIO);
    flash_boot();
    gdo_irq_setup();

    while (!radio_configure()) {
        LOG("radio: bring-up failed; retrying in %u s", RETRY_PERIOD_MS / 1000);
        for (unsigned ms = 0; ms < RETRY_PERIOD_MS; ms += 100) {
            watchdog_checkin(WDG_RADIO);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    TickType_t next_health = xTaskGetTickCount() + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
    TickType_t next_stats  = xTaskGetTickCount() + pdMS_TO_TICKS(STATS_PERIOD_MS);
    for (;;) {
        uint32_t bits = 0;
        /* Block with a timeout, never forever, so the watchdog check-in keeps
         * coming while the channel is quiet. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits,
                              pdMS_TO_TICKS(rx.assembling ? 2 : 100));
        watchdog_checkin(WDG_RADIO);

        rx_pump();
        check_tx_state_flag();
        if (wake.active && (uint32_t)(timebase_us() - wake.last_us) > WAKE_GAP_US) {
            wake_flush();
        }
        if (bits & NOTE_FLASH) {
            flash_check();
        }

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_health) >= 0) {
            next_health = now + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
            health_check();
        }
        if ((int32_t)(now - next_stats) >= 0) {
            next_stats = now + pdMS_TO_TICKS(STATS_PERIOD_MS);
            wake_flush();
            log_minute();
        }
    }
}
