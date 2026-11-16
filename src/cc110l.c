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
 *   - any write header to 0x2F-0x3F except those strobes: never PATABLE (the
 *     chip keeps its reset output power, as stock does, docs/radio.md), the
 *     TX FIFO only as a burst write while tx_permit is set; any read there
 *     except status registers and the RX FIFO;
 *   - MCSM1 with RXOFF_MODE = FSTXON or TX, or TXOFF_MODE = FSTXON or "stay
 *     in TX" (cc110l.txt:3929-3955): the only ways the state machine enters
 *     TX without an STX strobe;
 *   - PKTCTRL0 with PKT_FORMAT other than normal FIFO mode
 *     (cc110l.txt:3641-3660: random TX and serial modes).
 * The CC110L has no automatic acknowledgement and no wake-on-radio
 * (docs/datasheets/cc110l-vs-cc1101.md), so with these refused it cannot
 * transmit.
 *
 * tx_permit is set in exactly two places, cc110l_tx_start()/cc110l_tx_feed()
 * and cc110l_ack_fire_from_isr(), and only when RADIO_TX_ALLOWED is 1, the
 * whole register table has been written since the last reset, and the
 * transmit gate (src/txgate.c) has approved the frame against the frequency
 * registers in that table: off the protected channel, on the test channel,
 * our own source address, an ACK only as the answer to a frame addressed to
 * us. The bytes that go into the TX FIFO are the driver's own copy of the
 * approved frame. */
#include "cc110l.h"

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
        return;
    }
    if (read && burst && addr >= 0x30 && addr <= 0x3D) {
        return; /* status register */
    }
    if (read && addr == 0x3F) {
        return; /* RX FIFO */
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
}

/* A transmission of our own is on the air from its STX until tx_until_us. */
static volatile bool     tx_active;
static volatile uint32_t tx_until_us;

/* Airtime of a frame of n bytes: 4 preamble, 4 sync, the length byte and 2
 * CRC bytes around it at 160 us per byte (docs/radio.md "Data frame
 * plaintext"), plus IDLE to TX (75 us, cc110l.txt Table 5-11) and a margin
 * for the interrupt and task that watch for it. */
#define TX_MARGIN_US 2000u
static void tx_expect(size_t n) {
    tx_until_us = timebase_us() + (uint32_t)(11u + n) * 160u + 75u + TX_MARGIN_US;
    tx_active   = true;
}

bool cc110l_tx_expected(void) {
    if (tx_active && (int32_t)(timebase_us() - tx_until_us) >= 0) {
        tx_active = false;
    }
    return tx_active;
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

/* The prepared Enhanced ACK, the same way. prepared is the hand-over to the
 * GDO2 interrupt: the task writes buf only while it is false. */
static struct {
    uint8_t       buf[1u + MAC_ACK_LEN];
    volatile bool prepared;
} ack;

static enum txgate_verdict last_verdict = TXGATE_NO_IDENTITY;

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
    ack.prepared = false;
    tx_active    = false;
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

bool cc110l_tx_verify_channel(void) {
    uint8_t a[CC_FREQ0 - CC_CHANNR + 1u], b[2];
    if (!shadow_full) {
        return false;
    }
    if (!cc110l_read_burst(CC_CHANNR, a, sizeof a) || !cc110l_read_burst(CC_MDMCFG1, b, sizeof b)) {
        return false; /* no answer now; the table itself is not in doubt */
    }
    if (memcmp(a, shadow + CC_CHANNR, sizeof a) != 0
        || memcmp(b, shadow + CC_MDMCFG1, sizeof b) != 0) {
        shadow_full = false; /* the chip no longer holds what was written */
        return false;
    }
    return true;
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
    last_verdict = judge(frame, len, NULL, flags);
    if (last_verdict != TXGATE_OK) {
        return CC_TX_REFUSED;
    }
    cc110l_ack_cancel();
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
    bool ok = begin(CC_SIDLE, NULL);
    if (ok) {
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
        return CC_TX_SPI;
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
        return false; /* underflow: only SFTX from IDLE gets out of it (cc110l.txt:4553-4557) */
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

enum cc110l_tx_result cc110l_ack_prepare(const uint8_t a[MAC_ACK_LEN],
                                         const uint8_t answered[MAC_HDR_LEN]) {
    if (!RADIO_TX_ALLOWED) {
        return CC_TX_DISABLED;
    }
    ack.prepared = false;
    last_verdict = judge(a, MAC_ACK_LEN, answered, 0);
    if (last_verdict != TXGATE_OK) {
        return CC_TX_REFUSED;
    }
    ack.buf[0] = MAC_ACK_LEN;
    memcpy(ack.buf + 1, a, MAC_ACK_LEN);
    ack.prepared = true;
    return CC_TX_OK;
}

void cc110l_ack_cancel(void) {
    ack.prepared = false;
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
 * must not wait for. About 35 bytes at 6 MHz: 60-70 us in total [I]. */
bool cc110l_ack_fire_from_isr(void) {
    if (!RADIO_TX_ALLOWED || !ack.prepared) {
        return false;
    }
    ack.prepared          = false; /* at most once */
    struct txgate_radio r = shadow_radio();
    if (!shadow_full || txgate_check_channel(&r) != TXGATE_OK) {
        return false;
    }
    if (!spi1_select_from_isr(SPI1_DEV_RADIO)) {
        return false;
    }
    bool permit_before = tx_permit;
    tx_permit          = true;
    /* CHIP_RDYn: SO is low at once unless the chip sleeps (cc110l.txt:
     * 1297-1299), which it never does here. */
    bool ok = wait_so_low(100);
    if (ok) {
        (void)more(CC_SIDLE);
        (void)more(CC_SFTX);
        (void)more(0x3F | HDR_BURST);
        for (size_t i = 0; i < sizeof ack.buf; i++) {
            (void)spi1_xfer(ack.buf[i]);
        }
    }
    spi1_deselect_from_isr(SPI1_DEV_RADIO);
    if (ok && spi1_select_from_isr(SPI1_DEV_RADIO)) {
        ok = wait_so_low(100);
        if (ok) {
            tx_expect(MAC_ACK_LEN);
            (void)more(CC_STX);
        }
        spi1_deselect_from_isr(SPI1_DEV_RADIO);
    } else {
        ok = false;
    }
    tx_permit = permit_before;
    return ok;
}
