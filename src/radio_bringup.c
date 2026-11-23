/* Radio task, bring-up: the CC110L register table and its build-time
 * checks, reset and configuration, calibration, the health check, and the
 * chip-state checks every part uses (the TX-state watch, see the top of
 * src/radio.c). Part of the radio task (src/radio.c); the state it shares,
 * src/radio_internal.h. */
#include "radio.h"

#include "radio_internal.h"

#include "cc110l.h"
#include "log.h"
#include "phy.h"
#include "timebase.h"
#include "txgate.h"

#include <stdbool.h>
#include <stdint.h>

/* Also refused at run time by the transmit gate, whatever these say. The
 * protected channel only in a channel-26 build with the opt-in (P7.1,
 * src/txgate.h). */
_Static_assert(RADIO_CHANNEL != TXGATE_PROTECTED_CHANNEL || TXGATE_NET_ON_PROTECTED,
               "the bridge's channel is the protected one");
_Static_assert(RADIO_CHANNEL == TXGATE_TEST_CHANNEL, "transmit only on the test channel for now");

/* --- Register table ---------------------------------------------------------
 * The stock IB01 burst write 0x00..0x2E from the logic capture
 * (docs/radio.md "Stock CC110L register table", wiki
 * CC110-Register-Configuration.md:43-95), with four changes:
 *   IOCFG2  0x04 -> 0x07  GDO2 = packet received with CRC OK (src/radio.c, top)
 *   IOCFG0  0x01 -> 0x06  GDO0 = sync .. end of packet (src/radio.c, top)
 *   CHANNR  0x00/0x1A -> RADIO_CHANNEL (stock ends up on 26 = 0x1A)
 *   FSCTRL1 0x18 -> 0x06  IF 152 kHz, owner decision (docs/radio.md Conflicts)
 * IOCFG1 stays 0x2E: GDO1 is the SO pin and must be 3-state while CSn is
 * high, because the flash shares MISO (cc110l.txt:2891-2893, 2901). */
#define MCSM1_VAL    0x3Fu /* RXOFF_MODE 11 stay in RX, TXOFF_MODE 11 RX, CCA_MODE 11 */
#define PKTCTRL0_VAL 0x05u
/* The registers that the frequency plan checks below read. */
#define FSCTRL1_VAL 0x06u /* FREQ_IF in bits 4:0 */
#define FREQ2_VAL   0x21u
#define FREQ1_VAL   0x32u
#define FREQ0_VAL   0x76u
#define MDMCFG4_VAL 0xAAu /* CHANBW_E 7:6, CHANBW_M 5:4, DRATE_E 3:0 */
#define MDMCFG3_VAL 0xF8u /* DRATE_M */
#define MDMCFG1_VAL 0x22u /* CHANSPC_E in bits 1:0 */
#define MDMCFG0_VAL 0xF8u /* CHANSPC_M */
#define DEVIATN_VAL 0x40u /* DEVIATION_E 6:4, DEVIATION_M 2:0 */

static const uint8_t cc_config[CC_NUM_CONFIG_REGS] = {
    [CC_IOCFG2]   = 0x07,
    [CC_IOCFG1]   = 0x2E,
    [CC_IOCFG0]   = 0x06,
    [CC_FIFOTHR]  = 0x03,
    [CC_SYNC1]    = 0xD3,
    [CC_SYNC0]    = 0x91,
    [CC_PKTLEN]   = PKTLEN_MAX,
    [CC_PKTCTRL1] = 0x04,         /* APPEND_STATUS, no address check, no autoflush */
    [CC_PKTCTRL0] = PKTCTRL0_VAL, /* FIFO mode, CRC on, variable length, no whitening */
    [CC_ADDR]     = 0x00,
    [CC_CHANNR]   = RADIO_CHANNEL,
    [CC_FSCTRL1]  = FSCTRL1_VAL,
    [CC_FSCTRL0]  = 0x00,
    [CC_FREQ2]    = FREQ2_VAL,
    [CC_FREQ1]    = FREQ1_VAL,
    [CC_FREQ0]    = FREQ0_VAL,
    [CC_MDMCFG4]  = MDMCFG4_VAL,
    [CC_MDMCFG3]  = MDMCFG3_VAL,
    [CC_MDMCFG2]  = 0x13,
    [CC_MDMCFG1]  = MDMCFG1_VAL,
    [CC_MDMCFG0]  = MDMCFG0_VAL,
    [CC_DEVIATN]  = DEVIATN_VAL,
    [CC_MCSM2]    = 0x07,
    [CC_MCSM1]    = MCSM1_VAL,
    [CC_MCSM0]    = 0x08,
    [CC_FOCCFG]   = 0x16,
    [CC_BSCFG]    = 0x6C,
    [CC_AGCCTRL2] = 0x43,
    [CC_AGCCTRL1] = 0x40,
    [CC_AGCCTRL0] = 0x91,
    [0x1E]        = 0x87,
    [0x1F]        = 0x6B, /* "Not used" (cc110l.txt:3335); stock's dummy bytes */
    [CC_RSVD_20]  = 0xFB,
    [CC_FREND1]   = 0x56,
    [CC_FREND0]   = 0x10,
    [CC_FSCAL3]   = 0xE9,
    [CC_FSCAL2]   = 0x2A,
    [CC_FSCAL1]   = 0x00,
    [CC_FSCAL0]   = 0x1F,
    [0x27]        = 0x41,
    [0x28]        = 0x00, /* "Not used" (cc110l.txt:3369) */
    [0x29]        = 0x59,
    [0x2A]        = 0x7F,
    [0x2B]        = 0x3F, /* reserved, = reset values */
    [CC_TEST2]    = 0x81,
    [CC_TEST1]    = 0x35,
    [CC_TEST0]    = 0x09,
};

/* MCSM1 RXOFF_MODE = 11 (stay in RX after a packet) and TXOFF_MODE = 11
 * (back to RX after a frame of ours, cc110l.txt:3929-3955): the chip enters
 * TX only on an STX strobe. The runtime guard in cc110l.c refuses any MCSM1
 * that leads to TX or FSTXON by itself; this catches a bad table at build
 * time. CCA_MODE = 11 has no effect here: every frame is sent from IDLE. */
_Static_assert(((MCSM1_VAL >> 2) & 3) == 3 && (MCSM1_VAL & 3) == 3, "MCSM1 must stay in RX");
_Static_assert(((PKTCTRL0_VAL >> 4) & 3) == 0, "PKTCTRL0 FIFO mode (no serial or random TX mode)");
_Static_assert(RADIO_CHANNEL <= 49, "channel plan 0-49 (docs/radio.md)");

/* Frequency plan checks, all from the table above with fXOSC = 26 MHz
 * (PHY_FXOSC_HZ, src/phy.h):
 *   carrier = fXOSC / 2^16 x FREQ (cc110l.txt:3729-3736)
 *   spacing = fXOSC / 2^18 x (256 + CHANSPC_M) x 2^CHANSPC_E (cc110l.txt:3869-3880)
 *   IF      = fXOSC / 2^10 x FREQ_IF (cc110l.txt:3700-3711)
 *   rate    = (256 + DRATE_M) x 2^DRATE_E / 2^28 x fXOSC (cc110l.txt:3768-3778)
 *   BW      = fXOSC / (8 x (4 + CHANBW_M) x 2^CHANBW_E) (cc110l.txt:3755-3763)
 *   dev     = fXOSC / 2^17 x (8 + DEVIATION_M) x 2^DEVIATION_E (cc110l.txt:3882-3906) */
#define FREQ_WORD        (FREQ2_VAL * 65536ull + FREQ1_VAL * 256u + FREQ0_VAL)
#define CH_SPACING_X2_18 (PHY_FXOSC_HZ * (256u + MDMCFG0_VAL) * (1u << (MDMCFG1_VAL & 3u)))
#define CARRIER_HZ(ch)   ((PHY_FXOSC_HZ * FREQ_WORD) / 65536u + ((ch) * CH_SPACING_X2_18) / 262144u)
_Static_assert(CARRIER_HZ(0) > 863124000ull && CARRIER_HZ(0) < 863126000ull, "base 863.125 MHz");
_Static_assert(CH_SPACING_X2_18 / 262144u == 199951u, "spacing 199.951 kHz");
_Static_assert(FREQ_WORD == PHY_FREQ_WORD && (MDMCFG1_VAL & 3u) == PHY_CHANSPC_E
                   && MDMCFG0_VAL == PHY_CHANSPC_M,
               "the table programs the channel plan of src/phy.h");
_Static_assert(CARRIER_HZ(26) > 868322700ull && CARRIER_HZ(26) < 868324700ull,
               "ch 26 = 868.3237 MHz");
_Static_assert(CARRIER_HZ(16) > 866323500ull && CARRIER_HZ(16) < 866325000ull,
               "ch 16 = 866.3242 MHz");
_Static_assert(CARRIER_HZ(RADIO_CHANNEL) + TXGATE_MIN_DIST_HZ
                       <= CARRIER_HZ(TXGATE_PROTECTED_CHANNEL)
                   || CARRIER_HZ(RADIO_CHANNEL)
                          >= CARRIER_HZ(TXGATE_PROTECTED_CHANNEL) + TXGATE_MIN_DIST_HZ
                   || (TXGATE_NET_ON_PROTECTED && RADIO_CHANNEL == TXGATE_PROTECTED_CHANNEL),
               "the bridge's channel is at least 999 kHz from the protected one");
_Static_assert((FSCTRL1_VAL & 0x1Fu) * PHY_FXOSC_HZ / 1024u == 152343u,
               "IF 152 kHz with FSCTRL1 = 0x06");
_Static_assert((256u + MDMCFG3_VAL) * (1ull << (MDMCFG4_VAL & 0xFu)) * PHY_FXOSC_HZ / (1ull << 28)
                   == PHY_BAUD,
               "49.99 kBaud");
_Static_assert(PHY_FXOSC_HZ / (8u * (4u + ((MDMCFG4_VAL >> 4) & 3u)) * (1u << (MDMCFG4_VAL >> 6)))
                   == 135416u,
               "RX BW 135 kHz");
_Static_assert(PHY_FXOSC_HZ * (8u + (DEVIATN_VAL & 7u)) * (1u << ((DEVIATN_VAL >> 4) & 7u))
                       / (1u << 17)
                   == 25390u,
               "deviation 25.39 kHz");

/* MARCSTATE values reachable only through a transmit strobe: FSTXON, TX,
 * TX_END, RXTX_SWITCH, TXFIFO_UNDERFLOW (0x12-0x16), and TXRX_SWITCH (0x10),
 * the settling step from TX back to RX (cc110l.txt:4462-4495, state diagram
 * :2316-2324). */
static bool marc_is_tx(uint8_t m) {
    m &= 0x1F;
    return m == CC_MARC_TXRX_SWITCH || (m >= CC_MARC_FSTXON && m <= CC_MARC_TXFIFO_UNF);
}

/* Returns false at once on a TX state, so the caller's failure path (which
 * runs tx_watch) stops it without waiting out the timeout. */
bool wait_marcstate(uint8_t want, uint32_t timeout_us) {
    uint32_t start = timebase_us();
    uint8_t  m;
    do {
        if (cc110l_read_status_stable(CC_MARCSTATE, &m)) {
            if ((m & 0x1F) == want) {
                return true;
            }
            if (marc_is_tx(m)) {
                return false;
            }
        }
    } while ((uint32_t)(timebase_us() - start) < timeout_us);
    return false;
}

/* Take one fresh chip status byte, then act on the TX-state flag: confirm with
 * MARCSTATE (one status byte can be corrupt, cc110l.txt:1994-1997); a TX
 * state confirmed: SIDLE, then stop the board (fw_panic, watchdog reset).
 * Not confirmed: counted as a glitch and checked again, since the MARCSTATE
 * reads bring status bytes of their own. Still flagged after three rounds,
 * or MARCSTATE unreadable: SIDLE and return false; the caller re-initialises.
 *
 * The probe reads RXBYTES (header 0xFB): at least 3 bit errors away from any
 * transmit strobe (STX 0x35/0xB5, SFSTXON 0x31/0xB1). The confirmation reads
 * MARCSTATE (0xF5), 1 bit from 0xB5 (STX); a TX state caused that way shows
 * in the next status byte and is caught by the next round. */
bool tx_watch(void) {
    for (int round = 0; round < 3; round++) {
        uint8_t rb;
        (void)cc110l_read_status(CC_RXBYTES, &rb);
        if (!cc110l_take_tx_state_flag()) {
            return true;
        }
        uint8_t m = 0;
        if (!cc110l_read_status_stable(CC_MARCSTATE, &m)) {
            break;
        }
        if (marc_is_tx(m)) {
            if (cc110l_tx_expected()) {
                return true; /* a frame of ours started while we looked */
            }
            (void)cc110l_strobe(CC_SIDLE, NULL);
            fw_panic("radio: chip in a TX state that is not ours", __FILE__, __LINE__);
        }
        COUNT(status_glitch);
    }
    (void)cc110l_strobe(CC_SIDLE, NULL);
    LOG("radio: status bytes keep reporting a TX state, MARCSTATE unconfirmed or unreadable; "
        "forced IDLE");
    return false;
}

/* Bring-up failure or re-init: check first (a TX state found here is stopped
 * and stops the board), then force IDLE whatever the state, then check the
 * status byte that SIDLE and the probe returned. GDO interrupts off. */
static void radio_force_idle(void) {
    gdo_irq_enable(false);
    (void)tx_watch();
    (void)cc110l_strobe(CC_SIDLE, NULL);
    (void)tx_watch();
}

/* --- Bring-up ---------------------------------------------------------------- */
static bool reg_is_unused(unsigned a) {
    return a == 0x1E || a == 0x1F || a == 0x27 || a == 0x28;
}

/* Written with the stock (= reset) values; if this die reads them back
 * differently the radio stays idle, and the log line says which. */
static bool reg_is_reserved(unsigned a) {
    return a == CC_RSVD_20 || (a >= 0x29 && a <= 0x2B);
}

/* Registers 0x00-0x18 (IOCFG2 .. MCSM0) read back in one 25-byte burst and
 * compared with the table: they include IOCFG2/IOCFG0 (interrupt pins),
 * IOCFG1 (SO 3-state for the shared MISO), PKTCTRL0, CHANNR, FREQ and MCSM1
 * (RXOFF/TXOFF modes), and the chip changes none of them by itself. FSCAL3-0
 * (0x23-0x26) are not compared: calibration rewrites them. PATABLE[0]
 * (output power) is read back too and counts as one more register. Returns
 * the number of registers that differ, or HEALTH_REGS if the chip did not
 * answer. Used by the health check and after every calibration. */
#define HEALTH_REGS (CC_MCSM0 + 1u)
_Static_assert(CC_MCSM1 < HEALTH_REGS && CC_CHANNR < HEALTH_REGS && CC_PKTCTRL0 < HEALTH_REGS
                   && CC_IOCFG2 < HEALTH_REGS && CC_IOCFG0 < HEALTH_REGS,
               "health check covers the safety registers");

static unsigned regs_lost(void) {
    uint8_t back[HEALTH_REGS];
    if (!cc110l_read_burst(0x00, back, sizeof back)) {
        COUNT(spi_err);
        LOG("radio: register read-back: no answer");
        return HEALTH_REGS;
    }
    unsigned bad = 0;
    for (unsigned a = 0; a < HEALTH_REGS; a++) {
        if (back[a] != cc_config[a]) {
            if (bad < 4) {
                LOG("radio: reg %02x reads %02x, want %02x", a, back[a], cc_config[a]);
            }
            bad++;
        }
    }
    if (!cc110l_pa_reads_back()) {
        LOG("radio: PATABLE does not read back %02x", RADIO_PA_LEVEL);
        bad++;
    }
    return bad;
}

/* Recalibration policy (#11). The datasheet asks for calibration
 * "regularly" and after power-up or a frequency change, and notes that
 * supply voltage or temperature changes make a calibration invalid
 * (cc110l.txt:2740-2752); it gives no interval. MCSM0 FS_AUTOCAL = 0 (stock
 * table) means the chip never calibrates by itself, and an Enhanced ACK
 * cannot wait for the 0.8 ms an auto-calibration adds to IDLE -> TX. So:
 * once at every bring-up, then every RECAL_PERIOD_MS from IDLE at a quiet
 * moment (no packet arriving, no transaction on the air), independent of
 * temperature, which the bridge does not measure [I]. A calibration older
 * than CC_CAL_MAX_AGE_US (two missed periods and more) stops the
 * transmitter until the next one. Each recalibration costs about 1 ms of
 * deaf receiver: a Data frame lost then is sent again by its device. */
/* RECAL_PERIOD_MS: src/radio_internal.h. */
_Static_assert(2ull * RECAL_PERIOD_MS * 1000u < CC_CAL_MAX_AGE_US,
               "a missed recalibration does not stop the transmitter yet");

uint64_t cal_at;      /* last calibration attempt, now64() */
uint64_t cal_good_at; /* last calibration with lock */

/* With the chip in IDLE: calibrate (lock check and carrier read-back in the
 * driver), then the same register read-back as the health check, so every
 * calibration ends with both. False if the chip failed to calibrate at all
 * or lost registers: the caller re-initialises. A calibration that did not
 * lock returns true: the receiver may run, the driver refuses to transmit
 * until the next bring-up. */
static bool radio_calibrate(const char *why) {
    uint8_t         fscal[3];
    unsigned        tries = 0;
    enum cc110l_cal c     = cc110l_calibrate(fscal, &tries);
    cal_at                = now64();
    if (c == CC_CAL_ERROR) {
        LOG("radio: %s calibration failed on try %u (not IDLE, no answer, or the carrier "
            "registers changed)",
            why,
            tries);
        return false;
    }
    unsigned lost = regs_lost();
    if (lost) {
        LOG("radio: %u registers differ after %s calibration", lost, why);
        return false;
    }
    if (c == CC_CAL_LOCKED) {
        cal_good_at = cal_at;
    }
    LOG("radio: %s calibration, %u %s: FSCAL3 %02x FSCAL2 %02x FSCAL1 %02x, PLL %s, transmitter "
        "%s",
        why,
        tries,
        tries == 1 ? "try" : "tries",
        fscal[0],
        fscal[1],
        fscal[2],
        c == CC_CAL_LOCKED ? "locked" : "NOT locked",
        cc110l_cal_ok() ? "may be used" : "REFUSED until the next bring-up");
    return tx_watch();
}

static bool radio_configure_steps(void) {
    gdo_irq_enable(false);
    cc110l_ack_cancel();
    radio_rx.assembling = false;
    radio_rx.in_packet  = false;
    fe_sample.valid     = false;

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
    LOG("radio: PARTNUM %02x VERSION %02x%s",
        part,
        ver,
        (part == 0x00 && ver == 0x17) ? " (CC110L as documented)" : " (not the documented 00/17)");
    if (ver == 0x00 || ver == 0xFF) {
        LOG("radio: VERSION %02x looks like a dead bus; not configuring", ver);
        return false;
    }
    if (!tx_watch()) {
        return false;
    }

    if (!cc110l_write_burst(0x00, cc_config, CC_NUM_CONFIG_REGS)) {
        LOG("radio: register burst write failed");
        return false;
    }
    if (!tx_watch()) {
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
            LOG("radio: reg %02x%s wrote %02x read %02x",
                a,
                reg_is_reserved(a) ? " (reserved)" : "",
                cc_config[a],
                back[a]);
            bad++;
        }
    }
    if (bad) {
        LOG("radio: %u registers did not verify; staying idle", bad);
        return false;
    }

    /* Output power: SRES left PATABLE at 0xC6; the bring-up level is about
     * 20 dB lower (src/cc110l.h). */
    if (!cc110l_set_pa(RADIO_PA_LEVEL)) {
        LOG("radio: PATABLE %02x not set or did not read back", RADIO_PA_LEVEL);
        return false;
    }

    /* The chip is IDLE since SRES. A calibration without lock leaves the
     * transmitter refused for this bring-up; receiving goes on. */
    if (!radio_calibrate("bring-up")) {
        return false;
    }

    if (!cc110l_strobe(CC_SFRX, NULL) || !cc110l_strobe(CC_SRX, NULL)
        || !wait_marcstate(CC_MARC_RX, 2000)) {
        LOG("radio: did not enter RX");
        return false;
    }
    if (!tx_watch()) {
        return false;
    }
    gdo_irq_enable(true);
    LOG("radio: RX on channel %u (%lu Hz), PATABLE %02x, transmitter %s",
        RADIO_CHANNEL,
        (unsigned long)CARRIER_HZ(RADIO_CHANNEL),
        RADIO_PA_LEVEL,
        !RADIO_TX_ALLOWED            ? "switched off (receive-only image)"
        : !cc110l_cal_ok()           ? "refused: no PLL lock"
        : cc110l_tx_verify_channel() ? "enabled on this channel only"
                                     : "refused: frequency registers do not read back");
    return true;
}

/* Reset, load and verify the table, calibrate, enter RX. On any failure the
 * chip is checked for a TX state and forced to IDLE before returning, with
 * the GDO interrupts left off. */
bool radio_configure(void) {
    if (radio_configure_steps()) {
        return true;
    }
    radio_force_idle();
    return false;
}

/* Runtime re-init: check and force IDLE first, then the full bring-up. */
void radio_reinit(void) {
    COUNT(reinit);
    radio_force_idle();
    (void)radio_configure();
}

/* Every RECAL_PERIOD_MS, at a quiet moment: from RX to IDLE, calibrate,
 * back to RX. A prepared ACK is dropped first; GDO2 cannot fire while the
 * chip is IDLE. Any failure re-initialises the radio. */
void radio_recalibrate(void) {
    cc110l_ack_cancel();
    radio_rx.assembling = false;
    radio_rx.in_packet  = false;
    fe_sample.valid     = false;
    if (!cc110l_strobe(CC_SIDLE, NULL) || !wait_marcstate(CC_MARC_IDLE, 1000)
        || !radio_calibrate("periodic") || !rx_flush_and_arm()
        || !wait_marcstate(CC_MARC_RX, 2000)) {
        LOG("radio: periodic calibration failed; re-initialising");
        radio_reinit();
    }
}

/* Every 10 s: the chip must still hold our configuration and be in RX. A
 * brown-out or reset of the radio leaves reset defaults (IOCFG0 = 0x3F);
 * regs_lost() compares the registers that matter. */
void health_check(void) {
    if (!gdo_irq_on) {
        /* A storm guard trip or a failed re-init left the interrupts off. */
        LOG("radio: GDO interrupts off; re-initialising");
        radio_reinit();
        return;
    }
    unsigned bad = regs_lost();
    if (bad == HEALTH_REGS) {
        /* No CHIP_RDYn: the chip is not answering, as the old IOCFG0 check
         * also treated it. */
        LOG("radio: health: no answer; re-initialising");
        radio_reinit();
        return;
    }
    if (bad) {
        LOG("radio: %u registers lost; forcing IDLE and re-initialising", bad);
        radio_reinit();
        return;
    }
    uint8_t m = 0;
    if (!cc110l_read_status_stable(CC_MARCSTATE, &m)) {
        COUNT(spi_err);
        return;
    }
    m &= 0x1F;
    if (marc_is_tx(m)) {
        if (cc110l_tx_expected()) {
            return; /* our own frame is on the air */
        }
        (void)cc110l_strobe(CC_SIDLE, NULL);
        fw_panic("radio: MARCSTATE in a TX state that is not ours", __FILE__, __LINE__);
    }
    if (m == CC_MARC_IDLE) {
        LOG("radio: found IDLE, re-arming RX");
        COUNT(rearm);
        rx_flush_and_arm();
    }
}
