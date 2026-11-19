/* MX25R1635F (#10, #37). Datasheet: docs/datasheets/mx25r1635f.txt
 * (Macronix PM2161 Rev. 1.6).
 *
 * Reads: RDID 0x9F (mx25r1635f.txt:960-966, 1123-1125), RDSR 0x05
 * (:1380-1394, 2647), RDSCUR 0x2B (:2488-2560, 2649) and READ 0x03 (3-byte
 * address MSB first, then data, :1656-1685). Writes: WREN 0x06 (:877-897),
 * WRDI 0x04 (:910-947), PP 0x02 (:2306-2343) and SE 0x20 (:2134-2171), and
 * nothing else: no WRSR, no block or chip erase, no WRSCUR. Every PP and SE
 * goes through guard(), which panics outside the area selected with
 * mx25r_area() (src/flash_layout.h: records, boot state or slots, #24).
 *
 * Program and erase follow the datasheet's flow with the fail flags
 * (mx25r1635f.txt:1281-1330, Figure 13): WREN, RDSR until WEL = 1, the
 * command, RDSR until WIP = 0, check WEL = 0, then RDSCUR P_FAIL/E_FAIL = 0.
 * A program or erase on a protected area is ignored and clears WEL
 * (:1352-1357) and sets the fail flag (:2521-2527). The caller verifies by
 * reading back (docs/design.md "Slotted records").
 *
 * The chip may be in deep power-down if the stock firmware left it there
 * (the state survives an MCU reset): a CS# low pulse of at least tCRDP =
 * 20 ns releases it, and it is in stand-by tRDP = 35 us (ultra low power)
 * or 45 us (high performance) after CS# goes high (mx25r1635f.txt:
 * 2426-2433, 3307-3314, 3402-3409). SCK limit for every command used here:
 * 33 MHz (mx25r1635f.txt:3268-3271), far above the bus's 6 MHz. */
#include "mx25r.h"

#include "flash_layout.h"
#include "log.h"
#include "spi1.h"
#include "timebase.h"

#define OP_READ   0x03
#define OP_PP     0x02
#define OP_SE     0x20
#define OP_WREN   0x06
#define OP_WRDI   0x04
#define OP_RDSR   0x05
#define OP_RDSCUR 0x2B
#define OP_RDID   0x9F

#define SR_WIP     0x01u /* mx25r1635f.txt:1347-1350, 1383 */
#define SR_WEL     0x02u /* :1352-1357 */
#define SCUR_PFAIL 0x20u /* bit 5, :2521-2523, Table 8 :2531-2540 */
#define SCUR_EFAIL 0x40u /* bit 6, :2525-2527, Table 8 */

#define T_RDP_US 50u /* >= 45 us worst case, mx25r1635f.txt:3409 */

/* Waits: twice the worst-case cycle times of either power mode. tPP max
 * 10 ms (ultra low power, mx25r1635f.txt:3337-3338; 4 ms high performance,
 * :3432-3433); tSE max 240 ms (:3340-3341, :3435-3436). WREN sets WEL at
 * once (no cycle time is given); allow a few polls. */
#define PP_TIMEOUT_US 20000u
#define SE_TIMEOUT_US 480000u
#define WEL_POLLS     4u

/* The longest transaction, a 64-byte READ or PP with its command and
 * address, takes 68 x 8 SCK periods: 91 us at 6 MHz (src/spi1.c). A frame
 * that starts while one runs has its sync word at least 8 bytes (1.28 ms)
 * and its CRC-OK edge, which fires the Enhanced ACK, at least 12 bytes
 * (1.92 ms) after its first preamble bit (src/cc110l.c airtime: 4
 * preamble, 4 sync, length, CRC 2), so a transaction started on a quiet
 * radio is over long before either edge [I]. */
_Static_assert((4u + MX25R_XFER_MAX) * 8u * 1000u / 6000u < 1280u,
               "a flash transaction ends before a frame begun under it is seen");

_Static_assert(MX25R_XFER_MAX <= FLASH_PAGE, "a program stays within one page");
_Static_assert(FLASH_BOOT_END <= FLASH_SIZE && FLASH_RECORDS_END <= FLASH_SIZE,
               "writable areas inside the chip");

/* The hard address-range guard (#37, #24). Everything else in this file is
 * checked again by the datasheet flow; this is not. One area at a time: a
 * write by the store outside the records would destroy an image or the
 * boot state, and a write by the update outside the slots or the boot
 * state could erase the counter log, after which a reboot would reuse
 * frame counters under the network key. The identity sectors lie in no
 * area. */
static enum mx25r_area area = MX25R_AREA_RECORDS;

enum mx25r_area mx25r_area(enum mx25r_area a) {
    enum mx25r_area was = area;
    area                = a;
    return was;
}

static void guard(uint32_t addr, size_t n) {
    uint32_t lo = FLASH_RECORDS_BEGIN, hi = FLASH_RECORDS_END;
    if (area == MX25R_AREA_BOOT) {
        lo = FLASH_BOOT_BEGIN;
        hi = FLASH_BOOT_END;
    } else if (area == MX25R_AREA_SLOTS) {
        lo = FLASH_SLOTS_BEGIN;
        hi = FLASH_SLOTS_END;
    } else if (area != MX25R_AREA_RECORDS) {
        lo = hi = 0; /* no such area: nothing is writable */
    }
    if (n == 0 || addr < lo || addr >= hi || n > hi - addr) {
        fw_panic("flash: erase/program outside the selected area", __FILE__, __LINE__);
    }
}

static struct mx25r_stats stats;

const struct mx25r_stats *mx25r_stats(void) {
    return &stats;
}

/* After a give-up the radio counts as stuck (a pin held high, say): no more
 * waits until it is seen quiet again, so one stuck pin costs one
 * MX25R_GATE_MAX_US, not one per transaction. The radio task's health
 * check deals with the radio after the flash work. */
static bool radio_stuck;

/* Before every bus transaction: not while the radio needs the bus quiet
 * (#22). Serves the radio meanwhile, so its frames and ACKs go on. */
static void gate(void) {
    if (!mx25r_port_radio_busy()) {
        radio_stuck = false;
        return;
    }
    if (radio_stuck) {
        return;
    }
    stats.held++;
    uint32_t t0 = timebase_us();
    uint32_t waited;
    do {
        mx25r_port_radio_serve();
        waited = timebase_us() - t0;
        if (waited > MX25R_GATE_MAX_US) {
            stats.gave_up++;
            radio_stuck = true;
            break;
        }
    } while (mx25r_port_radio_busy());
    if (waited > stats.held_max) {
        stats.held_max = waited;
    }
}

/* One command byte, optional 24-bit address, then n bytes out (src) or in
 * (dst). One CS# low period, started only on a quiet radio. */
static void xfer(uint8_t op, const uint32_t *addr, const uint8_t *src, uint8_t *dst, size_t n) {
    gate();
    spi1_lock();
    spi1_select(SPI1_DEV_FLASH);
    (void)spi1_xfer(op);
    if (addr) {
        (void)spi1_xfer((uint8_t)(*addr >> 16));
        (void)spi1_xfer((uint8_t)(*addr >> 8));
        (void)spi1_xfer((uint8_t)*addr);
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t in = spi1_xfer(src ? src[i] : 0x00);
        if (dst) {
            dst[i] = in;
        }
    }
    spi1_deselect(SPI1_DEV_FLASH);
    spi1_unlock();
}

static uint8_t rdsr(void) {
    uint8_t sr;
    xfer(OP_RDSR, NULL, NULL, &sr, 1);
    return sr;
}

static uint8_t rdscur(void) {
    uint8_t s;
    xfer(OP_RDSCUR, NULL, NULL, &s, 1);
    return s;
}

bool mx25r_wake(void) {
    gate();
    spi1_lock();
    spi1_select(SPI1_DEV_FLASH);
    delay_us(1); /* tCRDP >= 20 ns */
    spi1_deselect(SPI1_DEV_FLASH);
    spi1_unlock();
    delay_us(T_RDP_US);
    uint32_t id;
    return mx25r_read_id(&id) && id == MX25R_JEDEC_ID;
}

bool mx25r_read_id(uint32_t *id) {
    uint8_t b[3];
    xfer(OP_RDID, NULL, NULL, b, 3);
    *id = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    return true;
}

bool mx25r_read_status(uint8_t *sr) {
    *sr = rdsr();
    return true;
}

/* RDSR until WIP = 0 or timeout_us, serving the radio between polls (#22):
 * the cycle runs in the chip with CS# high (mx25r1635f.txt:2149-2151,
 * 2323-2325), and RDSR may be read during it (:1142-1144). The last status
 * read is left in *sr. */
static bool wait_wip(uint32_t timeout_us, uint8_t *sr) {
    uint32_t t0 = timebase_us();
    while ((*sr = rdsr()) & SR_WIP) {
        if ((uint32_t)(timebase_us() - t0) > timeout_us) {
            return false;
        }
        mx25r_port_radio_serve();
    }
    return true;
}

/* A program or erase cycle: wait it out and record its length. */
static bool wait_cycle(uint32_t timeout_us, uint8_t *sr) {
    uint32_t t0 = timebase_us();
    bool     ok = wait_wip(timeout_us, sr);
    uint32_t us = timebase_us() - t0;
    stats.cycles++;
    if (us > stats.cycle_max) {
        stats.cycle_max = us;
    }
    return ok;
}

bool mx25r_wait_idle(void) {
    uint8_t sr;
    return wait_wip(SE_TIMEOUT_US, &sr);
}

bool mx25r_read(uint32_t addr, uint8_t *dst, size_t n) {
    if (addr >= FLASH_SIZE || n > FLASH_SIZE - addr) {
        return false;
    }
    while (n) {
        size_t k = n < MX25R_XFER_MAX ? n : MX25R_XFER_MAX;
        xfer(OP_READ, &addr, NULL, dst, k);
        addr += (uint32_t)k;
        dst += k;
        n -= k;
    }
    return true;
}

/* Not busy, and the write enable latch set by WREN (Figure 13). */
static bool write_enable(void) {
    if (rdsr() & SR_WIP) {
        return false; /* an earlier cycle has not finished */
    }
    for (unsigned i = 0; i < WEL_POLLS; i++) {
        xfer(OP_WREN, NULL, NULL, NULL, 0);
        if (rdsr() & SR_WEL) {
            return true;
        }
    }
    return false;
}

/* After the cycle: WEL must have cleared (:1354-1357), then the fail flag. */
static bool cycle_ok(uint8_t sr, uint8_t fail_bit) {
    if (sr & (SR_WIP | SR_WEL)) {
        xfer(OP_WRDI, NULL, NULL, NULL, 0); /* leave no latch set behind */
        return false;
    }
    return (rdscur() & fail_bit) == 0;
}

bool mx25r_program(uint32_t addr, const uint8_t *src, size_t n) {
    guard(addr, n);
    if (n > MX25R_XFER_MAX || (addr % FLASH_PAGE) + n > FLASH_PAGE) {
        /* A PP that runs past the page end wraps to the page start
         * (mx25r1635f.txt:2310-2315): never send one. */
        fw_panic("flash: program crosses a page", __FILE__, __LINE__);
    }
    if (!write_enable()) {
        return false;
    }
    xfer(OP_PP, &addr, src, NULL, n);
    uint8_t sr;
    /* tPP: 3.2 ms typical, 10 ms max (mx25r1635f.txt:3337-3338). */
    if (!wait_cycle(PP_TIMEOUT_US, &sr)) {
        return false;
    }
    return cycle_ok(sr, SCUR_PFAIL);
}

bool mx25r_erase_sector(uint32_t addr) {
    guard(addr, FLASH_SECTOR);
    if (addr % FLASH_SECTOR) {
        fw_panic("flash: erase address not sector aligned", __FILE__, __LINE__);
    }
    if (!write_enable()) {
        return false;
    }
    xfer(OP_SE, &addr, NULL, NULL, 0);
    uint8_t sr;
    /* tSE: 58 ms typical, 240 ms max (mx25r1635f.txt:3340-3341); the radio
     * is served throughout (#22). */
    if (!wait_cycle(SE_TIMEOUT_US, &sr)) {
        return false;
    }
    return cycle_ok(sr, SCUR_EFAIL);
}
