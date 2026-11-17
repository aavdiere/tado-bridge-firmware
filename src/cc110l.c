/* CC110L access over SPI1 (#9). Datasheet: docs/datasheets/cc110l.txt
 * (TI SWRS109C). Reference flow: freedo/firmware/cc1101_bridge/
 * cc1101_bridge.ino:582-603 (reset), :530-575 (access helpers).
 *
 * SPI header byte: bit 7 R/W (1 = read), bit 6 burst, bits 5:0 address
 * (cc110l.txt:1292-1293). Addresses 0x30-0x3D are command strobes when the
 * burst bit is 0, whatever the R/W bit (cc110l.txt:1431-1434, 1453-1456), and
 * status registers when it is 1. 0x3E is PATABLE, 0x3F the FIFOs: a write
 * header (R/W = 0) reaches the TX FIFO (cc110l.txt:1482-1494).
 *
 * TRANSMIT GUARD. Every access goes through guard_header(), and every byte
 * written to a configuration register through guard_write(). They refuse
 * everything that can transmit or prepare a transmission:
 *   - strobes other than SRES, SCAL, SRX, SIDLE, SFRX, SNOP: never SFSTXON,
 *     SXOFF or SPWD; STX and SFTX only while tx_permit is set;
 *   - any write header to 0x2F-0x3F except those strobes: PATABLE only as
 *     the single-byte write inside cc110l_set_pa(), which accepts only the
 *     settings documented at or below the reset level 0xC6 (pa_allowed()),
 *     never a burst (PATABLE[1]); the TX FIFO only as a burst write while
 *     tx_permit is set; any read there except status registers, PATABLE[0]
 *     and the RX FIFO;
 *   - FREND0 with PA_POWER other than 0: the PA uses PATABLE[0] only
 *     (cc110l.txt:2799-2805, Table 5-55);
 *   - MCSM1 with RXOFF_MODE = FSTXON or TX, or TXOFF_MODE = FSTXON or "stay
 *     in TX" (cc110l.txt:3929-3955): the only ways the state machine enters
 *     TX without an STX strobe;
 *   - PKTCTRL0 with PKT_FORMAT other than normal FIFO mode
 *     (cc110l.txt:3641-3660: random TX and serial modes), or with
 *     LENGTH_CONFIG other than variable length (cc110l.txt:3673-3684:
 *     infinite length transmits until stopped by hand).
 * The CC110L has no automatic acknowledgement and no wake-on-radio
 * (docs/datasheets/cc110l-vs-cc1101.md), so with these refused it cannot
 * transmit.
 *
 * tx_permit is set only in the transmit functions at the end of this file
 * (cc110l_tx_start(), cc110l_tx_feed(), cc110l_tx_abort() for its SFTX, and
 * cc110l_ack_fire_from_isr()), and only when RADIO_TX_ALLOWED is 1. STX is
 * strobed in two places, cc110l_tx_start() and cc110l_ack_fire_from_isr(),
 * and only when all of this holds:
 *   - the whole register table has been written since the last reset;
 *   - PATABLE[0] has been set by cc110l_set_pa() since the last reset and
 *     reads back as set, before every frame and every ACK;
 *   - the synthesizer was calibrated after the last register write, the
 *     PLL locked (FSCAL1 != 0x3F), and that calibration is younger than
 *     CC_CAL_MAX_AGE_US (cc110l_calibrate(); #11);
 *   - the chip's own frequency registers have been read back and equal that
 *     table: before every frame the task sends, and at most
 *     ACK_VERIFY_MAX_US before an Enhanced ACK;
 *   - the transmit gate (src/txgate.c) has approved the frame against those
 *     registers: off the protected channel, on the test channel, our own
 *     source address, an ACK only as the answer to a frame addressed to us;
 *   - no other transmission of ours is on the air;
 *   - the frame's airtime fits in the rolling one-hour budget (airtime.h:
 *     30 s per hour against the band's 1 %). Every frame is charged here,
 *     before it is keyed: an Enhanced ACK when it is prepared, any other
 *     frame in cc110l_tx_start().
 * The bytes that go into the TX FIFO are the driver's own copy of the
 * approved frame. */
#include "cc110l.h"

#include "airtime.h"
#include "log.h"
#include "mac.h"
#include "spi1.h"
#include "timebase.h"

#include <string.h>

#define HDR_READ  0x80
#define HDR_BURST 0x40
#define HDR_ADDR  0x3F

#define CHIP_RDY_TIMEOUT_US \
    1000u /* SO goes low at once unless in SLEEP/XOFF, cc110l.txt:1297-1299 */
#define RESET_RDY_TIMEOUT_US \
    5000u /* XOSC restart after SRES; 150 us typical, cc110l.txt:1333,1356-1362 */

static volatile bool tx_state_seen;

/* Open only inside the transmit functions, for a frame the gate approved. */
static volatile bool tx_permit;

/* Values last written to the configuration registers, and whether the whole
 * table has been written since the last reset (and not been found different
 * from the chip since). The transmit gate judges the carrier from these. */
static uint8_t shadow[CC_NUM_CONFIG_REGS];
static bool    shadow_full;

/* A read-back of the chip's frequency registers that equalled the shadow
 * (cc110l_tx_verify_channel()), and when. Each one covers one frame: the
 * frame cc110l_tx_start() sends right after it, or the Enhanced ACK
 * prepared next. The GDO2 interrupt sends that ACK only while the read-back
 * is from the reception of the frame it answers: the ACK is prepared after
 * that frame's 16 header bytes, so at most 113 bytes (18 ms) before it
 * ends. */
#define ACK_VERIFY_MAX_US 50000u
static bool     verified;
static uint32_t verified_us;

/* PLL lock (#11). MCSM0 FS_AUTOCAL = 0: the synthesizer is calibrated only
 * by SCAL, from IDLE (cc110l.txt:2744-2747). FSCAL1 = 0x3F after it means
 * the PLL did not lock (cc110l.txt:2756-2759). pll_locked is set only by
 * cc110l_calibrate() after that check and a read-back of the carrier
 * registers, and cleared by every SCAL strobe (guard_header()), every
 * configuration write (a new frequency needs a new calibration,
 * cc110l.txt:2741-2742) and every reset. cal_failed latches when
 * CAL_TRIES calibrations in a row did not lock, until the next reset: no
 * transmitter for the rest of that bring-up, whatever later calibrations
 * give. Transmitting needs pll_locked, no latch, and a calibration younger
 * than CC_CAL_MAX_AGE_US. */
#define CAL_TRIES      3u
#define CAL_WAIT_US    750u  /* manual calibration 735 us, cc110l.txt Table 5-11 */
#define CAL_TIMEOUT_US 5000u /* TEST0 0x09: 712 us at most, Table 5-12; wide margin */
static volatile bool pll_locked;
static bool          cal_failed;
static uint32_t      cal_us;

/* Output power (PATABLE[0]). The chip resets it to 0xC6 (cc110l.txt:2806-
 * 2809). pa_ok: cc110l_set_pa() wrote pa_value since the last reset and it
 * read back; cleared by a reset and by any read-back that differs. pa_permit
 * opens the guard for that one write. */
#define PATABLE 0x3Eu
static volatile bool pa_ok;
static bool          pa_permit;
static uint8_t       pa_value;

static bool strobe_allowed(uint8_t addr) {
    switch (addr) {
        case CC_SRES:
        case CC_SCAL:
        case CC_SRX:
        case CC_SIDLE:
        case CC_SFRX:
        case CC_SNOP:
            return true;
        case CC_STX:
        case CC_SFTX:
            return RADIO_TX_ALLOWED && tx_permit;
        default:
            return false; /* SFSTXON, SXOFF, SPWD, reserved */
    }
}

static void guard_header(uint8_t hdr) {
    uint8_t addr  = hdr & HDR_ADDR;
    bool    read  = (hdr & HDR_READ) != 0;
    bool    burst = (hdr & HDR_BURST) != 0;

    if (addr < CC_NUM_CONFIG_REGS) {
        return; /* configuration register; written values go through guard_write */
    }
    if (addr >= 0x30 && addr <= 0x3D && !burst) {
        if (!strobe_allowed(addr)) {
            fw_panic("radio guard: strobe refused", __FILE__, __LINE__);
        }
        if (addr == CC_SCAL) {
            pll_locked = false; /* until cc110l_calibrate() has checked the lock */
        }
        return;
    }
    if (read && burst && addr >= 0x30 && addr <= 0x3D) {
        return; /* status register */
    }
    if (read && addr == 0x3F) {
        return; /* RX FIFO */
    }
    if (addr == PATABLE && !burst && (read || pa_permit)) {
        return; /* PATABLE[0]: a read, or the checked write of cc110l_set_pa() */
    }
    if (!read && burst && addr == 0x3F && RADIO_TX_ALLOWED && tx_permit) {
        return; /* TX FIFO, an approved frame */
    }
    fw_panic("radio guard: TX FIFO / PATABLE / unknown access refused", __FILE__, __LINE__);
}

static void guard_write(uint8_t addr, uint8_t val) {
    if (addr >= CC_NUM_CONFIG_REGS) {
        fw_panic("radio guard: write outside 0x00-0x2E", __FILE__, __LINE__);
    }
    if (addr == CC_MCSM1) {
        uint8_t rxoff = (val >> 2) & 3, txoff = val & 3;
        /* RXOFF 01 = FSTXON, 10 = TX; TXOFF 01 = FSTXON, 10 = stay in TX. */
        if (rxoff == 1 || rxoff == 2 || txoff == 1 || txoff == 2) {
            fw_panic("radio guard: MCSM1 could enter TX", __FILE__, __LINE__);
        }
    }
    if (addr == CC_PKTCTRL0 && ((val >> 4) & 3) != 0) {
        fw_panic("radio guard: PKTCTRL0 not in FIFO mode", __FILE__, __LINE__);
    }
    /* LENGTH_CONFIG 01 variable only: 10 (infinite) keeps transmitting
     * until stopped by hand, 00 (fixed) sends PKTLEN bytes whatever the
     * length byte says, 11 is reserved (cc110l.txt:1826-1828, Table 5-31). */
    if (addr == CC_PKTCTRL0 && (val & 3u) != 1u) {
        fw_panic("radio guard: PKTCTRL0 not in variable length mode", __FILE__, __LINE__);
    }
    if (addr == CC_FREND0 && (val & 7u) != 0) {
        fw_panic("radio guard: FREND0 PA_POWER selects PATABLE[1..]", __FILE__, __LINE__);
    }
}

/* The prepared Enhanced ACK, as it goes into the TX FIFO: length byte,
 * frame. prepared is the hand-over to the GDO2 interrupt: the task writes
 * buf only while it is false, and only the task sets it. */
static struct {
    uint8_t       buf[1u + MAC_ACK_LEN];
    uint32_t      verified_us; /* the read-back this ACK was prepared on */
    volatile bool prepared;
} ack;

/* A transmission of our own is on the air from its STX (tx_start_us) for
 * tx_len_us. Written by the task (cc110l_tx_start()) and by the GDO2
 * interrupt (an Enhanced ACK).
 *
 * The time is kept as a start and a length, and compared as an unsigned
 * elapsed time: an end time compared as a signed difference (as until
 * 2026-11-17) reads "still on the air" from 35.8 to 71.6 minutes after the
 * frame if nobody looked in between, which in a quiet network nobody does,
 * and the TX-state watch would then take a stray TX state for ours. The
 * elapsed time can still wrap after 71.6 minutes; tx_active is cleared once
 * the frame is over, so that needs no call for 71.6 minutes, and the radio
 * task calls cc110l_tx_expected() on every loop pass.
 *
 * Who may clear tx_active: the interrupt, which nothing preempts here, and
 * the task only while no ACK is prepared. Only a prepared ACK can make the
 * interrupt start a transmission, and only the task prepares one, so the
 * interrupt cannot set tx_active between the task's check and its store.
 * ack.prepared is read first for that reason. */
static volatile bool     tx_active;
static volatile uint32_t tx_start_us, tx_len_us;

/* Airtime of a frame of n bytes: 4 preamble, 4 sync, the length byte and 2
 * CRC bytes around it at 160 us per byte (docs/radio.md "Data frame
 * plaintext"), plus IDLE to TX (75 us, cc110l.txt Table 5-11) and a margin
 * for the interrupt and task that watch for it. */
#define TX_MARGIN_US 2000u
static void tx_expect(size_t n) {
    tx_len_us   = (uint32_t)(11u + n) * 160u + 75u + TX_MARGIN_US;
    tx_start_us = timebase_us();
    tx_active   = true;
}

bool cc110l_tx_expected(void) {
    bool may_clear = !ack.prepared; /* read before tx_active, see above */
    if (!tx_active) {
        return false;
    }
    if ((uint32_t)(timebase_us() - tx_start_us) < tx_len_us) {
        return true;
    }
    if (may_clear) {
        tx_active = false;
    }
    return false;
}

/* STATE TX (010), FSTXON (011) and TXFIFO_UNDERFLOW (111) can only follow a
 * transmit strobe (cc110l.txt:1381-1405). Outside a transmission of our own
 * they are flagged. A status byte of 0xFF (MISO stuck high mid-byte) also
 * reads as 111; the radio task confirms with MARCSTATE before acting, so
 * such a glitch is only counted. */
static void note_status(uint8_t status) {
    uint8_t st = CC_STATE(status);
    if ((st == CC_STATE_TX || st == CC_STATE_FSTXON || st == CC_STATE_TXFIFO_UNF)
        && !cc110l_tx_expected()) {
        tx_state_seen = true;
    }
}

bool cc110l_take_tx_state_flag(void) {
    bool seen     = tx_state_seen;
    tx_state_seen = false;
    return seen;
}

static bool wait_so_low(uint32_t timeout_us) {
    uint32_t start = timebase_us();
    while (spi1_miso_high()) {
        if ((uint32_t)(timebase_us() - start) > timeout_us) {
            return false;
        }
    }
    return true;
}

/* --- Transmit state (#17) ---------------------------------------------------- */
static uint8_t own_eui[MAC_EUI_LEN];
static bool    own_set;

/* The frame being sent, as it goes into the TX FIFO: length byte, frame. */
static struct {
    uint8_t  buf[1u + MAC_FRAME_MAX];
    uint16_t total, idx;
} tx;

static enum txgate_verdict last_verdict = TXGATE_NO_IDENTITY;

/* Airtime charged in the last hour. Radio task only: cc110l_tx_start() and
 * cc110l_ack_prepare() charge it, the interrupt only sends an ACK that was
 * charged when it was prepared. An ACK prepared and then not sent stays
 * charged. */
static struct airtime air;

uint32_t cc110l_airtime_used_us(void) {
    return airtime_used_us(&air, timebase_us());
}

/* The chip status byte that came back with an SIDLE header shows the state
 * the strobe found. From TX the chip needs a quarter bit to reach IDLE,
 * and it never leaves TXFIFO_UNDERFLOW by SIDLE; in both cases the SFTX
 * that would follow in the same access is dropped (cc110l.txt:1449-1451),
 * and a frame written now would go out behind what the TX FIFO still
 * holds. */
static bool state_blocks_flush(uint8_t status) {
    uint8_t st = CC_STATE(status);
    return st == CC_STATE_TX || st == CC_STATE_FSTXON || st == CC_STATE_TXFIFO_UNF;
}

/* Lock the bus, select the radio, wait for CHIP_RDYn (cc110l.txt:1297-1299),
 * then send the checked header. Returns false (bus released) if SO stayed high. */
static bool begin(uint8_t hdr, uint8_t *status) {
    guard_header(hdr);
    spi1_lock();
    spi1_select(SPI1_DEV_RADIO);
    if (!wait_so_low(CHIP_RDY_TIMEOUT_US)) {
        spi1_deselect(SPI1_DEV_RADIO);
        spi1_unlock();
        return false;
    }
    uint8_t s = spi1_xfer(hdr);
    note_status(s);
    if (status) {
        *status = s;
    }
    return true;
}

static void end(void) {
    spi1_deselect(SPI1_DEV_RADIO);
    spi1_unlock();
}

bool cc110l_reset(void) {
    guard_header(CC_SRES);
    shadow_full  = false; /* the chip is back at its reset values */
    verified     = false;
    pll_locked   = false;
    cal_failed   = false; /* a new bring-up gets new tries */
    pa_ok        = false; /* PATABLE is back at 0xC6 */
    ack.prepared = false;
    tx_active    = false;
    tx.idx       = tx.total; /* nothing left to feed */
    spi1_lock();
    /* CSn low, then high for at least 40 us (cc110l.txt:2403-2404). SCLK
     * idles low here (mode 0), not high as in the datasheet's power-up
     * sequence; the chip is already powered and past its automatic POR, and
     * SRES resets it fully either way [I]. Freedo does the same
     * (.ino:582-596). */
    spi1_select(SPI1_DEV_RADIO);
    delay_us(10);
    spi1_deselect(SPI1_DEV_RADIO);
    delay_us(50);
    spi1_select(SPI1_DEV_RADIO);
    bool ok = wait_so_low(RESET_RDY_TIMEOUT_US);
    if (ok) {
        (void)spi1_xfer(CC_SRES);
        /* Reset is complete when SO goes low again (cc110l.txt:2406-2407,
         * Figure 5-4 at :1466-1472). Give the chip time to raise SO first. */
        delay_us(100);
        ok = wait_so_low(RESET_RDY_TIMEOUT_US);
    }
    spi1_deselect(SPI1_DEV_RADIO);
    spi1_unlock();
    return ok;
}

bool radio_strobe(uint8_t strobe, uint8_t *status) {
    if (!strobe_allowed(strobe)) { /* also refused in guard_header; checked here first */
        fw_panic("radio_strobe: refused", __FILE__, __LINE__);
    }
    if (!begin(strobe, status)) {
        return false;
    }
    end();
    return true;
}

bool cc110l_write_reg(uint8_t addr, uint8_t val) {
    return cc110l_write_burst(addr, &val, 1);
}

bool cc110l_write_burst(uint8_t addr, const uint8_t *val, size_t n) {
    if (n == 0 || addr + n > CC_NUM_CONFIG_REGS) {
        fw_panic("radio guard: burst beyond 0x2E", __FILE__, __LINE__);
    }
    for (size_t i = 0; i < n; i++) {
        guard_write((uint8_t)(addr + i), val[i]);
    }
    pll_locked = false; /* calibrate again after any change (cc110l.txt:2741-2742) */
    if (!begin((uint8_t)(addr | (n > 1 ? HDR_BURST : 0)), NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        note_status(spi1_xfer(val[i]));
    }
    end();
    memcpy(shadow + addr, val, n);
    if (addr == 0 && n == CC_NUM_CONFIG_REGS) {
        shadow_full = true;
    }
    return true;
}

bool cc110l_read_reg(uint8_t addr, uint8_t *val) {
    return cc110l_read_burst(addr, val, 1);
}

bool cc110l_read_burst(uint8_t addr, uint8_t *val, size_t n) {
    if (n == 0 || addr + n > CC_NUM_CONFIG_REGS) {
        fw_panic("radio: config read beyond 0x2E", __FILE__, __LINE__);
    }
    if (!begin((uint8_t)(addr | HDR_READ | (n > 1 ? HDR_BURST : 0)), NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        val[i] = spi1_xfer(0);
    }
    end();
    return true;
}

bool cc110l_read_status(uint8_t addr, uint8_t *val) {
    if (!begin((uint8_t)(addr | HDR_READ | HDR_BURST), NULL)) {
        return false;
    }
    *val = spi1_xfer(0);
    end();
    return true;
}

bool cc110l_read_status_stable(uint8_t addr, uint8_t *val) {
    uint8_t prev, cur;
    if (!cc110l_read_status(addr, &prev)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!cc110l_read_status(addr, &cur)) {
            return false;
        }
        if (cur == prev) {
            *val = cur;
            return true;
        }
        prev = cur;
    }
    *val = prev;
    return true;
}

bool cc110l_read_rxfifo(uint8_t *dst, size_t n) {
    if (n == 0 || n > CC_FIFO_SIZE) {
        return false;
    }
    if (!begin(0x3F | HDR_READ | HDR_BURST, NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = spi1_xfer(0);
    }
    end();
    return true;
}

/* --- Transmit (#17) ----------------------------------------------------------- */
void cc110l_set_identity(const uint8_t own[8]) {
    memcpy(own_eui, own, MAC_EUI_LEN);
    own_set = true;
}

enum txgate_verdict cc110l_tx_verdict(void) {
    return last_verdict;
}

static struct txgate_radio shadow_radio(void) {
    struct txgate_radio r = {
        .freq2   = shadow[CC_FREQ2],
        .freq1   = shadow[CC_FREQ1],
        .freq0   = shadow[CC_FREQ0],
        .channr  = shadow[CC_CHANNR],
        .mdmcfg1 = shadow[CC_MDMCFG1],
        .mdmcfg0 = shadow[CC_MDMCFG0],
        .fsctrl0 = shadow[CC_FSCTRL0],
    };
    return r;
}

/* The chip's carrier registers equal the full table last written. A
 * difference clears shadow_full: the chip no longer holds what was
 * written, and nothing is sent until the table is written again. */
static bool carrier_reads_back(void) {
    uint8_t a[CC_FREQ0 - CC_CHANNR + 1u], b[2];
    if (!shadow_full) {
        return false;
    }
    if (!cc110l_read_burst(CC_CHANNR, a, sizeof a) || !cc110l_read_burst(CC_MDMCFG1, b, sizeof b)) {
        return false; /* no answer now; the table itself is not in doubt */
    }
    if (memcmp(a, shadow + CC_CHANNR, sizeof a) != 0
        || memcmp(b, shadow + CC_MDMCFG1, sizeof b) != 0) {
        shadow_full = false;
        return false;
    }
    return true;
}

bool cc110l_tx_verify_channel(void) {
    verified = false;
    if (!carrier_reads_back() || !cc110l_pa_reads_back()) {
        return false;
    }
    verified_us = timebase_us();
    verified    = true;
    return true;
}

/* Safe from any context: plain loads and one subtraction. */
bool cc110l_cal_ok(void) {
    return pll_locked && !cal_failed && (uint32_t)(timebase_us() - cal_us) <= CC_CAL_MAX_AGE_US;
}

void cc110l_cal_expire(void) {
    pll_locked = false;
}

/* PATABLE settings for 868 MHz documented at or below the reset level 0xC6
 * (9.6 dBm with wire-wound inductors, Table 5-15, cc110l.txt:2834-2839;
 * 8.5 dBm with multi-layer inductors, Table 5-17, cc110l.txt:2868-2873).
 * The PA code is not monotonic: 0xC0, 0xC2, 0xC3 and 0xC5 are numerically
 * below 0xC6 but give 10-12 dBm (Tables 5-14 and 5-16, cc110l.txt:
 * 2816-2832, 2852-2865), so the check is this list, not a comparison.
 * Besides 0xC6 itself only the entries at 0 dBm and below, which are at
 * least 8 dB under 0xC6 in either inductor variant; the IB01's variant is
 * not known. 0x61-0x6F are not allowed at all (cc110l.txt:2808). */
static bool pa_allowed(uint8_t v) {
    switch (v) {
        case 0xC6: /* reset value, 9.6 / 8.5 dBm */
        case 0x50: /*   0 dBm, both tables */
        case 0x37: /*  -6 dBm, wire-wound */
        case 0x26: /* -10 dBm, wire-wound */
        case 0x27: /* -10 dBm, multi-layer */
        case 0x1D: /* -15 dBm, wire-wound */
        case 0x1E: /* -15 dBm, multi-layer */
        case 0x17: /* -20 dBm, wire-wound */
        case 0x0F: /* -20 dBm, multi-layer */
        case 0x03: /* -30 dBm, both tables */
            return true;
        default:
            return false;
    }
}

static bool pa_read(uint8_t *v) {
    if (!begin(PATABLE | HDR_READ, NULL)) {
        return false;
    }
    *v = spi1_xfer(0);
    end();
    return true;
}

bool cc110l_set_pa(uint8_t v) {
    pa_ok = false;
    if (!pa_allowed(v)) {
        return false; /* the chip is not touched */
    }
    /* One byte to PATABLE[0]: header 0x3E, no burst (cc110l.txt:1505-1517).
     * The read-back is a new access: CSn high resets the PATABLE index. */
    pa_permit = true;
    bool ok   = begin(PATABLE, NULL);
    if (ok) {
        (void)spi1_xfer(v);
        end();
    }
    pa_permit = false;
    uint8_t back;
    if (!ok || !pa_read(&back) || back != v) {
        return false;
    }
    pa_value = v;
    pa_ok    = true;
    return true;
}

bool cc110l_pa_reads_back(void) {
    uint8_t back;
    if (!pa_ok) {
        return false;
    }
    if (!pa_read(&back)) {
        return false; /* no answer now; the setting is not in doubt */
    }
    if (back != pa_value) {
        pa_ok = false; /* the chip no longer holds it: until the next bring-up */
        return false;
    }
    return true;
}

static bool wait_idle(uint32_t timeout_us) {
    uint32_t start = timebase_us();
    uint8_t  m;
    do {
        if (cc110l_read_status_stable(CC_MARCSTATE, &m) && (m & 0x1Fu) == CC_MARC_IDLE) {
            return true;
        }
        delay_us(50);
    } while ((uint32_t)(timebase_us() - start) < timeout_us);
    return false;
}

/* Bounded busy waits only: at most CAL_TRIES x (CAL_WAIT_US + CAL_TIMEOUT_US
 * + one 50 us poll), about 18 ms; when the chip answers as documented about
 * 0.8 ms per try, 2.4 ms for all three. The radio task's other work waits
 * meanwhile, but nothing arrives while the chip is IDLE. */
enum cc110l_cal cc110l_calibrate(uint8_t fscal[3], unsigned *tries) {
    pll_locked = false;
    *tries     = 0;
    fscal[0] = fscal[1] = fscal[2] = 0;
    for (unsigned t = 1; t <= CAL_TRIES; t++) {
        *tries = t;
        /* SCAL works from IDLE only (cc110l.txt:2746-2747); its own status
         * byte would tell the state too late, after the strobe. */
        uint8_t m = 0;
        if (!cc110l_read_status_stable(CC_MARCSTATE, &m) || (m & 0x1Fu) != CC_MARC_IDLE) {
            return CC_CAL_ERROR;
        }
        if (!radio_strobe(CC_SCAL, NULL)) {
            return CC_CAL_ERROR;
        }
        delay_us(CAL_WAIT_US);
        if (!wait_idle(CAL_TIMEOUT_US) || !cc110l_read_burst(CC_FSCAL3, fscal, 3)) {
            return CC_CAL_ERROR;
        }
        if (fscal[2] == 0x3F) {
            continue; /* not locked: calibrate again (cc110l.txt:2760-2761) */
        }
        if (!carrier_reads_back()) {
            return CC_CAL_ERROR;
        }
        cal_us     = timebase_us();
        pll_locked = true;
        return CC_CAL_LOCKED;
    }
    cal_failed = true;
    return CC_CAL_NO_LOCK;
}
_Static_assert(CC_CHANNR + 2u == CC_FSCTRL0 && CC_FSCTRL0 + 1u == CC_FREQ2
                   && CC_MDMCFG1 + 1u == CC_MDMCFG0,
               "the read-back bursts cover CHANNR, FSCTRL0, FREQ2-0, MDMCFG1-0");

/* The gate's verdict on a frame, against the registers last written. */
static enum txgate_verdict
judge(const uint8_t *frame, size_t len, const uint8_t *answered, unsigned flags) {
    struct txgate_radio r = shadow_radio();
    if (!shadow_full) {
        return TXGATE_OFF_PLAN;
    }
    return txgate_check_frame(&r, own_set ? own_eui : NULL, frame, len, answered, flags);
}

/* One more header byte inside an open access. */
static uint8_t more(uint8_t hdr) {
    guard_header(hdr);
    return spi1_xfer(hdr);
}

static bool txfifo_write(const uint8_t *p, size_t n) {
    if (!begin(0x3F | HDR_BURST, NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        (void)spi1_xfer(p[i]);
    }
    end();
    return true;
}

enum cc110l_tx_result cc110l_tx_start(const uint8_t *frame, size_t len, unsigned flags) {
    if (!RADIO_TX_ALLOWED) {
        return CC_TX_DISABLED;
    }
    /* From here the interrupt sends nothing. If it has just sent an Enhanced
     * ACK, or an earlier frame has not been reported done, that frame is
     * still on the air: SIDLE would cut it, and SFTX is dropped until the
     * chip is IDLE (cc110l.txt:1449-1451; TX to IDLE takes a quarter bit,
     * Table 5-11), so its unsent bytes would lead this frame. */
    cc110l_ack_cancel();
    if (cc110l_tx_expected()) {
        return CC_TX_BUSY;
    }
    if (!cc110l_cal_ok()) {
        return CC_TX_NO_LOCK;
    }
    if (!pa_ok) {
        return CC_TX_NO_PA;
    }
    /* The gate judges the registers last written; the chip must still hold
     * them now (it may have reset since). Two short burst reads, about
     * 30 us at 6 MHz [I]. */
    if (!cc110l_tx_verify_channel()) {
        last_verdict = TXGATE_OFF_PLAN;
        return CC_TX_REFUSED;
    }
    verified     = false; /* used for this frame */
    last_verdict = judge(frame, len, NULL, flags);
    if (last_verdict != TXGATE_OK) {
        return CC_TX_REFUSED;
    }
    if (!airtime_charge(&air, timebase_us(), AIRTIME_FRAME_US(len))) {
        return CC_TX_DUTY;
    }
    tx.buf[0] = (uint8_t)len;
    memcpy(tx.buf + 1, frame, len);
    tx.total     = (uint16_t)(len + 1u);
    tx.idx       = 0;
    size_t first = tx.total > CC_FIFO_SIZE ? CC_FIFO_SIZE : tx.total;

    tx_permit = true;
    /* SIDLE keeps a frame cut off mid-air in the RX FIFO, and the chip
     * returns to RX straight after this frame: flush both FIFOs now, in one
     * access (freedo cc1101_bridge.ino:645-649). SFRX and SFTX need IDLE,
     * which SIDLE reaches in 0.1 us (cc110l.txt:1449-1451, Table 5-11). */
    uint8_t status = 0;
    bool    ok     = begin(CC_SIDLE, &status);
    bool    stuck  = ok && state_blocks_flush(status);
    if (stuck) {
        /* A transmit state nobody expected (cc110l_tx_expected() was false
         * above). Nothing more goes to the chip here; the caller aborts and
         * re-arms as for any failed transmission. */
        end();
        ok = false;
    } else if (ok) {
        (void)more(CC_SFRX);
        (void)more(CC_SFTX);
        end();
        ok = txfifo_write(tx.buf, first);
    }
    if (ok) {
        tx_expect(len);
        ok = begin(CC_STX, NULL);
        if (ok) {
            end();
        }
    }
    tx_permit = false;
    if (!ok) {
        tx_active = false;
        tx.idx    = tx.total; /* nothing to feed */
        return stuck ? CC_TX_STATE : CC_TX_SPI;
    }
    tx.idx = (uint16_t)first;
    return CC_TX_OK;
}

bool cc110l_tx_feed(size_t *left) {
    *left = (size_t)(tx.total - tx.idx);
    if (*left == 0) {
        return true;
    }
    uint8_t tb;
    if (!cc110l_read_status_stable(CC_TXBYTES, &tb) || (tb & 0x80u)) {
        return false; /* underflow: only SFTX gets out of it (cc110l.txt:1944-1945) */
    }
    size_t used = tb & 0x7Fu;
    if (used >= CC_FIFO_SIZE) {
        return true; /* full, or a corrupt read (cc110l.txt:1436-1441): try again */
    }
    size_t chunk = CC_FIFO_SIZE - used < *left ? CC_FIFO_SIZE - used : *left;
    tx_permit    = true;
    bool ok      = txfifo_write(tx.buf + tx.idx, chunk);
    tx_permit    = false;
    if (ok) {
        tx.idx = (uint16_t)(tx.idx + chunk);
        *left -= chunk;
    }
    return ok;
}

void cc110l_tx_done(void) {
    tx_active = false;
}

/* SFTX works in IDLE and in TXFIFO_UNDERFLOW only (cc110l.txt:3255-3257).
 * Out of an underflow it is the only way (cc110l.txt:1944-1945), and an
 * SIDLE first would have it dropped: SIDLE clears every strobe that follows
 * until IDLE is reached (cc110l.txt:1449-1451), which an underflow never
 * does by itself. From every other state: SIDLE, then SFTX once the chip is
 * IDLE. TX to IDLE takes a quarter bit, 5 us at 50 kBaud (Table 5-11), more
 * than the 1.3 us to the next header byte in the same access, so the two
 * strobes are separate accesses with a wait in between (freedo
 * cc1101_bridge.ino:659-660 and :665: the same two cases, each strobe an
 * SPI transaction of its own). */
#define TX_TO_IDLE_US 20u
void cc110l_tx_abort(void) {
    if (!RADIO_TX_ALLOWED) {
        return;
    }
    uint8_t m = 0;
    bool    underflow =
        cc110l_read_status_stable(CC_MARCSTATE, &m) && (m & 0x1Fu) == CC_MARC_TXFIFO_UNF;
    if (!underflow) {
        (void)radio_strobe(CC_SIDLE, NULL);
        delay_us(TX_TO_IDLE_US);
    }
    tx_permit = true;
    if (begin(CC_SFTX, NULL)) {
        end();
    }
    tx_permit = false;
    tx_active = false;
    tx.idx    = tx.total;
}

enum cc110l_tx_result cc110l_ack_prepare(const uint8_t a[MAC_ACK_LEN],
                                         const uint8_t answered[MAC_HDR_LEN]) {
    if (!RADIO_TX_ALLOWED) {
        return CC_TX_DISABLED;
    }
    ack.prepared = false;
    if (!cc110l_cal_ok()) {
        return CC_TX_NO_LOCK;
    }
    if (!pa_ok) {
        return CC_TX_NO_PA;
    }
    if (!verified) {
        last_verdict = TXGATE_OFF_PLAN;
        return CC_TX_REFUSED;
    }
    verified     = false; /* used for this ACK */
    last_verdict = judge(a, MAC_ACK_LEN, answered, 0);
    if (last_verdict != TXGATE_OK) {
        return CC_TX_REFUSED;
    }
    if (!airtime_charge(&air, timebase_us(), AIRTIME_FRAME_US(MAC_ACK_LEN))) {
        return CC_TX_DUTY;
    }
    ack.buf[0] = MAC_ACK_LEN;
    memcpy(ack.buf + 1, a, MAC_ACK_LEN);
    ack.verified_us = verified_us;
    /* The interrupt reads buf and verified_us once it sees prepared: they
     * must be in memory before that store, not merely earlier in the source. */
    __asm volatile("" ::: "memory");
    ack.prepared = true;
    return CC_TX_OK;
}

void cc110l_ack_cancel(void) {
    ack.prepared = false;
}

/* From the interrupt, after a failed STX access: the chip is IDLE (the first
 * access strobed SIDLE) with the ACK in its TX FIFO and the rest of the Data
 * frame in its RX FIFO. One access: SIDLE (nothing from IDLE; it stops a
 * state the first access did not see), SFTX (allowed in IDLE, cc110l.txt:
 * 1499-1502), SRX. The RX FIFO is kept: the task still reads the frame,
 * and the device, without an ACK, sends it again. Whether the chip did
 * reach RX the caller has the task check with MARCSTATE: a status byte
 * cannot tell, since IDLE is "also reported for some transitional states"
 * on the way to RX (cc110l.txt:1381-1385). */
static bool rx_back_from_isr(void) {
    if (!spi1_select_from_isr(SPI1_DEV_RADIO)) {
        return false;
    }
    bool ok = wait_so_low(100);
    if (ok && state_blocks_flush(more(CC_SIDLE))) {
        ok = false; /* a transmit state: the task's TX-state watch handles it */
    }
    if (ok) {
        (void)more(CC_SFTX);
        (void)more(CC_SRX);
    }
    spi1_deselect_from_isr(SPI1_DEV_RADIO);
    return ok;
}

/* GDO2 interrupt, priority configMAX_SYSCALL_INTERRUPT_PRIORITY: task-level
 * accesses to SPI1 run with it masked (src/spi1.c), so the bus is between
 * two accesses here. No mutex, no logging, bounded waits only.
 *
 * Access 1: SIDLE (stop receiving; the Data frame is complete, and what is
 * left of it stays in the RX FIFO for the task), SFTX, then the prepared
 * ACK into the TX FIFO. Access 2: STX. IDLE to TX takes 75 us without
 * calibration (MCSM0 FS_AUTOCAL = 0; cc110l.txt Table 5-11), and no clear
 * channel assessment applies from IDLE (cc110l.txt:2253-2262), which an ACK
 * must not wait for. About 35 bytes at 6 MHz: 60-70 us in total [I].
 *
 * Failure points, and the state each one leaves:
 *   - SO does not go low for access 1: no byte reached the chip, which is
 *     still receiving. CC_ACK_NONE.
 *   - The SIDLE status byte shows a transmit state: SRX in the same access
 *     (as before). CC_ACK_NONE.
 *   - Access 2 cannot be opened (bus taken, SO high): the chip is IDLE with
 *     the ACK in its TX FIFO. A third access puts it back in RX at once
 *     (rx_back_from_isr()): CC_ACK_FAILED; the caller has the radio task
 *     confirm RX with MARCSTATE. If that access fails too: CC_ACK_REARM,
 *     the caller wakes the radio task to abort and re-arm right away
 *     instead of at the next 10 s health check.
 * Every path that fails keeps tx_active false: the chip did not transmit. */
enum cc110l_ack_fire cc110l_ack_fire_from_isr(void) {
    if (!RADIO_TX_ALLOWED || !ack.prepared) {
        return CC_ACK_NONE;
    }
    ack.prepared          = false; /* at most once */
    struct txgate_radio r = shadow_radio();
    /* Not over a frame of ours that is still on the air: the chip cannot
     * report a received packet then, so that edge is not a packet's, and
     * the SFTX below would be dropped (see cc110l_tx_start()). */
    if (!shadow_full || !pa_ok || cc110l_tx_expected() || !cc110l_cal_ok()
        || (uint32_t)(timebase_us() - ack.verified_us) > ACK_VERIFY_MAX_US
        || txgate_check_channel(&r) != TXGATE_OK) {
        return CC_ACK_NONE;
    }
    if (!spi1_select_from_isr(SPI1_DEV_RADIO)) {
        return CC_ACK_NONE;
    }
    bool                 permit_before = tx_permit;
    enum cc110l_ack_fire res           = CC_ACK_NONE;
    tx_permit                          = true;
    /* CHIP_RDYn: SO is low at once unless the chip sleeps (cc110l.txt:
     * 1297-1299), which it never does here. */
    if (!wait_so_low(100)) {
        spi1_deselect_from_isr(SPI1_DEV_RADIO);
        goto out;
    }
    if (state_blocks_flush(more(CC_SIDLE))) {
        /* No ACK. If the status byte was wrong about the state (it can
         * read corrupt, cc110l.txt:1994-1996) the chip was receiving and is
         * IDLE now: SRX puts it back, with the frame still in the RX FIFO.
         * From a real transmit state the SRX is dropped, and the TX-state
         * watch of the radio task deals with it. */
        (void)more(CC_SRX);
        spi1_deselect_from_isr(SPI1_DEV_RADIO);
        goto out;
    }
    (void)more(CC_SFTX);
    (void)more(0x3F | HDR_BURST);
    for (size_t i = 0; i < sizeof ack.buf; i++) {
        (void)spi1_xfer(ack.buf[i]);
    }
    spi1_deselect_from_isr(SPI1_DEV_RADIO);

    if (spi1_select_from_isr(SPI1_DEV_RADIO)) {
        if (wait_so_low(100)) {
            tx_expect(MAC_ACK_LEN);
            (void)more(CC_STX);
            spi1_deselect_from_isr(SPI1_DEV_RADIO);
            res = CC_ACK_SENT;
            goto out;
        }
        spi1_deselect_from_isr(SPI1_DEV_RADIO);
    }
    res = rx_back_from_isr() ? CC_ACK_FAILED : CC_ACK_REARM;
out:
    tx_permit = permit_before;
    return res;
}
