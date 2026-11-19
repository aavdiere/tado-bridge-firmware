/* The application's side of a firmware update (#24). See fwupd.h and
 * docs/design.md "Implementation: self-update, application side (#24)". */
#include "fwupd.h"

#include "flash_layout.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include <stdio.h>
#include <string.h>

_Static_assert(FWUPD_BLOCK % FWUPD_PROG_MAX == 0, "a block is whole pages");
_Static_assert(FLASH_SECTOR % FWUPD_BLOCK == 0, "a sector is whole blocks");
_Static_assert(FWUPD_PROG_MAX == FLASH_PAGE, "a program operation is one page");
_Static_assert(FWUPD_READ_MAX == FWUPD_BLOCK, "one read buffer");
_Static_assert(FWIMG_HDR_LEN <= FWUPD_BLOCK, "the header fits block 0");
_Static_assert(FWIMG_HEAD_LEN <= FWUPD_BLOCK, "the image head is in block 1");
_Static_assert(FWUPD_BLOCKS < (1u << 20), "block numbers fit Block1");

/* --- Flash operations --------------------------------------------------------- */
/* [addr, addr + len) inside one of the two slots. */
static bool in_slot(uint32_t addr, uint32_t len) {
    static const uint32_t slots[2] = {FLASH_SLOT_CAND, FLASH_SLOT_PREV};
    for (unsigned i = 0; i < 2u; i++) {
        if (addr >= slots[i] && len <= FLASH_SLOT_SIZE
            && addr - slots[i] <= FLASH_SLOT_SIZE - len) {
            return true;
        }
    }
    return false;
}

bool fwupd_op_allowed(const struct fwupd_op *op) {
    switch (op->kind) {
        case FWUPD_OP_READ:
            return op->dst && op->len >= 1u && op->len <= FWUPD_READ_MAX
                   && in_slot(op->addr, op->len);
        case FWUPD_OP_ERASE:
            return op->addr % FLASH_SECTOR == 0 && in_slot(op->addr, FLASH_SECTOR);
        case FWUPD_OP_PROGRAM:
            return op->src && op->len >= 1u && op->len <= FWUPD_PROG_MAX
                   && op->addr % FLASH_PAGE + op->len <= FLASH_PAGE && in_slot(op->addr, op->len);
        case FWUPD_OP_BOOT_LOAD:
            return true;
        case FWUPD_OP_BOOT_WRITE:
            /* The application writes only these two (docs/design.md "Boot
             * state record"); every other state is the bootloader's. */
            return (op->boot.state == FWIMG_BOOT_PENDING || op->boot.state == FWIMG_BOOT_CONFIRMED)
                   && op->boot.tries == 0;
        default:
            return false; /* FWUPD_OP_RESTART is the port's, not a flash operation */
    }
}

/* Read [addr, addr + n) back in driver-sized pieces: all 0xFF (src NULL) or
 * equal to src. */
static bool read_back(uint32_t addr, const uint8_t *src, uint32_t n) {
    uint8_t b[STORE_XFER_MAX];
    for (uint32_t off = 0; off < n; off += sizeof b) {
        uint32_t k = n - off < sizeof b ? n - off : (uint32_t)sizeof b;
        if (!store_port_read(addr + off, b, k)) {
            return false;
        }
        for (uint32_t i = 0; i < k; i++) {
            if (b[i] != (src ? src[off + i] : 0xFFu)) {
                return false;
            }
        }
    }
    return true;
}

bool fwupd_op_run(struct fwupd_op *op) {
    op->ok = false;
    if (!fwupd_op_allowed(op)) {
        return false;
    }
    fwupd_port_area(op->kind == FWUPD_OP_BOOT_WRITE ? FWUPD_AREA_BOOT
                    : op->kind == FWUPD_OP_ERASE || op->kind == FWUPD_OP_PROGRAM
                        ? FWUPD_AREA_SLOTS
                        : FWUPD_AREA_DEFAULT);
    bool ok = false;
    switch (op->kind) {
        case FWUPD_OP_READ:
            ok = store_port_read(op->addr, op->dst, op->len);
            break;
        case FWUPD_OP_ERASE:
            ok = store_port_erase(op->addr) && read_back(op->addr, NULL, FLASH_SECTOR);
            break;
        case FWUPD_OP_PROGRAM:
            ok = true;
            for (uint32_t off = 0; ok && off < op->len; off += STORE_XFER_MAX) {
                uint32_t k = op->len - off < STORE_XFER_MAX ? op->len - off : STORE_XFER_MAX;
                ok         = store_port_program(op->addr + off, op->src + off, k);
            }
            ok = ok && read_back(op->addr, op->src, op->len);
            break;
        case FWUPD_OP_BOOT_LOAD:
            op->found = fwimg_boot_load(&op->boot);
            ok        = true;
            break;
        case FWUPD_OP_BOOT_WRITE:
            ok = fwimg_boot_write(&op->boot);
            break;
        default:
            break;
    }
    fwupd_port_area(FWUPD_AREA_DEFAULT);
    op->ok = ok;
    return ok;
}

/* --- Update logic --------------------------------------------------------------- */
static struct {
    const uint8_t    *img;
    uint32_t          img_size, img_fw;
    uint8_t           run_sha[FWIMG_SHA_LEN];
    struct fwimg_boot boot;
    bool              testing;
    uint32_t          boot_ms, last_tick_ms, healthy_ms, confirm_at;
    bool              confirm_wait; /* a CONFIRMED write failed: wait until confirm_at */
    /* The transfer. */
    bool             active, done;
    struct fwimg_hdr hdr;
    uint32_t         next, blocks, last_ms, done_ms;
    const char      *why;
} u;

static uint8_t                rbuf[FWUPD_READ_MAX];
static mbedtls_sha256_context sha;

/* The status fields fwupd_info() copies change only between these. */
#define LOCKED(stmts)        \
    do {                     \
        fwupd_port_lock();   \
        stmts;               \
        fwupd_port_unlock(); \
    } while (0)

static bool op(struct fwupd_op *o) {
    o->done    = false;
    o->network = false;
    return fwupd_port_op(o);
}

static bool op_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    struct fwupd_op o = {.kind = FWUPD_OP_READ, .addr = addr, .len = len, .dst = dst};
    return op(&o);
}

static bool op_erase(uint32_t addr) {
    struct fwupd_op o = {.kind = FWUPD_OP_ERASE, .addr = addr};
    return op(&o);
}

/* [addr, addr + n) from src, one page per operation. */
static bool program(uint32_t addr, const uint8_t *src, uint32_t n) {
    for (uint32_t off = 0; off < n;) {
        uint32_t        room = FLASH_PAGE - (addr + off) % FLASH_PAGE;
        uint32_t        k    = n - off < room ? n - off : room;
        struct fwupd_op o    = {
            .kind = FWUPD_OP_PROGRAM, .addr = addr + off, .len = k, .src = src + off};
        if (!op(&o)) {
            return false;
        }
        off += k;
    }
    return true;
}

static bool boot_write(const struct fwimg_boot *b) {
    struct fwupd_op o = {.kind = FWUPD_OP_BOOT_WRITE, .boot = *b};
    return op(&o);
}

static void set_why(const char *why) {
    u.why = why;
}

/* SHA-256 of [base, base + size) of a slot, read back through operations.
 * The image's head must pass fwimg_app_check() with that size. */
static bool slot_hash(uint32_t base, uint32_t size, uint8_t out[FWIMG_SHA_LEN]) {
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    for (uint32_t off = 0; ok && off < size; off += FWUPD_READ_MAX) {
        uint32_t n = size - off < FWUPD_READ_MAX ? size - off : FWUPD_READ_MAX;
        ok         = op_read(base + off, rbuf, n);
        if (ok && off == 0) {
            uint32_t s = 0;
            ok         = n >= FWIMG_HEAD_LEN && fwimg_app_check(rbuf, &s) && s == size;
        }
        ok = ok && mbedtls_sha256_update(&sha, rbuf, n) == 0;
    }
    ok = ok && mbedtls_sha256_finish(&sha, out) == 0;
    mbedtls_sha256_free(&sha);
    return ok;
}

/* Does the slot hold a whole valid image (fwimg_slot_verify(), through
 * operations)? *h: its header. */
static bool slot_valid(uint32_t slot, struct fwimg_hdr *h) {
    uint8_t raw[FWIMG_HDR_LEN];
    uint8_t s[FWIMG_SHA_LEN];
    return op_read(slot, raw, sizeof raw) && fwimg_hdr_parse(raw, h)
           && slot_hash(slot + FLASH_SLOT_IMAGE, h->size, s)
           && memcmp(s, h->sha256, FWIMG_SHA_LEN) == 0;
}

/* Program a slot's header last, and read it back as the bootloader will. */
static bool write_header(uint32_t slot, const struct fwimg_hdr *h) {
    uint8_t          raw[FWIMG_HDR_LEN];
    struct fwimg_hdr back;
    fwimg_hdr_build(h, raw);
    return program(slot, raw, sizeof raw) && op_read(slot, rbuf, sizeof raw)
           && memcmp(rbuf, raw, sizeof raw) == 0 && fwimg_hdr_parse(rbuf, &back)
           && memcmp(&back, h, sizeof back) == 0;
}

/* The running image into the previous slot, unless it holds it already
 * (whole, by its SHA-256). The header sector is erased first and the
 * header programmed last, after the read-back hash matched, so a cut in
 * between leaves the slot without a valid image, never a wrong one. */
static bool save_running(bool *saved) {
    struct fwimg_hdr h;
    *saved = false;
    if (slot_valid(FLASH_SLOT_PREV, &h) && memcmp(h.sha256, u.run_sha, FWIMG_SHA_LEN) == 0
        && h.size == u.img_size) {
        return true;
    }
    uint32_t base = FLASH_SLOT_PREV + FLASH_SLOT_IMAGE;
    if (!op_erase(FLASH_SLOT_PREV)) {
        return false;
    }
    for (uint32_t off = 0; off < u.img_size; off += FLASH_SECTOR) {
        uint32_t n = u.img_size - off < FLASH_SECTOR ? u.img_size - off : FLASH_SECTOR;
        if (!op_erase(base + off) || !program(base + off, u.img + off, n)) {
            return false;
        }
    }
    uint8_t s[FWIMG_SHA_LEN];
    if (!slot_hash(base, u.img_size, s) || memcmp(s, u.run_sha, FWIMG_SHA_LEN) != 0) {
        return false;
    }
    h = (struct fwimg_hdr){.size = u.img_size, .fw_version = u.img_fw};
    memcpy(h.sha256, u.run_sha, FWIMG_SHA_LEN);
    *saved = true;
    return write_header(FLASH_SLOT_PREV, &h);
}

void fwupd_abort(void) {
    LOCKED(u.active = false);
}

void fwupd_init(const uint8_t *image, uint32_t size, uint32_t fw_version, uint32_t now_ms) {
    memset(&u, 0, sizeof u);
    u.img          = image;
    u.img_size     = size;
    u.img_fw       = fw_version;
    u.boot_ms      = now_ms;
    u.last_tick_ms = now_ms;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0 || mbedtls_sha256_update(&sha, image, size) != 0
        || mbedtls_sha256_finish(&sha, u.run_sha) != 0) {
        memset(u.run_sha, 0, sizeof u.run_sha); /* not reached with mbedTLS's software SHA-256 */
    }
    mbedtls_sha256_free(&sha);
    struct fwupd_op   o       = {.kind = FWUPD_OP_BOOT_LOAD};
    struct fwimg_boot b       = {0};
    bool              testing = false;
    if (op(&o) && o.found) {
        b       = o.boot;
        testing = b.state == FWIMG_BOOT_TESTING && memcmp(b.cand_id, u.run_sha, FWIMG_ID_LEN) == 0;
    }
    LOCKED(u.boot = b; u.testing = testing);
    set_why(testing ? "running the candidate under test: confirmation due" : "no update pending");
}

/* --- Transfer --------------------------------------------------------------- */
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static enum fwupd_rc fail(enum fwupd_rc rc, const char *why) {
    set_why(why);
    if (rc == FWUPD_FAILED) {
        LOCKED(u.active = false);
    }
    return rc;
}

static enum fwupd_rc
start(const uint8_t *data, size_t len, bool more, bool session_ok, uint32_t now_ms) {
    struct fwimg_hdr h;
    LOCKED(u.active = false); /* block 0 ends any transfer under way, whatever follows */
    if (len != FWUPD_BLOCK || !more) {
        return fail(FWUPD_BAD, "block 0 must be 1024 bytes, more to follow");
    }
    if (get32(data) == FWIMG_HDR_MAGIC && get32(data + 8) > FWIMG_APP_MAX) {
        return fail(FWUPD_TOO_LARGE, "image larger than 448 KB");
    }
    if (!fwimg_hdr_parse(data, &h)) {
        return fail(FWUPD_BAD, "block 0 does not start with a valid TFW1 header");
    }
    for (size_t i = FWIMG_HDR_LEN; i < len; i++) {
        if (data[i] != 0) {
            return fail(FWUPD_BAD, "block 0 not zero after the header");
        }
    }
    if (!session_ok) {
        return fail(FWUPD_REFUSED, "no server session");
    }
    struct fwupd_op o = {.kind = FWUPD_OP_BOOT_LOAD};
    if (!op(&o)) {
        return fail(FWUPD_FAILED, "boot state not read (flash not ready)");
    }
    if (!o.network) {
        return fail(FWUPD_REFUSED, "no network record in use");
    }
    struct fwimg_boot b = o.found ? o.boot : (struct fwimg_boot){0};
    LOCKED(u.boot = b);
    if (b.state == FWIMG_BOOT_TESTING || b.state == FWIMG_BOOT_PENDING
        || b.state == FWIMG_BOOT_ROLLBACK) {
        return fail(FWUPD_REFUSED, "an update is still under way (boot state not settled)");
    }
    if (memcmp(h.sha256, u.run_sha, FWIMG_ID_LEN) == 0) {
        return fail(FWUPD_REFUSED, "this image is the one running");
    }
    bool saved = false;
    if (!save_running(&saved)) {
        return fail(FWUPD_FAILED, "running image not saved to the previous slot");
    }
    /* The candidate slot loses its header first: until the last block it
     * holds no valid image. */
    if (!op_erase(FLASH_SLOT_CAND)) {
        return fail(FWUPD_FAILED, "candidate slot header not erased");
    }
    LOCKED(u.active = true; u.hdr = h; u.next = 1;
           u.blocks                           = (h.size + FWUPD_BLOCK - 1u) / FWUPD_BLOCK;
           u.last_ms                          = now_ms);
    set_why(saved ? "transfer started; running image saved to the previous slot"
                  : "transfer started; previous slot already holds the running image");
    return FWUPD_CONTINUE;
}

/* The last block is in: check the whole candidate, write its header, check
 * the previous slot again, then PENDING. */
static enum fwupd_rc finish(uint32_t now_ms) {
    uint8_t s[FWIMG_SHA_LEN];
    if (!slot_hash(FLASH_SLOT_CAND + FLASH_SLOT_IMAGE, u.hdr.size, s)) {
        return fail(FWUPD_FAILED, "candidate not read back, or its head is not an application");
    }
    if (memcmp(s, u.hdr.sha256, FWIMG_SHA_LEN) != 0) {
        return fail(FWUPD_FAILED, "candidate read back does not match its SHA-256");
    }
    if (!write_header(FLASH_SLOT_CAND, &u.hdr)) {
        return fail(FWUPD_FAILED, "candidate header not written");
    }
    struct fwimg_hdr hp;
    if (!slot_valid(FLASH_SLOT_PREV, &hp) || memcmp(hp.sha256, u.run_sha, FWIMG_SHA_LEN) != 0) {
        return fail(FWUPD_FAILED, "previous slot no longer holds the running image");
    }
    struct fwimg_boot b = {.state = FWIMG_BOOT_PENDING, .tries = 0};
    memcpy(b.cand_id, u.hdr.sha256, FWIMG_ID_LEN);
    memcpy(b.prev_id, u.run_sha, FWIMG_ID_LEN);
    if (!boot_write(&b)) {
        return fail(FWUPD_FAILED, "boot state PENDING not written");
    }
    LOCKED(u.active = false; u.done = true; u.done_ms = now_ms; u.boot = b);
    set_why("image verified, previous image saved, PENDING written: restarting");
    return FWUPD_DONE;
}

enum fwupd_rc fwupd_block(
    uint32_t num, bool more, const uint8_t *data, size_t len, bool session_ok, uint32_t now_ms) {
    if (u.done) {
        /* PENDING is written; only the last block's answer may be asked for again. */
        if (num == u.blocks && !more) {
            set_why("update written: restarting");
            return FWUPD_DONE;
        }
        return fail(FWUPD_REFUSED, "update written: restarting");
    }
    if (num == 0) {
        return start(data, len, more, session_ok, now_ms);
    }
    if (!u.active) {
        return fail(FWUPD_INCOMPLETE, "no transfer: start with block 0");
    }
    if (num + 1u == u.next) {
        LOCKED(u.last_ms = now_ms); /* its answer was lost: the same answer, no write */
        set_why("block stored before");
        return FWUPD_CONTINUE;
    }
    if (num != u.next) {
        return fail(FWUPD_INCOMPLETE, "not the block expected next");
    }
    uint32_t off  = (num - 1u) * FWUPD_BLOCK;
    uint32_t want = u.hdr.size - off < FWUPD_BLOCK ? u.hdr.size - off : FWUPD_BLOCK;
    bool     last = num == u.blocks;
    if (len != want || more == last) {
        return fail(FWUPD_BAD, "block length or more flag does not fit the header's size");
    }
    if (num == 1u) {
        uint32_t s = 0;
        if (!fwimg_app_check(data, &s) || s != u.hdr.size) {
            LOCKED(u.active = false);
            return fail(FWUPD_BAD, "not an application for 0x08010000 of the header's size");
        }
    }
    uint32_t addr = FLASH_SLOT_CAND + FLASH_SLOT_IMAGE + off;
    if (off % FLASH_SECTOR == 0 && !op_erase(addr)) {
        return fail(FWUPD_FAILED, "candidate sector not erased");
    }
    if (!program(addr, data, (uint32_t)len)) {
        return fail(FWUPD_FAILED, "block not programmed or not read back");
    }
    LOCKED(u.next++; u.last_ms = now_ms);
    if (last) {
        return finish(now_ms);
    }
    set_why("block stored");
    return FWUPD_CONTINUE;
}

const char *fwupd_why(void) {
    return u.why ? u.why : "";
}

/* --- Confirmation and timeouts ---------------------------------------------- */

enum fwupd_event fwupd_tick(uint32_t now_ms, bool healthy) {
    uint32_t dt    = now_ms - u.last_tick_ms;
    u.last_tick_ms = now_ms;
    if (u.done) {
        return now_ms - u.done_ms >= FWUPD_RESTART_MS ? FWUPD_EV_RESTART : FWUPD_EV_NONE;
    }
    if (u.active && now_ms - u.last_ms >= FWUPD_IDLE_MS) {
        LOCKED(u.active = false);
        set_why("transfer dropped: no block for 120 s");
        return FWUPD_EV_DROPPED;
    }
    if (!u.testing) {
        return FWUPD_EV_NONE;
    }
    /* A tick that comes late (the task was held off) counts at most 5 s. */
    uint32_t h = healthy ? u.healthy_ms + (dt < 5000u ? dt : 5000u) : 0;
    LOCKED(u.healthy_ms = h);
    if (u.healthy_ms >= FWUPD_CONFIRM_MS
        && (!u.confirm_wait || (int32_t)(now_ms - u.confirm_at) >= 0)) {
        struct fwimg_boot b = u.boot;
        b.state             = FWIMG_BOOT_CONFIRMED;
        b.tries             = 0;
        if (boot_write(&b)) {
            LOCKED(u.boot = b; u.testing = false);
            set_why("candidate confirmed");
            return FWUPD_EV_CONFIRMED;
        }
        u.confirm_wait = true;
        u.confirm_at   = now_ms + FWUPD_RETRY_MS;
        set_why("CONFIRMED not written; trying again in 10 s");
        return FWUPD_EV_CONFIRM_FAILED;
    }
    if (now_ms - u.boot_ms >= FWUPD_TEST_MAX_MS) {
        set_why("candidate not confirmed within 10 min: restarting (the bootloader counts it)");
        return FWUPD_EV_RESTART;
    }
    return FWUPD_EV_NONE;
}

/* --- Status ------------------------------------------------------------------- */
void fwupd_info(struct fwupd_info *out) {
    fwupd_port_lock();
    memcpy(out->run_id, u.run_sha, FWIMG_ID_LEN);
    out->boot       = u.boot;
    out->testing    = u.testing;
    out->active     = u.active;
    out->done       = u.done;
    out->next       = u.next;
    out->blocks     = u.blocks;
    out->healthy_ms = u.healthy_ms;
    fwupd_port_unlock();
}

const char *fwupd_state_name(uint8_t state) {
    static const char *const name[] = {
        [FWIMG_BOOT_NONE]        = "none",
        [FWIMG_BOOT_CONFIRMED]   = "confirmed",
        [FWIMG_BOOT_PENDING]     = "pending",
        [FWIMG_BOOT_TESTING]     = "testing",
        [FWIMG_BOOT_ROLLBACK]    = "rollback",
        [FWIMG_BOOT_ROLLED_BACK] = "rolled_back",
        [FWIMG_BOOT_REJECTED]    = "rejected",
        [FWIMG_BOOT_NO_ROLLBACK] = "no_rollback",
        [FWIMG_BOOT_EXTERNAL]    = "external",
    };
    return state < sizeof name / sizeof name[0] ? name[state] : "?";
}

static void hex(char *out, const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2u * i]      = d[p[i] >> 4];
        out[2u * i + 1u] = d[p[i] & 15u];
    }
    out[2u * n] = '\0';
}

size_t fwupd_status_text(char *buf, size_t cap) {
    struct fwupd_info in;
    char run[2u * FWIMG_ID_LEN + 1u], cand[2u * FWIMG_ID_LEN + 1u], prev[2u * FWIMG_ID_LEN + 1u],
        xfer[24];
    fwupd_info(&in);
    hex(run, in.run_id, FWIMG_ID_LEN);
    hex(cand, in.boot.cand_id, FWIMG_ID_LEN);
    hex(prev, in.boot.prev_id, FWIMG_ID_LEN);
    if (in.done) {
        snprintf(xfer, sizeof xfer, "done");
    } else if (in.active) {
        snprintf(xfer, sizeof xfer, "%lu/%lu", (unsigned long)in.next, (unsigned long)in.blocks);
    } else {
        snprintf(xfer, sizeof xfer, "idle");
    }
    int n = snprintf(buf,
                     cap,
                     "run %s boot %s tries %u cand %s prev %s xfer %s healthy %lu",
                     run,
                     fwupd_state_name(in.boot.state),
                     (unsigned)in.boot.tries,
                     cand,
                     prev,
                     xfer,
                     (unsigned long)(in.testing ? in.healthy_ms / 1000u : 0u));
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1u;
}
