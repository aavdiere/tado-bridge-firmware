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
 * written to a configuration register through guard_write(). In P2 they
 * refuse everything that can transmit or prepare a transmission:
 *   - strobes other than SRES, SCAL, SRX, SIDLE, SFRX, SNOP (so no STX,
 *     SFSTXON; also no SXOFF, SPWD, SFTX, which P2 never needs);
 *   - any write header to 0x2F-0x3F except the whitelisted strobes (no TX
 *     FIFO, no PATABLE), and any read there except status registers and the
 *     RX FIFO;
 *   - MCSM1 with RXOFF_MODE = FSTXON or TX, or TXOFF_MODE = FSTXON or "stay
 *     in TX" (cc110l.txt:3929-3955): the only ways the state machine enters
 *     TX without an STX strobe;
 *   - PKTCTRL0 with PKT_FORMAT other than normal FIFO mode
 *     (cc110l.txt:3641-3660: random TX and serial modes).
 * The CC110L has no automatic acknowledgement and no wake-on-radio
 * (docs/datasheets/cc110l-vs-cc1101.md), so with these refused it cannot
 * transmit. */
#include "cc110l.h"

#include "log.h"
#include "spi1.h"
#include "timebase.h"

#define HDR_READ  0x80
#define HDR_BURST 0x40
#define HDR_ADDR  0x3F

#define CHIP_RDY_TIMEOUT_US \
    1000u /* SO goes low at once unless in SLEEP/XOFF, cc110l.txt:1297-1299 */
#define RESET_RDY_TIMEOUT_US \
    5000u /* XOSC restart after SRES; 150 us typical, cc110l.txt:1333,1356-1362 */

static volatile bool tx_state_seen;

static bool strobe_allowed(uint8_t addr) {
    switch (addr) {
        case CC_SRES:
        case CC_SCAL:
        case CC_SRX:
        case CC_SIDLE:
        case CC_SFRX:
        case CC_SNOP:
            return true;
        default:
            return false; /* SFSTXON, STX, SXOFF, SPWD, SFTX, reserved */
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
            fw_panic("radio guard: strobe refused (receive-only image)", __FILE__, __LINE__);
        }
        return;
    }
    if (read && burst && addr >= 0x30 && addr <= 0x3D) {
        return; /* status register */
    }
    if (read && addr == 0x3F) {
        return; /* RX FIFO */
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

/* STATE TX (010), FSTXON (011) and TXFIFO_UNDERFLOW (111) can only follow a
 * transmit strobe (cc110l.txt:1381-1405). A status byte of 0xFF (MISO stuck
 * high mid-byte) also reads as 111; the radio task confirms with MARCSTATE
 * before acting, so such a glitch is only counted. */
static void note_status(uint8_t status) {
    uint8_t st = CC_STATE(status);
    if (st == CC_STATE_TX || st == CC_STATE_FSTXON || st == CC_STATE_TXFIFO_UNF) {
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
        fw_panic("radio_strobe: refused (receive-only image)", __FILE__, __LINE__);
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
