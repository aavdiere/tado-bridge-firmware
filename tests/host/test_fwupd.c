/* Host tests for the application's side of an update (#24, src/fwupd.c),
 * with the real image and boot-state formats (src/fwimg.c, src/store.c)
 * and the real bootloader decisions (src/boot/bootctl.c) on the same flash
 * models, so that an update runs end to end: receive, save, PENDING,
 * install, TESTING, confirm or roll back.
 *
 * Models: the external flash (NOR: a program only clears bits; power cuts
 * that tear a program or an erase; programs that fail; reads that return a
 * flipped bit) and the F411's internal flash for the bootloader. Rules
 * checked on every write:
 *   - the application (fwupd) programs and erases only the two update
 *     slots and the boot-state sectors, never the identity, the network
 *     record or the counter log, and never internal flash;
 *   - the bootloader writes only the boot-state sectors and sectors 4-7;
 *   - no byte is programmed twice without an erase.
 * Checked after every power cut: the next boot starts a whole image, the old
 * one unless PENDING had been written in full, and a new upload then
 * succeeds. */
#include "fwupd.h"

#include "bootctl.h"
#include "coap.h"
#include "flash_layout.h"
#include "fwimg.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include "test.h"

#include <stdlib.h>

/* --- Models ---------------------------------------------------------------------- */
#define INT_BASE 0x08000000u
#define INT_SIZE (512u * 1024u)
#define APP_OFF  (FWIMG_APP_BASE - INT_BASE)

static uint8_t ext[FLASH_SIZE];
static uint8_t intf[INT_SIZE];

static struct {
    bool     in_boot;      /* the bootloader runs: its write rules apply */
    long     cut_in;       /* program/erase steps until the power goes; 0: none */
    bool     dead;         /* the power is gone: every access fails */
    unsigned steps;        /* program and erase steps of the application */
    long     flip_read_in; /* flip a bit in the n-th slot read from now; 0: none */
    long     prog_fail_in; /* the n-th program from now leaves a byte unprogrammed */
    unsigned app_programs, app_erases, ext_reads;
    unsigned int_writes_by_app;
    bool     net_on; /* what the radio task reports: a network is active */
    unsigned restarts;
    int      locked;
    uint32_t rnd;
} m;

static uint32_t rnd(void) {
    uint32_t x = m.rnd ? m.rnd : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m.rnd = x;
    return x;
}

/* The area fwupd_op_run() selected (as the flash driver's guard sees it). */
static enum fwupd_area area = FWUPD_AREA_DEFAULT;

void fwupd_port_area(enum fwupd_area a) {
    area = a;
}

_Static_assert(FLASH_SLOTS_BEGIN == 0, "the slots start at 0");
static void write_rule(uint32_t addr, size_t n) {
    if (m.in_boot) {
        CHECK(addr >= FLASH_BOOT_BEGIN && addr + n <= FLASH_BOOT_END);
    } else if (area == FWUPD_AREA_SLOTS) {
        CHECK(addr + n <= FLASH_SLOTS_END);
    } else if (area == FWUPD_AREA_BOOT) {
        CHECK(addr >= FLASH_BOOT_BEGIN && addr + n <= FLASH_BOOT_END);
    } else {
        CHECK(false); /* the update writes nothing in the default area */
    }
}

static bool cut_now(void) {
    if (m.in_boot) {
        return false; /* cuts are aimed at the application's steps */
    }
    m.steps++;
    if (m.cut_in > 0 && --m.cut_in == 0) {
        m.dead = true;
        return true;
    }
    return false;
}

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    CHECK(addr < FLASH_SIZE && n <= FLASH_SIZE - addr);
    if (m.dead) {
        return false;
    }
    memcpy(dst, ext + addr, n);
    m.ext_reads++;
    if (!m.in_boot && m.flip_read_in > 0 && addr < FLASH_SLOTS_END && --m.flip_read_in == 0) {
        dst[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8u));
    }
    return true;
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    write_rule(addr, n);
    CHECK(n >= 1 && n <= STORE_XFER_MAX && addr % FLASH_PAGE + n <= FLASH_PAGE);
    if (m.dead) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(ext[addr + i], 0xFF); /* never twice without an erase */
    }
    if (!m.in_boot) {
        m.app_programs++;
    }
    if (cut_now()) {
        size_t k = rnd() % (n + 1u);
        for (size_t i = 0; i < k; i++) {
            ext[addr + i] &= src[i];
        }
        if (k < n) {
            ext[addr + k] &= (uint8_t)(src[k] | rnd()); /* a torn byte */
        }
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        ext[addr + i] &= src[i];
    }
    if (!m.in_boot && m.prog_fail_in > 0 && --m.prog_fail_in == 0) {
        /* A silent miss: one byte left erased; only the read-back sees it. */
        for (size_t j = 0, i = rnd() % n; j < n; j++, i = (i + 1u) % n) {
            if (src[i] != 0xFFu) {
                ext[addr + i] = 0xFF;
                break;
            }
        }
    }
    return true;
}

bool store_port_erase(uint32_t addr) {
    write_rule(addr, FLASH_SECTOR);
    CHECK_EQ(addr % FLASH_SECTOR, 0);
    if (m.dead) {
        return false;
    }
    if (!m.in_boot) {
        m.app_erases++;
    }
    if (cut_now()) {
        size_t k = rnd() % (FLASH_SECTOR + 1u);
        memset(ext + addr, 0xFF, k);
        return false;
    }
    memset(ext + addr, 0xFF, FLASH_SECTOR);
    return true;
}

void fwimg_port_progress(void) {}

bool bl_port_int_erase(unsigned sector) {
    if (!m.in_boot) {
        m.int_writes_by_app++;
    }
    CHECK(sector >= 4u && sector <= 7u);
    memset(intf + (fwimg_sector_addr(sector) - INT_BASE), 0xFF, fwimg_sector_size(sector));
    return true;
}

bool bl_port_int_program(uint32_t addr, const uint8_t *src, size_t n) {
    if (!m.in_boot) {
        m.int_writes_by_app++;
    }
    CHECK(addr >= FWIMG_APP_BASE && addr + n <= INT_BASE + INT_SIZE && addr % 4u == 0);
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(intf[addr - INT_BASE + i], 0xFF);
        intf[addr - INT_BASE + i] &= src[i];
    }
    return true;
}

void bl_port_int_read(uint32_t addr, uint8_t *dst, size_t n) {
    memcpy(dst, intf + (addr - INT_BASE), n);
}

void bl_port_kick(void) {}

/* --- fwupd port: the radio task's side, run in place ----------------------------- */
bool fwupd_port_op(struct fwupd_op *op) {
    CHECK_EQ(m.locked, 0); /* no flash work under the status lock */
    op->network = m.net_on;
    if (op->kind == FWUPD_OP_RESTART) {
        m.restarts++;
        op->ok = true;
    } else {
        CHECK_EQ(area, FWUPD_AREA_DEFAULT);
        (void)fwupd_op_run(op);
        CHECK_EQ(area, FWUPD_AREA_DEFAULT); /* back to the store's area after every operation */
    }
    op->done = true;
    return op->ok;
}

void fwupd_port_lock(void) {
    CHECK_EQ(m.locked, 0);
    m.locked++;
}

void fwupd_port_unlock(void) {
    CHECK_EQ(m.locked, 1);
    m.locked--;
}

/* --- Images ------------------------------------------------------------------------ */
/* An application image as the linker lays it out (cmake/sections.ld): stack
 * pointer, reset vector after the descriptor, the descriptor at 0x200, then
 * bytes from seed. */
static void make_image(uint8_t *img, uint32_t size, uint32_t seed) {
    uint32_t r = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < size; i++) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        img[i] = (uint8_t)r;
    }
    uint32_t w[] = {0x20020000u, FWIMG_APP_BASE + FWIMG_HEAD_LEN + 0x41u};
    memcpy(img, w, sizeof w);
    struct fwimg_desc d = {.magic      = FWIMG_DESC_MAGIC,
                           .version    = FWIMG_DESC_VERSION,
                           .len        = FWIMG_DESC_LEN,
                           .image_end  = FWIMG_APP_BASE + size,
                           .fw_version = seed};
    memcpy(img + FWIMG_DESC_OFFSET, &d, sizeof d);
}

static void sha(const uint8_t *p, uint32_t n, uint8_t out[FWIMG_SHA_LEN]) {
    CHECK_EQ(mbedtls_sha256(p, n, out, 0), 0);
}

#define IMG_MAX FWIMG_APP_MAX
static uint8_t img_a[IMG_MAX], img_b[IMG_MAX], img_c[IMG_MAX];

/* Internal flash holds img (as the first flash over SWD leaves it). */
static void install_int(const uint8_t *img, uint32_t size) {
    memset(intf, 0xFF, sizeof intf);
    memcpy(intf + APP_OFF, img, size);
}

static bool int_is(const uint8_t *img, uint32_t size) {
    return memcmp(intf + APP_OFF, img, size) == 0;
}

static uint32_t int_size(void) {
    uint32_t s = 0;
    return fwimg_app_check(intf + APP_OFF, &s) ? s : 0;
}

/* The application starts: the running image is what internal flash holds. */
static void app_boot(uint32_t now_ms) {
    uint32_t s = int_size();
    CHECK(s != 0);
    fwupd_init(intf + APP_OFF, s, 0, now_ms);
}

/* A boot of the bootloader on the models. */
static enum bootctl_result boot(void) {
    struct bootctl_report r;
    m.in_boot              = true;
    enum bootctl_result rc = bootctl_run(true, &r);
    m.in_boot              = false;
    return rc;
}

static struct fwimg_boot state(void) {
    struct fwimg_boot b;
    m.in_boot = true; /* a read only, outside the application's counters */
    (void)fwimg_boot_load(&b);
    m.in_boot = false;
    return b;
}

/* Fresh flash: the stock firmware's data in both slots, records and
 * identity filled with something to watch, no boot state. */
static uint32_t fixed_sum(void) {
    uint32_t h = 2166136261u;
    for (uint32_t a = FLASH_SLOTS_END; a < FLASH_BOOT_BEGIN; a++) {
        h = (h ^ ext[a]) * 16777619u;
    }
    for (uint32_t a = FLASH_BOOT_END; a < FLASH_SIZE; a++) {
        h = (h ^ ext[a]) * 16777619u;
    }
    return h;
}

static uint32_t fixed0;

static void reset_models(void) {
    uint32_t keep = m.rnd;
    memset(&m, 0, sizeof m);
    m.rnd    = keep;
    m.net_on = true;
    for (uint32_t a = 0; a < FLASH_SIZE; a++) {
        ext[a] = (uint8_t)(rnd() | 0x01u); /* stock data, never all 0xFF */
    }
    memset(ext + FLASH_SLOT_CAND + 0x3E000u, 0xFF, FLASH_SLOT_SIZE - 0x3E000u);
    memset(ext + FLASH_BOOT_BEGIN, 0xFF, FLASH_BOOT_END - FLASH_BOOT_BEGIN);
    fixed0 = fixed_sum();
}

/* The transfer body as tools/update/upload.py sends it: block 0 the header
 * and zeros, then the image. */
static uint8_t blk[FWUPD_BLOCK];

static void block0(const uint8_t *img, uint32_t size, uint8_t out[FWUPD_BLOCK]) {
    struct fwimg_hdr h = {.size = size, .fw_version = 7};
    sha(img, size, h.sha256);
    memset(out, 0, FWUPD_BLOCK);
    fwimg_hdr_build(&h, out);
}

static uint32_t nblocks(uint32_t size) {
    return (size + FWUPD_BLOCK - 1u) / FWUPD_BLOCK;
}

static enum fwupd_rc send(const uint8_t *img, uint32_t size, uint32_t num, uint32_t now) {
    if (num == 0) {
        block0(img, size, blk);
        return fwupd_block(0, true, blk, FWUPD_BLOCK, true, now);
    }
    uint32_t off = (num - 1u) * FWUPD_BLOCK;
    uint32_t n   = size - off < FWUPD_BLOCK ? size - off : FWUPD_BLOCK;
    memcpy(blk, img + off, n);
    return fwupd_block(num, num < nblocks(size), blk, n, true, now);
}

/* The whole upload; the result of the last block sent (stops at the first
 * answer that is not CONTINUE). */
static enum fwupd_rc upload(const uint8_t *img, uint32_t size, uint32_t now) {
    enum fwupd_rc rc = FWUPD_CONTINUE;
    for (uint32_t num = 0; num <= nblocks(size) && rc == FWUPD_CONTINUE; num++) {
        rc = send(img, size, num, now);
    }
    return rc;
}

static bool slot_holds(uint32_t slot, const uint8_t *img, uint32_t size) {
    struct fwimg_hdr h;
    uint8_t          s[FWIMG_SHA_LEN];
    m.in_boot = true;
    bool ok   = fwimg_slot_verify(slot, &h);
    m.in_boot = false;
    sha(img, size, s);
    return ok && h.size == size && memcmp(h.sha256, s, FWIMG_SHA_LEN) == 0;
}

static void id_of(const uint8_t *img, uint32_t size, uint8_t id[FWIMG_ID_LEN]) {
    uint8_t s[FWIMG_SHA_LEN];
    sha(img, size, s);
    memcpy(id, s, FWIMG_ID_LEN);
}

/* Ticks of 1 s from t, healthy or not, until an event or until t + ms. */
static enum fwupd_event ticks(uint32_t *t, uint32_t ms, bool healthy) {
    for (uint32_t end = *t + ms; *t < end;) {
        *t += 1000u;
        enum fwupd_event e = fwupd_tick(*t, healthy);
        if (e != FWUPD_EV_NONE) {
            return e;
        }
    }
    return FWUPD_EV_NONE;
}

/* --- Tests -------------------------------------------------------------------------- */
static void test_ops_allowed(void) {
    uint8_t         b[FWUPD_READ_MAX];
    struct fwupd_op o;
#define OP(...) (o = (struct fwupd_op){__VA_ARGS__}, fwupd_op_allowed(&o))
    CHECK(OP(.kind = FWUPD_OP_READ, .addr = FLASH_SLOT_CAND, .len = 1024, .dst = b));
    CHECK(OP(.kind = FWUPD_OP_READ,
             .addr = FLASH_SLOT_PREV + FLASH_SLOT_SIZE - 1024u,
             .len  = 1024,
             .dst  = b));
    CHECK(!OP(.kind = FWUPD_OP_READ, .addr = FLASH_SLOT_CAND, .len = 1025, .dst = b));
    CHECK(!OP(.kind = FWUPD_OP_READ, .addr = FLASH_SLOT_PREV - 512u, .len = 1024, .dst = b));
    CHECK(!OP(.kind = FWUPD_OP_READ, .addr = FLASH_ID_A, .len = 16, .dst = b));
    CHECK(!OP(.kind = FWUPD_OP_READ, .addr = FLASH_SLOT_CAND, .len = 0, .dst = b));
    CHECK(!OP(.kind = FWUPD_OP_READ, .addr = FLASH_SLOT_CAND, .len = 4, .dst = NULL));
    CHECK(OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_SLOT_PREV + 0x7F000u));
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_SLOT_PREV + 0x80000u)); /* = identity A */
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_SLOT_CAND + 0x800u));
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_NET_A));
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_CTR_B));
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = FLASH_BOOT_A));
    CHECK(!OP(.kind = FWUPD_OP_ERASE, .addr = 0xFFFFF000u));
    CHECK(OP(.kind = FWUPD_OP_PROGRAM, .addr = 0x1100u, .len = 256, .src = b));
    CHECK(!OP(.kind = FWUPD_OP_PROGRAM, .addr = 0x1101u, .len = 256, .src = b)); /* crosses */
    CHECK(!OP(.kind = FWUPD_OP_PROGRAM, .addr = FLASH_ID_B, .len = 4, .src = b));
    CHECK(!OP(.kind = FWUPD_OP_PROGRAM, .addr = FLASH_BOOT_B, .len = 4, .src = b));
    CHECK(!OP(.kind = FWUPD_OP_PROGRAM, .addr = FLASH_NET_B, .len = 4, .src = b));
    CHECK(OP(.kind = FWUPD_OP_BOOT_LOAD));
    CHECK(OP(.kind = FWUPD_OP_BOOT_WRITE, .boot = {.state = FWIMG_BOOT_PENDING}));
    CHECK(OP(.kind = FWUPD_OP_BOOT_WRITE, .boot = {.state = FWIMG_BOOT_CONFIRMED}));
    CHECK(!OP(.kind = FWUPD_OP_BOOT_WRITE, .boot = {.state = FWIMG_BOOT_PENDING, .tries = 1}));
    for (uint8_t s = 0; s <= FWIMG_BOOT_EXTERNAL + 1u; s++) {
        if (s != FWIMG_BOOT_PENDING && s != FWIMG_BOOT_CONFIRMED) {
            CHECK(!OP(.kind = FWUPD_OP_BOOT_WRITE, .boot = {.state = s}));
        }
    }
    CHECK(!OP(.kind = FWUPD_OP_RESTART));
    CHECK(!OP(.kind = 0));
    CHECK(!OP(.kind = 99));
    /* Refused: no flash access at all. */
    reset_models();
    unsigned reads = m.ext_reads;
    o              = (struct fwupd_op){.kind = FWUPD_OP_ERASE, .addr = FLASH_CTR_A};
    CHECK(!fwupd_op_run(&o) && !o.ok);
    o = (struct fwupd_op){.kind = FWUPD_OP_PROGRAM, .addr = FLASH_ID_A, .len = 4, .src = b};
    CHECK(!fwupd_op_run(&o));
    o = (struct fwupd_op){.kind = FWUPD_OP_BOOT_WRITE, .boot = {.state = FWIMG_BOOT_ROLLBACK}};
    CHECK(!fwupd_op_run(&o));
    CHECK_EQ(m.app_erases + m.app_programs, 0);
    CHECK_EQ(m.ext_reads, reads);
    CHECK_EQ(fixed_sum(), fixed0);
#undef OP
}

/* A: first flash. B: updated and confirmed. C: never confirms, rolled
 * back to B. Then C again with B already saved, and D after a refused
 * candidate. */
#define SZ_A 23552u /* 23 KB: six sectors, the last one partly */
#define SZ_B 30724u /* not a whole block */
#define SZ_C 16384u
static void test_update_cycle(void) {
    reset_models();
    make_image(img_a, SZ_A, 1);
    make_image(img_b, SZ_B, 2);
    make_image(img_c, SZ_C, 3);
    install_int(img_a, SZ_A);
    CHECK_EQ(boot(), BOOTCTL_JUMP); /* no boot state: nothing written */
    CHECK_EQ(state().state, FWIMG_BOOT_NONE);
    uint32_t t = 1000;
    app_boot(t);
    struct fwupd_info in;
    fwupd_info(&in);
    CHECK(!in.testing && !in.active);

    /* A -> B. */
    CHECK_EQ(upload(img_b, SZ_B, t), FWUPD_DONE);
    CHECK_EQ(m.int_writes_by_app, 0);
    CHECK_EQ(fixed_sum(), fixed0);
    CHECK(slot_holds(FLASH_SLOT_CAND, img_b, SZ_B));
    CHECK(slot_holds(FLASH_SLOT_PREV, img_a, SZ_A));
    struct fwimg_boot b = state();
    uint8_t           ida[FWIMG_ID_LEN], idb[FWIMG_ID_LEN], idc[FWIMG_ID_LEN];
    id_of(img_a, SZ_A, ida);
    id_of(img_b, SZ_B, idb);
    id_of(img_c, SZ_C, idc);
    CHECK_EQ(b.state, FWIMG_BOOT_PENDING);
    CHECK_EQ(b.tries, 0);
    CHECK_MEM(b.cand_id, idb, FWIMG_ID_LEN);
    CHECK_MEM(b.prev_id, ida, FWIMG_ID_LEN);
    fwupd_info(&in);
    CHECK(in.done && !in.active);
    /* The last block's answer again: the same answer; another block: no. */
    CHECK_EQ(send(img_b, SZ_B, nblocks(SZ_B), t), FWUPD_DONE);
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_REFUSED);
    CHECK_EQ(ticks(&t, 2000, true), FWUPD_EV_NONE);
    CHECK_EQ(ticks(&t, 2000, true), FWUPD_EV_RESTART); /* FWUPD_RESTART_MS after the last block */

    /* The bootloader installs B; B starts under test and confirms after
     * 60 s healthy, not before, and an unhealthy tick starts the count again. */
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, SZ_B));
    CHECK_EQ(state().state, FWIMG_BOOT_TESTING);
    CHECK_EQ(state().tries, 1);
    t += 2000;
    app_boot(t);
    fwupd_info(&in);
    CHECK(in.testing);
    CHECK_EQ(send(img_c, SZ_C, 0, t), FWUPD_REFUSED); /* no update while under test */
    CHECK_EQ(ticks(&t, 40000, true), FWUPD_EV_NONE);
    CHECK_EQ(ticks(&t, 1000, false), FWUPD_EV_NONE);
    CHECK_EQ(ticks(&t, 59000, true), FWUPD_EV_NONE);
    CHECK_EQ(ticks(&t, 2000, true), FWUPD_EV_CONFIRMED);
    b = state();
    CHECK_EQ(b.state, FWIMG_BOOT_CONFIRMED);
    CHECK_MEM(b.cand_id, idb, FWIMG_ID_LEN);
    fwupd_info(&in);
    CHECK(!in.testing);
    CHECK_EQ(ticks(&t, 700000, true), FWUPD_EV_NONE); /* no restart once confirmed */
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, SZ_B));
    CHECK_EQ(state().state, FWIMG_BOOT_CONFIRMED);
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_REFUSED); /* the running image */

    /* B -> C: B is saved into the previous slot. C never gets healthy: it
     * restarts itself after 10 min, three times, and the bootloader puts
     * B back. */
    app_boot(t);
    CHECK_EQ(upload(img_c, SZ_C, t), FWUPD_DONE);
    CHECK(slot_holds(FLASH_SLOT_PREV, img_b, SZ_B));
    CHECK(slot_holds(FLASH_SLOT_CAND, img_c, SZ_C));
    for (unsigned k = 1; k <= BOOTCTL_MAX_TRIES; k++) {
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK(int_is(img_c, SZ_C));
        CHECK_EQ(state().state, FWIMG_BOOT_TESTING);
        CHECK_EQ(state().tries, k);
        t += 3000;
        app_boot(t);
        uint32_t t0 = t;
        CHECK_EQ(ticks(&t, FWUPD_TEST_MAX_MS + 5000u, false), FWUPD_EV_RESTART);
        CHECK(t - t0 >= FWUPD_TEST_MAX_MS && t - t0 <= FWUPD_TEST_MAX_MS + 1000u);
    }
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, SZ_B));
    CHECK_EQ(state().state, FWIMG_BOOT_ROLLED_BACK);

    /* B runs again. C once more: the previous slot already holds B, so
     * nothing is erased there. */
    app_boot(t);
    fwupd_info(&in);
    CHECK(!in.testing);
    CHECK_MEM(in.run_id, idb, FWIMG_ID_LEN);
    unsigned erases = m.app_erases;
    CHECK_EQ(send(img_c, SZ_C, 0, t), FWUPD_CONTINUE);
    CHECK(strstr(fwupd_why(), "already holds") != NULL);
    CHECK_EQ(m.app_erases, erases + 1u); /* the candidate's header sector only */
    for (uint32_t n = 1; n <= nblocks(SZ_C); n++) {
        CHECK_EQ(send(img_c, SZ_C, n, t), n < nblocks(SZ_C) ? FWUPD_CONTINUE : FWUPD_DONE);
    }
    b = state();
    CHECK_EQ(b.state, FWIMG_BOOT_PENDING);
    CHECK_MEM(b.cand_id, idc, FWIMG_ID_LEN);
    CHECK_MEM(b.prev_id, idb, FWIMG_ID_LEN);
    CHECK_EQ(m.int_writes_by_app, 0);
    CHECK_EQ(fixed_sum(), fixed0);
}

/* Refusals and the transfer's rules. */
static void test_refusals(void) {
    reset_models();
    make_image(img_a, SZ_A, 11);
    make_image(img_b, SZ_B, 12);
    install_int(img_a, SZ_A);
    uint32_t t = 0;
    app_boot(t);
    uint8_t before[0x2000];
    memcpy(before, ext, sizeof before);
    unsigned w0 = m.app_programs + m.app_erases;
#define NOTHING_WRITTEN() CHECK_EQ(m.app_programs + m.app_erases, w0)

    /* Block 0 checks, before anything is written. */
    block0(img_b, SZ_B, blk);
    CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, false, t), FWUPD_REFUSED); /* no session */
    CHECK(strstr(fwupd_why(), "session") != NULL);
    m.net_on = false;
    CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_REFUSED);
    CHECK(strstr(fwupd_why(), "network") != NULL);
    m.net_on = true;
    CHECK_EQ(fwupd_block(0, false, blk, FWUPD_BLOCK, true, t), FWUPD_BAD); /* M = 0 */
    CHECK_EQ(fwupd_block(0, true, blk, 512, true, t), FWUPD_BAD);
    for (unsigned bit = 0; bit < FWIMG_HDR_LEN * 8u; bit += 7u) {
        block0(img_b, SZ_B, blk);
        blk[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        enum fwupd_rc rc = fwupd_block(0, true, blk, FWUPD_BLOCK, true, t);
        CHECK(rc == FWUPD_BAD || rc == FWUPD_TOO_LARGE);
    }
    block0(img_b, SZ_B, blk);
    blk[1000] = 1; /* padding not zero */
    CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_BAD);
    struct fwimg_hdr big = {.size = FWIMG_APP_MAX + 4u};
    memset(blk, 0, sizeof blk);
    fwimg_hdr_build(&big, blk);
    CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_TOO_LARGE);
    block0(img_a, SZ_A, blk);
    CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_REFUSED); /* running */
    CHECK(strstr(fwupd_why(), "running") != NULL);
    /* Boot states the application must leave alone. */
    static const uint8_t busy[] = {FWIMG_BOOT_TESTING, FWIMG_BOOT_PENDING, FWIMG_BOOT_ROLLBACK};
    for (unsigned i = 0; i < sizeof busy; i++) {
        struct fwimg_boot s = {.state = busy[i], .tries = 1};
        m.in_boot           = true;
        CHECK(fwimg_boot_write(&s));
        m.in_boot = false;
        block0(img_b, SZ_B, blk);
        CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_REFUSED);
        w0 = m.app_programs + m.app_erases;
    }
    NOTHING_WRITTEN();
    CHECK_MEM(ext, before, sizeof before); /* the slot headers are untouched */
    struct fwimg_boot none = {.state = FWIMG_BOOT_CONFIRMED};
    m.in_boot              = true;
    CHECK(fwimg_boot_write(&none)); /* settled again */
    m.in_boot = false;

    /* No transfer: blocks other than 0 are refused. */
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_INCOMPLETE);
    CHECK_EQ(send(img_b, SZ_B, 5, t), FWUPD_INCOMPLETE);

    /* A transfer: out of order, wrong length, M wrong, duplicates. */
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    CHECK_EQ(send(img_b, SZ_B, 2, t), FWUPD_INCOMPLETE);
    CHECK_EQ(fwupd_block(1, true, img_b, 1000, true, t), FWUPD_BAD);
    CHECK_EQ(fwupd_block(1, false, img_b, FWUPD_BLOCK, true, t), FWUPD_BAD);
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_CONTINUE); /* still on after those */
    unsigned p = m.app_programs;
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_CONTINUE); /* resent: nothing written */
    CHECK_EQ(m.app_programs, p);
    CHECK_EQ(send(img_b, SZ_B, 2, t), FWUPD_CONTINUE);
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_INCOMPLETE); /* two back */
    CHECK_EQ(fwupd_block(nblocks(SZ_B),
                         false,
                         img_b + (nblocks(SZ_B) - 1u) * FWUPD_BLOCK,
                         SZ_B % FWUPD_BLOCK,
                         true,
                         t),
             FWUPD_INCOMPLETE);
    /* The last block must have exactly the rest. */
    for (uint32_t n = 3; n < nblocks(SZ_B); n++) {
        CHECK_EQ(send(img_b, SZ_B, n, t), FWUPD_CONTINUE);
    }
    uint32_t last = nblocks(SZ_B), off = (last - 1u) * FWUPD_BLOCK;
    CHECK_EQ(fwupd_block(last, false, img_b + off, SZ_B - off + 4u, true, t), FWUPD_BAD);
    CHECK_EQ(fwupd_block(last, true, img_b + off, SZ_B - off, true, t), FWUPD_BAD);
    CHECK_EQ(state().state, FWIMG_BOOT_CONFIRMED); /* nothing marked */
    CHECK(!slot_holds(FLASH_SLOT_CAND, img_b, SZ_B));
    CHECK_EQ(send(img_b, SZ_B, last, t), FWUPD_DONE);
    CHECK(slot_holds(FLASH_SLOT_CAND, img_b, SZ_B));

    /* Block 1 that is not an application of the header's size: dropped. */
    reset_models();
    install_int(img_a, SZ_A);
    app_boot(t);
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    memcpy(blk, img_b, FWUPD_BLOCK);
    blk[FWIMG_DESC_OFFSET] ^= 1u; /* descriptor magic */
    CHECK_EQ(fwupd_block(1, true, blk, FWUPD_BLOCK, true, t), FWUPD_BAD);
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_INCOMPLETE); /* dropped */
    make_image(img_c, SZ_B + 4u, 13);
    memcpy(blk, img_c, FWUPD_BLOCK); /* an image of another size */
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    CHECK_EQ(fwupd_block(1, true, blk, FWUPD_BLOCK, true, t), FWUPD_BAD);

    /* No block for FWUPD_IDLE_MS: dropped. */
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_CONTINUE);
    uint32_t t1 = t;
    CHECK_EQ(ticks(&t1, FWUPD_IDLE_MS + 2000u, true), FWUPD_EV_DROPPED);
    CHECK_EQ(send(img_b, SZ_B, 2, t1), FWUPD_INCOMPLETE);
    /* An abort (the session ended) drops it too. */
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    fwupd_abort();
    CHECK_EQ(send(img_b, SZ_B, 1, t), FWUPD_INCOMPLETE);
    CHECK_EQ(state().state, FWIMG_BOOT_NONE);
    CHECK_EQ(m.int_writes_by_app, 0);
    CHECK_EQ(fixed_sum(), fixed0);
#undef NOTHING_WRITTEN
}

/* Corruption the read-back must catch: a bit flipped on the bus while the
 * candidate is hashed, a program that silently leaves a byte wrong, a
 * header whose SHA-256 is not the image's. Nothing is marked: the boot
 * state stays, the candidate slot has no valid header, the next boot
 * starts the old image without installing anything. */
static void test_corruption(void) {
    make_image(img_a, SZ_A, 21);
    make_image(img_b, SZ_B, 22);
    unsigned caught = 0;
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned trial = 0; trial < 12; trial++) {
            reset_models();
            install_int(img_a, SZ_A);
            uint32_t t = 0;
            app_boot(t);
            enum fwupd_rc rc = FWUPD_CONTINUE;
            if (kind == 0) {
                /* A read of the final hash: the reads of block 0 (previous
                 * slot check, save) come first, so aim at the end. */
                for (uint32_t n = 0; n < nblocks(SZ_B); n++) {
                    CHECK_EQ(send(img_b, SZ_B, n, t), FWUPD_CONTINUE);
                }
                m.flip_read_in = 1 + (long)(rnd() % nblocks(SZ_B));
                rc             = send(img_b, SZ_B, nblocks(SZ_B), t);
            } else if (kind == 1) {
                CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
                m.prog_fail_in = 1 + (long)(rnd() % (nblocks(SZ_B) * 16u));
                for (uint32_t n = 1;
                     n <= nblocks(SZ_B) && (rc = send(img_b, SZ_B, n, t)) == FWUPD_CONTINUE;
                     n++) {
                }
            } else {
                block0(img_b, SZ_B, blk);
                struct fwimg_hdr h;
                CHECK(fwimg_hdr_parse(blk, &h));
                h.sha256[rnd() % FWIMG_SHA_LEN] ^= 0x10u;
                memset(blk, 0, sizeof blk);
                fwimg_hdr_build(&h, blk);
                CHECK_EQ(fwupd_block(0, true, blk, FWUPD_BLOCK, true, t), FWUPD_CONTINUE);
                for (uint32_t n = 1;
                     n <= nblocks(SZ_B) && (rc = send(img_b, SZ_B, n, t)) == FWUPD_CONTINUE;
                     n++) {
                }
            }
            CHECK_EQ(rc, FWUPD_FAILED);
            caught += rc == FWUPD_FAILED;
            CHECK_EQ(state().state, FWIMG_BOOT_NONE);
            struct fwimg_hdr h;
            m.in_boot = true;
            CHECK(!fwimg_slot_verify(FLASH_SLOT_CAND, &h));
            m.in_boot = false;
            CHECK_EQ(boot(), BOOTCTL_JUMP);
            CHECK(int_is(img_a, SZ_A));
            /* The upload again, clean: it works. */
            m.flip_read_in = m.prog_fail_in = 0;
            app_boot(t);
            CHECK_EQ(upload(img_b, SZ_B, t), FWUPD_DONE);
            CHECK_EQ(boot(), BOOTCTL_JUMP);
            CHECK(int_is(img_b, SZ_B));
        }
    }
    printf("test_fwupd: corruption: %u of 36 caught before anything was marked\n", caught);
}

/* A power cut at every program and erase step of an update (save, transfer,
 * PENDING) and of the confirmation. After the cut: one boot, which must
 * start a whole image (the new one only if PENDING had been written whole);
 * then, from whatever is left, the upload again and the confirmation, which
 * must end on the new image. */
static void test_power_cuts(void) {
    make_image(img_a, 9216u, 31); /* small, for many cuts: 3 sectors */
    make_image(img_b, 6148u, 32);
    const uint32_t sa = 9216u, sb = 6148u;
    /* Survey: the steps of one update and its confirmation. */
    reset_models();
    install_int(img_a, sa);
    uint32_t t = 0;
    app_boot(t);
    CHECK_EQ(upload(img_b, sb, t), FWUPD_DONE);
    unsigned up_steps = m.steps;
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    app_boot(t);
    CHECK_EQ(ticks(&t, 62000, true), FWUPD_EV_CONFIRMED);
    unsigned all_steps = m.steps;
    CHECK(up_steps > 50u && all_steps > up_steps);

    unsigned to_new = 0, to_old = 0;
    for (unsigned k = 1; k <= all_steps; k++) {
        reset_models();
        install_int(img_a, sa);
        t = 0;
        app_boot(t);
        m.cut_in           = (long)k;
        bool pending_whole = false;
        if (k <= up_steps) {
            enum fwupd_rc rc = upload(img_b, sb, t);
            CHECK(m.dead);
            CHECK(rc == FWUPD_FAILED);
        } else {
            CHECK_EQ(upload(img_b, sb, t), FWUPD_DONE);
            pending_whole = true;
            CHECK_EQ(boot(), BOOTCTL_JUMP);
            app_boot(t);
            CHECK_EQ(ticks(&t, 62000, true), FWUPD_EV_CONFIRM_FAILED);
            CHECK(m.dead);
        }
        /* Power back: one boot. */
        m.dead   = false;
        m.cut_in = 0;
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        bool on_new = int_is(img_b, sb);
        CHECK(on_new || int_is(img_a, sa));
        struct fwimg_boot b = state();
        if (!pending_whole) {
            /* Before PENDING was whole: the old image, or the new one only
             * if the cut came after the record's commit mark. */
            CHECK(!on_new || b.state == FWIMG_BOOT_TESTING);
        } else {
            /* Cut while CONFIRMED was written: still under test (that boot
             * counted), or confirmed if the cut came after the commit mark. */
            CHECK(on_new
                  && ((b.state == FWIMG_BOOT_TESTING && b.tries == 2)
                      || b.state == FWIMG_BOOT_CONFIRMED));
        }
        to_new += on_new;
        to_old += !on_new;
        /* From here: the application runs, uploads again if it is the old
         * one, and the new one confirms. */
        t += 1000;
        app_boot(t);
        if (!on_new) {
            CHECK_EQ(upload(img_b, sb, t), FWUPD_DONE);
            CHECK_EQ(boot(), BOOTCTL_JUMP);
            CHECK(int_is(img_b, sb));
            app_boot(t);
        }
        if (state().state == FWIMG_BOOT_TESTING) {
            CHECK_EQ(ticks(&t, 62000, true), FWUPD_EV_CONFIRMED);
        }
        CHECK_EQ(state().state, FWIMG_BOOT_CONFIRMED);
        CHECK_EQ(boot(), BOOTCTL_JUMP);
        CHECK(int_is(img_b, sb));
        CHECK_EQ(m.int_writes_by_app, 0);
        CHECK_EQ(fixed_sum(), fixed0);
    }
    printf("test_fwupd: power cuts: %u (update %u steps, confirmation %u): first boot after the "
           "cut on the new image %u, on the old one %u; all ended confirmed on the new image\n",
           all_steps,
           up_steps,
           all_steps - up_steps,
           to_new,
           to_old);
}

/* The largest image, 448 KB: every block, the save of a 448 KB image, the
 * install. */
static void test_largest(void) {
    reset_models();
    make_image(img_a, FWIMG_APP_MAX, 41);
    make_image(img_b, FWIMG_APP_MAX - 1024u, 42);
    install_int(img_a, FWIMG_APP_MAX);
    uint32_t t = 0;
    app_boot(t);
    CHECK_EQ(upload(img_b, FWIMG_APP_MAX - 1024u, t), FWUPD_DONE);
    CHECK(slot_holds(FLASH_SLOT_PREV, img_a, FWIMG_APP_MAX));
    CHECK(slot_holds(FLASH_SLOT_CAND, img_b, FWIMG_APP_MAX - 1024u));
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, FWIMG_APP_MAX - 1024u));
    printf("test_fwupd: 448 KB image: %u programs, %u erases by the application\n",
           m.app_programs,
           m.app_erases);
    CHECK_EQ(fixed_sum(), fixed0);
}

/* The previous slot damaged (a bit gone bad in the flash itself): a save
 * that finds the running image's header there still hashes the slot and
 * writes it again, and a slot damaged between block 0 and the last block
 * fails the update before PENDING (the bootloader would refuse it). */
static void test_prev_damaged(void) {
    reset_models();
    make_image(img_a, SZ_A, 61);
    make_image(img_b, SZ_B, 62);
    install_int(img_a, SZ_A);
    uint32_t t = 0;
    app_boot(t);
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    CHECK(strstr(fwupd_why(), "saved") != NULL);
    fwupd_abort();
    CHECK(slot_holds(FLASH_SLOT_PREV, img_a, SZ_A));
    ext[FLASH_SLOT_PREV + FLASH_SLOT_IMAGE + 5000u] ^= 0x04u;
    unsigned erases = m.app_erases;
    CHECK_EQ(send(img_b, SZ_B, 0, t), FWUPD_CONTINUE);
    CHECK(strstr(fwupd_why(), "saved to") != NULL);
    CHECK(m.app_erases > erases + 2u);
    CHECK(slot_holds(FLASH_SLOT_PREV, img_a, SZ_A));
    for (uint32_t n = 1; n < nblocks(SZ_B); n++) {
        CHECK_EQ(send(img_b, SZ_B, n, t), FWUPD_CONTINUE);
    }
    ext[FLASH_SLOT_PREV + FLASH_SLOT_IMAGE + 9000u] ^= 0x01u;
    CHECK_EQ(send(img_b, SZ_B, nblocks(SZ_B), t), FWUPD_FAILED);
    CHECK(strstr(fwupd_why(), "previous slot") != NULL);
    CHECK_EQ(state().state, FWIMG_BOOT_NONE);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_a, SZ_A));
    /* Again from block 0: saved again, then installed. */
    app_boot(t);
    CHECK_EQ(upload(img_b, SZ_B, t), FWUPD_DONE);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, SZ_B));
}

static void test_status_text(void) {
    reset_models();
    make_image(img_a, SZ_A, 51);
    make_image(img_b, SZ_B, 52);
    install_int(img_a, SZ_A);
    app_boot(0);
    char   s[256];
    size_t n = fwupd_status_text(s, sizeof s);
    CHECK_EQ(n, strlen(s));
    CHECK(n < 200u);
    uint8_t id[FWIMG_ID_LEN];
    char    want[80];
    id_of(img_a, SZ_A, id);
    snprintf(want, sizeof want, "run ");
    for (unsigned i = 0; i < FWIMG_ID_LEN; i++) {
        snprintf(want + 4 + 2 * i, sizeof want - 4 - 2 * i, "%02x", id[i]);
    }
    CHECK(strncmp(s, want, strlen(want)) == 0);
    CHECK(strstr(s, " boot none ") && strstr(s, " xfer idle "));
    CHECK_EQ(send(img_b, SZ_B, 0, 0), FWUPD_CONTINUE);
    fwupd_status_text(s, sizeof s);
    CHECK(strstr(s, " xfer 1/31 ") != NULL);
    printf("test_fwupd: status: %s\n", s);
    CHECK_EQ(fwupd_status_text(s, 10), 9);
    CHECK_EQ(strlen(s), 9);
}

/* --- With the uploader (tests/host/test_upload_tool.sh) ---------------------------
 * --write-image FILE SEED SIZE: a synthetic application image for
 * tools/update/upload.py. --feed-frames IMAGE FRAMES IPV6HEX: the frames
 * the tool wrote (each with a 2-byte length), taken apart as src/session.c
 * does (28-byte frame addressed to the bridge, CoAP, CON POST b/fw, Block1
 * SZX 6) and handed to fwupd_block(); then the candidate must be the image
 * and the boot state PENDING for it. */
static int write_image(const char *path, uint32_t seed, uint32_t size) {
    make_image(img_a, size, seed);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img_a, 1, size, f) != size || fclose(f) != 0) {
        return 2;
    }
    return 0;
}

static size_t read_file(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static uint8_t frames[(FWUPD_BLOCKS + 2u) * (FWUPD_BLOCK + 64u)];

static int feed_frames(const char *image, const char *path, const char *ipv6hex) {
    uint8_t ipv6[16];
    for (unsigned i = 0; i < 16; i++) {
        unsigned v = 0;
        CHECK(sscanf(ipv6hex + 2 * i, "%2x", &v) == 1);
        ipv6[i] = (uint8_t)v;
    }
    uint32_t size = (uint32_t)read_file(image, img_b, sizeof img_b);
    size_t   n    = read_file(path, frames, sizeof frames);
    CHECK(size > 0 && n > 0);
    reset_models();
    make_image(img_a, SZ_A, 99);
    install_int(img_a, SZ_A);
    app_boot(0);
    enum fwupd_rc rc   = FWUPD_CONTINUE;
    unsigned      seen = 0;
    for (size_t pos = 0; pos + 2u <= n && rc == FWUPD_CONTINUE;) {
        size_t         len = (size_t)(frames[pos] << 8 | frames[pos + 1u]);
        const uint8_t *d   = frames + pos + 2u;
        pos += 2u + len;
        CHECK(pos <= n && len >= 28u && d[2] == 16u);
        CHECK((d[0] << 8 | d[1]) == 0x0002);
        CHECK_MEM(d + 3, ipv6, 16);
        size_t clen = (size_t)(d[26] << 8 | d[27]);
        CHECK_EQ(clen, len - 28u);
        struct coap_msg msg;
        uint32_t        num  = 0;
        bool            more = false;
        unsigned        szx  = 0;
        CHECK(coap_parse(d + 28, clen, &msg));
        CHECK_EQ(msg.type, COAP_CON);
        CHECK_EQ(msg.code, COAP_POST);
        CHECK(coap_path_is(&msg, "b/fw"));
        CHECK(coap_block1(&msg, &num, &more, &szx));
        CHECK_EQ(szx, FWUPD_SZX);
        CHECK_EQ(num, seen);
        CHECK(msg.payload_len <= FWUPD_BLOCK);
        rc = fwupd_block(num, more, msg.payload, msg.payload_len, true, 0);
        seen++;
    }
    CHECK_EQ(rc, FWUPD_DONE);
    CHECK_EQ(seen, nblocks(size) + 1u);
    CHECK(slot_holds(FLASH_SLOT_CAND, img_b, size));
    struct fwimg_boot b = state();
    uint8_t           id[FWIMG_ID_LEN];
    id_of(img_b, size, id);
    CHECK_EQ(b.state, FWIMG_BOOT_PENDING);
    CHECK_MEM(b.cand_id, id, FWIMG_ID_LEN);
    CHECK_EQ(boot(), BOOTCTL_JUMP);
    CHECK(int_is(img_b, size));
    return TEST_REPORT("test_fwupd (frames of tools/update/upload.py)");
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--write-image") == 0) {
        return write_image(
            argv[2], (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 0));
    }
    if (argc == 5 && strcmp(argv[1], "--feed-frames") == 0) {
        return feed_frames(argv[2], argv[3], argv[4]);
    }
    test_ops_allowed();
    test_update_cycle();
    test_refusals();
    test_corruption();
    test_power_cuts();
    test_largest();
    test_prev_damaged();
    test_status_text();
    return TEST_REPORT("test_fwupd");
}
