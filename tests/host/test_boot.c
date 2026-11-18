/* Host tests for the bootloader's decisions (src/boot/bootctl.c) and the
 * image and boot-state formats (src/fwimg.c), #24. Models: the external
 * flash (NOR: programming only clears bits) and the F411's internal flash
 * (sectors 0-7, word programming), both with power cuts at any program or
 * erase step and with failures while the power stays on; a clock that
 * charges every flash operation and every hashed byte, to check the IWDG
 * is reloaded often enough.
 *
 * Checked on every boot that starts an application: internal flash holds
 * one whole known image, byte for byte (never a torn one); an unconfirmed
 * candidate is started at most BOOTCTL_MAX_TRIES times; the bootloader
 * wrote only the boot-state sectors of the external flash and only sectors
 * 4-7 of the internal flash; no word or byte was programmed twice without
 * an erase; the IWDG gap stayed under its bound. */
#include "bootctl.h"
#include "flash_layout.h"
#include "fwimg.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include "test.h"

#include <stdint.h>
#include <stdlib.h>

/* --- Models ------------------------------------------------------------------- */
#define INT_BASE 0x08000000u
#define INT_SIZE (512u * 1024u)

static uint8_t ext[FLASH_SIZE];
static uint8_t intf[INT_SIZE];

enum fail { FAIL_NONE, FAIL_NOTHING, FAIL_PARTIAL, FAIL_SILENT };

static struct {
    bool     in_boot; /* the bootloader runs: its write rules apply */
    long     cut_in;  /* program/erase steps until the power goes; 0: none */
    bool     dead;
    unsigned ops;
    unsigned cuts;
    unsigned int_prog_fail, int_erase_fail; /* enum fail, power on */
    unsigned ext_prog_fail;
    bool     ext_dead_reads; /* every external read fails */
    uint64_t now_us;         /* simulated time */
    uint64_t last_kick_us;
    uint64_t max_gap_us;
    unsigned int_erases, int_programs;
    unsigned ext_writes_in_boot;
    bool     full_checks; /* checksum everything outside the writable areas per boot */
    uint32_t rnd;
} m;

/* Log of program and erase steps of a survey run, to pick cut points. */
enum op_kind { OP_EXT_PROG = 1, OP_EXT_ERASE, OP_INT_ERASE, OP_INT_PROG };
static struct {
    bool     on;
    unsigned n;
    uint8_t  kind[1u << 20];
    uint32_t addr[1u << 20];
} oplog;

static uint32_t rnd(void) {
    uint32_t x = m.rnd ? m.rnd : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m.rnd = x;
    return x;
}

static bool cut_now(enum op_kind kind, uint32_t addr) {
    if (oplog.on && oplog.n < sizeof oplog.kind) {
        oplog.kind[oplog.n] = (uint8_t)kind;
        oplog.addr[oplog.n] = addr;
        oplog.n++;
    }
    m.ops++;
    if (m.cut_in > 0 && --m.cut_in == 0) {
        m.dead = true;
        m.cuts++;
        return true;
    }
    return false;
}

/* Reading and hashing: 4 MHz SPI (2 us a byte) plus SHA-256 at 16 MHz,
 * charged as 3 us a byte, on the generous side [I]. */
#define EXT_READ_US_PER_BYTE 5u
#define INT_READ_US_PER_BYTE 3u

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    CHECK(addr < FLASH_SIZE && n <= FLASH_SIZE - addr);
    if (m.dead || m.ext_dead_reads) {
        return false;
    }
    m.now_us += 10u + EXT_READ_US_PER_BYTE * n;
    memcpy(dst, ext + addr, n);
    return true;
}

static void boot_write_rule(uint32_t addr, size_t n) {
    if (m.in_boot) {
        /* The bootloader writes the boot state and nothing else. */
        CHECK(addr >= FLASH_BOOT_A && addr + n <= FLASH_BOOT_B + FLASH_SECTOR);
        m.ext_writes_in_boot++;
    }
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    boot_write_rule(addr, n);
    CHECK(n >= 1 && n <= STORE_XFER_MAX && addr % FLASH_PAGE + n <= FLASH_PAGE);
    if (m.dead) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(ext[addr + i], 0xFF);
    }
    m.now_us += 10000u; /* tPP max, mx25r1635f.txt:3337 */
    if (m.ext_prog_fail != FAIL_NONE) {
        m.ops++;
        if (m.ext_prog_fail != FAIL_NOTHING) {
            for (size_t i = 0; i < n; i++) {
                ext[addr + i] &= (uint8_t)(src[i] | rnd());
            }
        }
        return m.ext_prog_fail == FAIL_SILENT;
    }
    if (cut_now(OP_EXT_PROG, addr)) {
        size_t k = rnd() % (n + 1u);
        for (size_t i = 0; i < k; i++) {
            ext[addr + i] &= src[i];
        }
        if (k < n) {
            ext[addr + k] &= (uint8_t)(src[k] | rnd());
        }
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        ext[addr + i] &= src[i];
    }
    return true;
}

bool store_port_erase(uint32_t addr) {
    boot_write_rule(addr, FLASH_SECTOR);
    CHECK_EQ(addr % FLASH_SECTOR, 0);
    if (m.dead) {
        return false;
    }
    m.now_us += 240000u; /* tSE max, mx25r1635f.txt:3340 */
    if (cut_now(OP_EXT_ERASE, addr)) {
        for (uint32_t i = 0; i < FLASH_SECTOR; i++) {
            switch (rnd() % 4u) {
                case 0:
                    ext[addr + i] = 0xFF;
                    break;
                case 1:
                    ext[addr + i] |= (uint8_t)rnd();
                    break;
                case 2:
                    ext[addr + i] &= (uint8_t)rnd();
                    break;
                default:
                    break;
            }
        }
        return false;
    }
    memset(ext + addr, 0xFF, FLASH_SECTOR);
    return true;
}

bool bl_port_int_erase(unsigned sector) {
    CHECK(m.in_boot);
    CHECK(sector >= FWIMG_APP_FIRST_SECTOR && sector <= FWIMG_APP_LAST_SECTOR);
    if (m.dead || sector < FWIMG_APP_FIRST_SECTOR || sector > FWIMG_APP_LAST_SECTOR) {
        return false;
    }
    uint32_t a = fwimg_sector_addr(sector) - INT_BASE;
    uint32_t n = fwimg_sector_size(sector);
    /* x32 maxima: 1.1 s for 64 KB, 2 s for 128 KB (stm32f411ce.txt:5320-5330) */
    m.now_us += n == 0x10000u ? 1100000u : 2000000u;
    m.int_erases++;
    if (m.int_erase_fail != FAIL_NONE) {
        m.ops++;
        for (uint32_t i = 0; m.int_erase_fail != FAIL_NOTHING && i < n; i++) {
            intf[a + i] |= (uint8_t)rnd();
        }
        return false;
    }
    if (cut_now(OP_INT_ERASE, fwimg_sector_addr(sector))) {
        for (uint32_t i = 0; i < n; i++) {
            switch (rnd() % 3u) {
                case 0:
                    intf[a + i] = 0xFF;
                    break;
                case 1:
                    intf[a + i] |= (uint8_t)rnd();
                    break;
                default:
                    break;
            }
        }
        return false;
    }
    memset(intf + a, 0xFF, n);
    return true;
}

bool bl_port_int_program(uint32_t addr, const uint8_t *src, size_t n) {
    CHECK(m.in_boot);
    CHECK(addr >= FWIMG_APP_BASE && addr + n <= FWIMG_APP_BASE + FWIMG_APP_MAX);
    CHECK(addr % 4u == 0 && n % 4u == 0 && n > 0);
    if (m.dead || addr < FWIMG_APP_BASE || addr + n > FWIMG_APP_BASE + FWIMG_APP_MAX) {
        return false;
    }
    uint32_t a = addr - INT_BASE;
    for (size_t w = 0; w < n; w += 4u) {
        for (unsigned i = 0; i < 4u; i++) {
            CHECK_EQ(intf[a + w + i], 0xFF); /* never twice without an erase */
        }
        m.now_us += 100u; /* tprog max, stm32f411ce.txt:5305 */
        m.int_programs++;
        if (m.int_prog_fail != FAIL_NONE) {
            m.ops++;
            for (unsigned i = 0; m.int_prog_fail != FAIL_NOTHING && i < 4u; i++) {
                intf[a + w + i] &= (uint8_t)(src[w + i] | rnd());
            }
            if (m.int_prog_fail == FAIL_SILENT) {
                continue; /* reports success; the read-back must catch it */
            }
            return false;
        }
        if (cut_now(OP_INT_PROG, addr + (uint32_t)w)) {
            for (unsigned i = 0; i < 4u; i++) {
                intf[a + w + i] &= (uint8_t)(src[w + i] | rnd());
            }
            return false;
        }
        for (unsigned i = 0; i < 4u; i++) {
            intf[a + w + i] &= src[w + i];
        }
    }
    return true;
}

void bl_port_int_read(uint32_t addr, uint8_t *dst, size_t n) {
    CHECK(addr >= INT_BASE && addr + n <= INT_BASE + INT_SIZE);
    m.now_us += INT_READ_US_PER_BYTE * n;
    memcpy(dst, intf + (addr - INT_BASE), n);
}

static void kick(void) {
    uint64_t gap = m.now_us - m.last_kick_us;
    if (gap > m.max_gap_us) {
        m.max_gap_us = gap;
    }
    m.last_kick_us = m.now_us;
}

void bl_port_kick(void) {
    kick();
}

void fwimg_port_progress(void) {
    kick();
}

/* --- Images ------------------------------------------------------------------- */
#define NIMG 6
static struct img {
    uint32_t size;
    uint8_t  sha[FWIMG_SHA_LEN];
    uint8_t *bytes;
} img[NIMG];

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void sha256(const uint8_t *p, size_t n, uint8_t out[FWIMG_SHA_LEN]) {
    CHECK_EQ(mbedtls_sha256(p, n, out, 0), 0);
}

/* A made-up application of size bytes: vector table, descriptor, noise. */
static void make_img(int i, uint32_t size) {
    struct img *g = &img[i];
    free(g->bytes);
    g->size  = size;
    g->bytes = malloc(size);
    for (uint32_t k = 0; k < size; k++) {
        g->bytes[k] = (uint8_t)rnd();
    }
    put32(g->bytes, FWIMG_RAM_BASE + FWIMG_RAM_SIZE);
    put32(g->bytes + 4, (FWIMG_APP_BASE + FWIMG_HEAD_LEN + 0x40u) | 1u);
    uint8_t *d = g->bytes + FWIMG_DESC_OFFSET;
    put32(d, FWIMG_DESC_MAGIC);
    d[4] = FWIMG_DESC_VERSION;
    d[5] = 0;
    d[6] = FWIMG_DESC_LEN;
    d[7] = 0;
    put32(d + 8, FWIMG_APP_BASE + size);
    put32(d + 12, (uint32_t)i);
    sha256(g->bytes, size, g->sha);
}

/* Direct writes (the application's side, or the SWD programmer). */
static void slot_put(uint32_t slot, int i) {
    memset(ext + slot, 0xFF, FLASH_SLOT_SIZE);
    struct fwimg_hdr h = {.size = img[i].size, .fw_version = (uint32_t)i};
    memcpy(h.sha256, img[i].sha, FWIMG_SHA_LEN);
    fwimg_hdr_build(&h, ext + slot);
    memcpy(ext + slot + FLASH_SLOT_IMAGE, img[i].bytes, img[i].size);
}

static void int_put(int i) {
    memset(intf + (FWIMG_APP_BASE - INT_BASE), 0xFF, FWIMG_APP_MAX);
    if (i >= 0) {
        memcpy(intf + (FWIMG_APP_BASE - INT_BASE), img[i].bytes, img[i].size);
    }
}

/* Which known image internal flash holds, whole: its index, or -1. */
static int running(void) {
    for (int i = 0; i < NIMG; i++) {
        if (img[i].bytes
            && memcmp(intf + (FWIMG_APP_BASE - INT_BASE), img[i].bytes, img[i].size) == 0) {
            return i;
        }
    }
    return -1;
}

static void id_of(int i, uint8_t id[FWIMG_ID_LEN]) {
    memcpy(id, img[i].sha, FWIMG_ID_LEN);
}

/* The application writes the boot state (no power cut). */
static void app_state(uint8_t state, uint8_t tries, int cand, int prev) {
    struct fwimg_boot b = {.state = state, .tries = tries};
    if (cand >= 0) {
        id_of(cand, b.cand_id);
    }
    if (prev >= 0) {
        id_of(prev, b.prev_id);
    }
    bool was  = m.in_boot;
    m.in_boot = false;
    bool ok   = fwimg_boot_write(&b);
    m.in_boot = was;
    if (!m.dead) {
        CHECK(ok); /* only a power cut may stop the application's write */
    }
}

static struct fwimg_boot state_now(void) {
    struct fwimg_boot b;
    (void)fwimg_boot_load(&b);
    return b;
}

/* The application prepares an update from running image `from` to `to`:
 * candidate slot, previous slot, PENDING. */
static void app_update(int from, int to) {
    slot_put(FLASH_SLOT_CAND, to);
    slot_put(FLASH_SLOT_PREV, from);
    app_state(FWIMG_BOOT_PENDING, 0, to, from);
}

/* The bench unit's stock contents: noise where stock has data
 * (docs/design.md, #37 notes), blank elsewhere. */
static void ext_stock(void) {
    memset(ext, 0xFF, sizeof ext);
    static const uint32_t used[][2] = {
        {0x000000, 0x03E000}, {0x080000, 0x0BF000}, {0x100000, 0x101000}, {0x102000, 0x103000}};
    for (unsigned r = 0; r < 4; r++) {
        for (uint32_t a = used[r][0]; a < used[r][1]; a++) {
            ext[a] = (uint8_t)rnd();
        }
    }
}

static uint32_t sum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

/* Bootloader sectors 0-3 and everything outside the boot state must never
 * change during a boot. */
static uint32_t boot_area_sum(void) {
    return sum(intf, FWIMG_APP_BASE - INT_BASE);
}
static uint32_t ext_outside_sum(void) {
    return sum(ext, FLASH_BOOT_A) * 31u + sum(ext + FLASH_BOOT_B + FLASH_SECTOR,
                                               FLASH_SIZE - FLASH_BOOT_B - FLASH_SECTOR);
}

static struct bootctl_report rep;
static uint64_t              kick_bound_us = 4000000u; /* IWDG minimum 22.3 s; we want far less */

/* One boot: power back, no RAM state. Returns the result, or -1 if the
 * power was cut during it. */
static int boot(void) {
    m.dead         = false;
    m.in_boot      = true;
    m.last_kick_us = m.now_us;
    uint32_t b0 = 0, e0 = 0;
    if (m.full_checks) {
        b0 = boot_area_sum();
        e0 = ext_outside_sum();
    }
    enum bootctl_result r = bootctl_run(!m.ext_dead_reads, &rep);
    m.in_boot             = false;
    if (m.full_checks) {
        /* (Also enforced write by write in the ports above.) */
        CHECK_EQ(boot_area_sum(), b0);
        CHECK_EQ(ext_outside_sum(), e0);
    }
    kick();
    CHECK(m.max_gap_us <= kick_bound_us);
    if (m.dead) {
        m.dead   = false;
        m.cut_in = 0;
        return -1;
    }
    if (r == BOOTCTL_JUMP) {
        CHECK(running() >= 0); /* never a torn image */
        uint32_t size = 0;
        CHECK(fwimg_app_check(intf + (FWIMG_APP_BASE - INT_BASE), &size));
    }
    return (int)r;
}

static void reset_model(void) {
    memset(&m, 0, sizeof m);
    m.rnd         = 12345u;
    m.full_checks = true;
}

/* --- Tests -------------------------------------------------------------------- */
static void test_formats(void) {
    uint8_t raw[FWIMG_HDR_LEN];
    struct fwimg_hdr h = {.size = img[0].size, .fw_version = 7}, o;
    memcpy(h.sha256, img[0].sha, FWIMG_SHA_LEN);
    fwimg_hdr_build(&h, raw);
    CHECK(fwimg_hdr_parse(raw, &o));
    CHECK_EQ(o.size, h.size);
    CHECK_EQ(o.fw_version, 7);
    CHECK_MEM(o.sha256, h.sha256, FWIMG_SHA_LEN);
    for (unsigned i = 0; i < FWIMG_HDR_LEN * 8u; i++) {
        raw[i / 8u] ^= (uint8_t)(1u << (i % 8u));
        CHECK(!fwimg_hdr_parse(raw, &o)); /* every single bit flip is refused */
        raw[i / 8u] ^= (uint8_t)(1u << (i % 8u));
    }
    uint32_t size = 0;
    CHECK(fwimg_app_check(img[0].bytes, &size));
    CHECK_EQ(size, img[0].size);
    uint8_t head[FWIMG_HEAD_LEN];
    memcpy(head, img[0].bytes, sizeof head);
    head[FWIMG_DESC_OFFSET] ^= 1u; /* magic */
    CHECK(!fwimg_app_check(head, &size));
    memcpy(head, img[0].bytes, sizeof head);
    head[4] &= 0xFEu; /* reset vector not Thumb */
    CHECK(!fwimg_app_check(head, &size));
    memcpy(head, img[0].bytes, sizeof head);
    put32(head, 0x20020004u); /* stack above RAM */
    CHECK(!fwimg_app_check(head, &size));
    memcpy(head, img[0].bytes, sizeof head);
    put32(head + FWIMG_DESC_OFFSET + 8, FWIMG_APP_BASE + FWIMG_APP_MAX + 4u); /* too large */
    CHECK(!fwimg_app_check(head, &size));
    memset(head, 0xFF, sizeof head); /* erased */
    CHECK(!fwimg_app_check(head, &size));

    /* Sector map, rm0383.txt:2023-2031. */
    CHECK_EQ(fwimg_sector_addr(4), 0x08010000u);
    CHECK_EQ(fwimg_sector_addr(5), 0x08020000u);
    CHECK_EQ(fwimg_sector_addr(7), 0x08060000u);
    CHECK_EQ(fwimg_sector_addr(7) + fwimg_sector_size(7), 0x08080000u);

    /* Stock contents in both slots and the boot state blank: no image, no state. */
    ext_stock();
    struct fwimg_hdr x;
    CHECK(!fwimg_slot_verify(FLASH_SLOT_CAND, &x));
    CHECK(!fwimg_slot_verify(FLASH_SLOT_PREV, &x));
    CHECK_EQ(state_now().state, FWIMG_BOOT_NONE);
    slot_put(FLASH_SLOT_PREV, 0);
    CHECK(fwimg_slot_verify(FLASH_SLOT_PREV, &x));
    ext[FLASH_SLOT_PREV + FLASH_SLOT_IMAGE + img[0].size - 1u] ^= 0x10u; /* last byte */
    CHECK(!fwimg_slot_verify(FLASH_SLOT_PREV, &x));
}

/* First boot after the first P7 flash: no state, stock data in the slots. */
static void test_plain_boot(void) {
    reset_model();
    ext_stock();
    int_put(0);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 0);
    CHECK_EQ(rep.writes, 0);
    CHECK_EQ(m.ext_writes_in_boot, 0);
    CHECK_EQ(m.int_erases + m.int_programs, 0);

    /* Nothing in internal flash, nothing in the slots: halt. */
    int_put(-1);
    CHECK_EQ(boot(), BOOTCTL_HALT);
    CHECK_EQ(m.int_erases, 0);

    /* Nothing in internal flash, an image in the previous slot: put in. */
    slot_put(FLASH_SLOT_PREV, 1);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 1);
    CHECK_EQ(rep.rescue, 1);
    CHECK_EQ(rep.writes, 0);

    /* External flash dead: start what is there, or halt. */
    m.ext_dead_reads = true;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    int_put(-1);
    CHECK_EQ(boot(), BOOTCTL_HALT);
    m.ext_dead_reads = false;
}

/* Update A -> B, confirmed. */
static void test_update_confirmed(void) {
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 1);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 1);
    struct fwimg_boot b = state_now();
    CHECK_EQ(b.state, FWIMG_BOOT_TESTING);
    CHECK_EQ(b.tries, 1);
    CHECK_EQ(rep.installed, 1);
    app_state(FWIMG_BOOT_CONFIRMED, 0, 1, 0);
    unsigned w = m.ext_writes_in_boot, e = m.int_erases;
    for (int k = 0; k < 5; k++) {
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK_EQ(running(), 1);
    }
    CHECK_EQ(m.ext_writes_in_boot, w); /* confirmed: no writes at boot */
    CHECK_EQ(m.int_erases, e);
    printf("test_boot: update installed in %.2f s simulated, longest IWDG gap %.2f s\n",
           (double)m.now_us / 1e6, (double)m.max_gap_us / 1e6);
}

/* Update A -> B, B never confirms: three starts, then A is back. */
static void test_rollback(void) {
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 1);
    for (unsigned k = 1; k <= BOOTCTL_MAX_TRIES; k++) {
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK_EQ(running(), 1);
        CHECK_EQ(state_now().tries, k);
    }
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 0);
    CHECK_EQ(state_now().state, FWIMG_BOOT_ROLLED_BACK);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 0);
    CHECK_EQ(state_now().state, FWIMG_BOOT_ROLLED_BACK);
}

/* Bad slots at install time: nothing installed, A keeps running. */
static void test_rejected(void) {
    for (int which = 0; which < 4; which++) {
        reset_model();
        ext_stock();
        int_put(0);
        app_update(0, 1);
        switch (which) {
            case 0: /* candidate corrupted */
                ext[FLASH_SLOT_CAND + FLASH_SLOT_IMAGE + 1000u] ^= 4u;
                break;
            case 1: /* previous corrupted */
                ext[FLASH_SLOT_PREV + FLASH_SLOT_IMAGE + 5u] ^= 4u;
                break;
            case 2: /* candidate whole, but not the one named */
                slot_put(FLASH_SLOT_CAND, 2);
                break;
            default: /* header CRC */
                ext[FLASH_SLOT_CAND + 12u] ^= 1u;
                break;
        }
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK_EQ(running(), 0);
        CHECK_EQ(state_now().state, FWIMG_BOOT_REJECTED);
        CHECK_EQ(m.int_erases + m.int_programs, 0);
    }
}

/* B fails its test and the previous slot is gone: B is kept. */
static void test_no_rollback(void) {
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 1);
    for (unsigned k = 1; k <= BOOTCTL_MAX_TRIES; k++) {
        CHECK_EQ(boot(), BOOTCTL_JUMP);
    }
    ext[FLASH_SLOT_PREV + FLASH_SLOT_IMAGE + 77u] ^= 1u;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 1);
    CHECK_EQ(state_now().state, FWIMG_BOOT_NO_ROLLBACK);
}

/* Under test, the developer writes another image over SWD: no rollback. */
static void test_external(void) {
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 1);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    int_put(2);
    for (int k = 0; k < 5; k++) {
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK_EQ(running(), 2);
    }
    CHECK_EQ(state_now().state, FWIMG_BOOT_EXTERNAL);
}

/* Internal flash failing with the power on: bounded, ends started or halted. */
static void test_int_failures(void) {
    for (unsigned f = 0; f < 6; f++) {
        reset_model();
        ext_stock();
        int_put(0);
        app_update(0, 1);
        if (f < 3) {
            m.int_prog_fail = FAIL_NOTHING + f;
        } else {
            m.int_erase_fail = FAIL_NOTHING + (f - 3u);
            if (m.int_erase_fail == FAIL_SILENT + 0u) {
                m.int_erase_fail = FAIL_PARTIAL;
            }
        }
        int r = boot();
        /* Every install failed. An erase that changed nothing leaves A
         * whole (its commit mark too), so A starts; otherwise nothing is
         * left: halt. */
        if (m.int_erase_fail == FAIL_NOTHING) {
            CHECK_EQ(r, BOOTCTL_JUMP);
            CHECK_EQ(running(), 0);
        } else {
            CHECK_EQ(r, BOOTCTL_HALT);
        }
        CHECK(rep.installs <= 2u * BOOTCTL_MAX_TRIES + 2u);
        /* The flash recovers (or the part is replaced): the next boot finds
         * a way back without help. */
        m.int_prog_fail = m.int_erase_fail = FAIL_NONE;
        r                                  = boot();
        CHECK_EQ(r, BOOTCTL_JUMP);
        CHECK(running() == 0 || running() == 1);
    }
}

/* The boot state cannot be written: the bootloader still starts something
 * whole, and never loops. */
static void test_state_unwritable(void) {
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 1);
    m.ext_prog_fail = FAIL_NOTHING;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 0); /* PENDING could not be counted: no install */
    CHECK_EQ(m.int_erases, 0);
    m.ext_prog_fail = FAIL_NONE;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 1);
}

/* Run boots with no more cuts until things settle, with an application
 * that confirms the candidate if good_cand. Returns the image running at
 * the end. Counts unconfirmed starts of cand. */
static int settle(int cand, bool good_cand, unsigned *starts) {
    int last = -2;
    for (int k = 0; k < 12; k++) {
        int r = boot();
        CHECK(r == BOOTCTL_JUMP);
        if (r != BOOTCTL_JUMP) {
            return -1;
        }
        last              = running();
        struct fwimg_boot b = state_now();
        if (last == cand && b.state == FWIMG_BOOT_TESTING) {
            ++*starts;
            if (good_cand) {
                app_state(FWIMG_BOOT_CONFIRMED, 0, cand, -1);
            }
        }
    }
    CHECK(*starts <= BOOTCTL_MAX_TRIES);
    return last;
}

/* Cut points worth trying: every external program and erase, every
 * internal erase, the first and last word of every internal program chunk,
 * the descriptor magic (the commit mark) and its neighbour, and every 97th
 * step besides. */
static bool cut_point(unsigned k) {
    if (k == 0 || k > oplog.n) {
        return false;
    }
    uint32_t a = oplog.addr[k - 1u], off = a - FWIMG_APP_BASE;
    return oplog.kind[k - 1u] != OP_INT_PROG || off % 256u == 0 || off % 256u == 252u
           || off == FWIMG_DESC_OFFSET || off == FWIMG_DESC_OFFSET + 4u || k % 97u == 0;
}

static uint8_t saved_boot[2u * FLASH_SECTOR];
static uint8_t saved_app[FWIMG_APP_MAX];

static void save_setup(void) {
    memcpy(saved_boot, ext + FLASH_BOOT_A, sizeof saved_boot);
    memcpy(saved_app, intf + (FWIMG_APP_BASE - INT_BASE), sizeof saved_app);
}
/* Only the boot state and the application region can change in a boot
 * (the ports check every write), so only they are put back. */
static void restore_setup(void) {
    memcpy(ext + FLASH_BOOT_A, saved_boot, sizeof saved_boot);
    memcpy(intf + (FWIMG_APP_BASE - INT_BASE), saved_app, sizeof saved_app);
}

/* A power cut at every step of interest of an update A -> B, through to
 * B confirmed (good) or rolled back (bad), with 4 tears each. The images
 * are small (A in sector 4 only, B across sectors 4 and 5) to keep the
 * run short. */
static void test_cut_everywhere(void) {
    unsigned cases = 0, ended_new = 0, ended_old = 0, cut_kind[5] = {0};
    make_img(0, 9000u);
    make_img(1, 70000u);
    for (int good = 0; good < 2; good++) {
        reset_model();
        ext_stock();
        int_put(0);
        app_update(0, 1);
        save_setup();
        /* Survey: the uncut sequence, logging every step. */
        oplog.on = true;
        oplog.n  = 0;
        unsigned s0 = 0;
        (void)settle(1, good, &s0);
        oplog.on       = false;
        unsigned steps = oplog.n;
        for (unsigned k = 1; k <= steps; k++) {
            if (!cut_point(k)) {
                continue;
            }
            for (unsigned tear = 0; tear < 4; tear++) {
                uint64_t now = m.now_us;
                reset_model();
                m.full_checks = false;
                m.now_us      = now;
                m.rnd         = 1000u + k * 7u + tear;
                restore_setup();
                m.cut_in = (long)k;
                /* Boots with the cut armed until it fires. */
                unsigned starts = 0;
                for (int b = 0; b < 12 && m.cut_in > 0; b++) {
                    int r = boot();
                    if (r == BOOTCTL_JUMP && running() == 1
                        && state_now().state == FWIMG_BOOT_TESTING) {
                        starts++;
                        if (good) {
                            app_state(FWIMG_BOOT_CONFIRMED, 0, 1, -1);
                        }
                    }
                }
                CHECK_EQ(m.cuts, 1);
                int end = settle(1, good, &starts);
                /* A good candidate ends up running, a bad one is rolled back. */
                CHECK_EQ(end, good ? 1 : 0);
                ended_new += end == 1;
                ended_old += end == 0;
                cut_kind[oplog.kind[k - 1u]]++;
                cases++;
            }
        }
    }
    printf("test_boot: power cut at %u steps (external program %u, erase %u; internal erase %u, "
           "program %u), ended on the new image %u, the old %u\n",
           cases, cut_kind[OP_EXT_PROG], cut_kind[OP_EXT_ERASE], cut_kind[OP_INT_ERASE],
           cut_kind[OP_INT_PROG], ended_new, ended_old);
    make_img(0, 161392u);
    make_img(1, 170000u);
}

/* Many updates with random cuts and random good or bad candidates. */
static void test_random(void) {
    reset_model();
    m.full_checks = false;
    m.rnd         = 777u;
    ext_stock();
    int_put(0);
    int      cur = 0, cuts = 0, updates = 0, rolled = 0, given_up = 0;
    unsigned boots = 0;
    for (int u = 0; u < 100; u++) {
        int  to   = (cur + 1 + (int)(rnd() % (NIMG - 1))) % NIMG;
        bool good = rnd() % 3u != 0;
        app_update(cur, to);
        updates++;
        unsigned starts = 0;
        int      last   = -1;
        for (int b = 0; b < 16; b++) {
            if (rnd() % 4u == 0) {
                m.cut_in = (long)(1u + rnd() % 400u);
            }
            int r = boot();
            boots++;
            if (r < 0) {
                cuts++;
                continue;
            }
            m.cut_in = 0;
            CHECK_EQ(r, BOOTCTL_JUMP);
            last = running();
            CHECK(last == cur || last == to);
            if (last == to && state_now().state == FWIMG_BOOT_TESTING) {
                starts++;
                if (good) {
                    app_state(FWIMG_BOOT_CONFIRMED, 0, to, cur);
                }
            }
        }
        m.cut_in = 0;
        last     = settle(to, good, &starts);
        /* A good candidate ends up running and a bad one is rolled back,
         * unless the update was given up: its install was cut short
         * BOOTCTL_MAX_TRIES times before the candidate ever started. */
        uint8_t st        = state_now().state;
        bool    abandoned = starts == 0 && last == cur
                         && (st == FWIMG_BOOT_ROLLED_BACK || st == FWIMG_BOOT_NO_ROLLBACK);
        CHECK(last == (good ? to : cur) || abandoned);
        rolled += last == cur && !abandoned;
        given_up += abandoned;
        cur = last;
    }
    printf("test_boot: random: %d updates (%d rolled back, %d given up after cut installs), %u "
           "boots, %d cut, longest IWDG gap %.2f s\n",
           updates, rolled, given_up, boots, cuts, (double)m.max_gap_us / 1e6);
}

/* The largest image: install time and the IWDG gap. */
static void test_largest(void) {
    make_img(5, FWIMG_APP_MAX);
    reset_model();
    ext_stock();
    int_put(0);
    app_update(0, 5);
    uint64_t t0 = m.now_us;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK_EQ(running(), 5);
    printf("test_boot: 448 KB image installed in %.1f s simulated (maxima), longest IWDG gap "
           "%.2f s\n",
           (double)(m.now_us - t0) / 1e6, (double)m.max_gap_us / 1e6);
    make_img(5, 100000u);
}

int main(void) {
    m.rnd = 4242u;
    make_img(0, 161392u);
    make_img(1, 170000u);
    make_img(2, 152000u);
    make_img(3, 70000u); /* fits sector 4 and part of 5 */
    make_img(4, 200004u);
    make_img(5, 100000u);
    test_formats();
    test_plain_boot();
    test_update_confirmed();
    test_rollback();
    test_rejected();
    test_no_rollback();
    test_external();
    test_int_failures();
    test_state_unwritable();
    test_largest();
    test_cut_everywhere();
    test_random();
    for (int i = 0; i < NIMG; i++) {
        free(img[i].bytes);
    }
    return TEST_REPORT("test_boot");
}
