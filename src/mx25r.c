/* MX25R1635F (#10, #37). Datasheet: docs/datasheets/mx25r1635f.txt
 * (Macronix PM2161 Rev. 1.6).
 *
 * Reads: RDID 0x9F (mx25r1635f.txt:960-966, 1123-1125), RDSR 0x05
 * (:1380-1394, 2647), RDSCUR 0x2B (:2488-2560, 2649) and READ 0x03 (3-byte
 * address MSB first, then data, :1656-1685). Writes: WREN 0x06 (:877-897),
 * WRDI 0x04 (:910-947), PP 0x02 (:2306-2343) and SE 0x20 (:2134-2171), and
 * nothing else: no WRSR, no block or chip erase, no WRSCUR. Every PP and SE
 * goes through guard(), which panics outside FLASH_WRITE_BEGIN..END
 * (src/flash_layout.h), the network and counter-log sectors.
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
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

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
 * 10 ms (ultra low power, mx25r1635f.txt:3337; 4 ms high performance,
 * :3432); tSE max 240 ms (:3340, :3435). WREN sets WEL at once (no cycle
 * time is given); allow a few polls. */
#define PP_TIMEOUT_US 20000u
#define SE_TIMEOUT_MS 480u
#define WEL_POLLS     4u

_Static_assert(MX25R_XFER_MAX <= FLASH_PAGE, "a program stays within one page");
_Static_assert(FLASH_WRITE_END <= FLASH_SIZE, "writable range inside the chip");

/* The hard address-range guard (#37). Everything else in this file is
 * checked again by the datasheet flow; this is not: a write outside the
 * network and counter-log sectors would destroy data this firmware does
 * not own (the update slots, the boot state, the unassigned rest). */
static void guard(uint32_t addr, size_t n) {
    if (n == 0 || addr < FLASH_WRITE_BEGIN || addr >= FLASH_WRITE_END
        || n > FLASH_WRITE_END - addr) {
        fw_panic("flash: erase/program outside the record sectors", __FILE__, __LINE__);
    }
}

/* One command byte, optional 24-bit address, then n bytes out (src) or in
 * (dst). One CS# low period. */
static void xfer(uint8_t op, const uint32_t *addr, const uint8_t *src, uint8_t *dst, size_t n) {
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

/* RDSR until WIP = 0, giving the CPU to the other tasks between polls
 * (1 ms ticks); the radio task is the caller and checks in with the
 * watchdog (bounded by max_ms; the IWDG window is ~3 s). The last status
 * read is left in *sr. */
static bool wait_idle_ms(unsigned max_ms, uint8_t *sr) {
    for (unsigned ms = 0; (*sr = rdsr()) & SR_WIP; ms++) {
        if (ms > max_ms) {
            return false;
        }
        vTaskDelay(1);
        watchdog_checkin(WDG_RADIO);
    }
    return true;
}

bool mx25r_wait_idle(void) {
    uint8_t sr;
    return wait_idle_ms(SE_TIMEOUT_MS, &sr);
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
    uint32_t t0 = timebase_us();
    uint8_t  sr;
    while ((sr = rdsr()) & SR_WIP) {
        if ((uint32_t)(timebase_us() - t0) > PP_TIMEOUT_US) {
            return false;
        }
        delay_us(20);
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
    /* 40-240 ms: give the CPU to the other tasks while the chip works. The
     * radio task is the caller, so its own FIFO is not served meanwhile
     * (docs/design.md "Counter log", erase cost). */
    if (!wait_idle_ms(SE_TIMEOUT_MS, &sr)) {
        return false;
    }
    return cycle_ok(sr, SCUR_EFAIL);
}
