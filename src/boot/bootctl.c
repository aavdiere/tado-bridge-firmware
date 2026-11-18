/* The bootloader's decisions (#24). See bootctl.h and docs/design.md
 * "Implementation: bootloader and boot state (#24)". */
#include "bootctl.h"

#include "flash_layout.h"
#include "fwimg.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include <string.h>

#define CHUNK     256u
#define MAX_STEPS 16u /* state changes in one boot; the tries bound it to about 8 */

_Static_assert(FWIMG_DESC_OFFSET % CHUNK == 0, "the commit word starts a chunk");
_Static_assert(FWIMG_HEAD_LEN <= CHUNK * 3u, "head fits the buffers");

static uint8_t                buf[CHUNK];
static uint8_t                back[CHUNK];
static struct bootctl_report *rep;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* --- Internal flash ----------------------------------------------------------- */
/* Is there a valid application in internal flash? *size from its descriptor. */
static bool int_app(uint32_t *size) {
    uint8_t head[FWIMG_HEAD_LEN];
    bl_port_int_read(FWIMG_APP_BASE, head, sizeof head);
    return fwimg_app_check(head, size);
}

/* SHA-256 of internal flash [base, base + size). With assume_magic, the
 * descriptor's magic word counts as FWIMG_DESC_MAGIC whatever the flash
 * holds there (during an install it is still erased). */
static bool int_sha(uint32_t size, bool assume_magic, uint8_t out[FWIMG_SHA_LEN]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    for (uint32_t off = 0; ok && off < size; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        bl_port_int_read(FWIMG_APP_BASE + off, buf, n);
        if (assume_magic && off == FWIMG_DESC_OFFSET) {
            put32(buf, FWIMG_DESC_MAGIC);
        }
        ok = mbedtls_sha256_update(&c, buf, n) == 0;
        if ((off & 0xFFFu) == 0) {
            bl_port_kick();
        }
    }
    ok = ok && mbedtls_sha256_finish(&c, out) == 0;
    mbedtls_sha256_free(&c);
    return ok;
}

/* The id (first 16 bytes of the SHA-256) of the valid application in
 * internal flash; false if there is none. */
static bool int_id(uint8_t id[FWIMG_ID_LEN]) {
    uint32_t size = 0;
    uint8_t  sha[FWIMG_SHA_LEN];
    if (!int_app(&size) || !int_sha(size, false, sha)) {
        return false;
    }
    memcpy(id, sha, FWIMG_ID_LEN);
    return true;
}

/* Copy the image in slot (already checked: h) into internal flash. Erases
 * the sectors it covers, sector 4 first, so the old commit mark goes
 * first; programs everything but the descriptor magic and reads each chunk
 * back; checks the SHA-256 of internal flash with the magic as it will be;
 * then programs the magic and checks the application. Any failure: false,
 * and internal flash holds no valid application (unless the magic itself
 * was the last thing to fail and still reads right, which the check after
 * it catches). */
static bool install(uint32_t slot, const struct fwimg_hdr *h) {
    rep->installs++;
    for (unsigned s = FWIMG_APP_FIRST_SECTOR; s <= FWIMG_APP_LAST_SECTOR; s++) {
        if (fwimg_sector_addr(s) >= FWIMG_APP_BASE + h->size) {
            break;
        }
        bl_port_kick(); /* a 128 KB sector takes up to 2 s (stm32f411ce.txt:5320-5330) */
        if (!bl_port_int_erase(s)) {
            return false;
        }
    }
    bl_port_kick();
    for (uint32_t off = 0; off < h->size; off += CHUNK) {
        uint32_t n    = h->size - off < CHUNK ? h->size - off : CHUNK;
        uint32_t addr = FWIMG_APP_BASE + off;
        if (!store_port_read(slot + FLASH_SLOT_IMAGE + off, buf, n)) {
            return false;
        }
        bool ok;
        if (off == FWIMG_DESC_OFFSET) {
            /* Not the magic: it stays erased (0xFF) until the end. */
            memset(buf, 0xFF, 4u);
            ok = bl_port_int_program(addr + 4u, buf + 4u, n - 4u);
        } else {
            ok = bl_port_int_program(addr, buf, n);
        }
        bl_port_int_read(addr, back, n);
        if (!ok || memcmp(back, buf, n) != 0) {
            return false;
        }
        if ((off & 0xFFFu) == 0) {
            bl_port_kick();
        }
    }
    uint8_t sha[FWIMG_SHA_LEN];
    if (!int_sha(h->size, true, sha) || memcmp(sha, h->sha256, FWIMG_SHA_LEN) != 0) {
        return false;
    }
    uint8_t m[4];
    put32(m, FWIMG_DESC_MAGIC);
    uint32_t size = 0;
    if (!bl_port_int_program(FWIMG_APP_BASE + FWIMG_DESC_OFFSET, m, sizeof m) || !int_app(&size)
        || size != h->size) {
        return false;
    }
    rep->installed++;
    return true;
}

/* --- Slots and state ---------------------------------------------------------- */
/* A whole valid image in slot, and (if id) the one with that id. */
static bool slot_ok(uint32_t slot, const uint8_t *id, struct fwimg_hdr *h) {
    return fwimg_slot_verify(slot, h) && (!id || memcmp(h->sha256, id, FWIMG_ID_LEN) == 0);
}

static bool set_state(const struct fwimg_boot *b, uint8_t state, uint8_t tries) {
    struct fwimg_boot n = *b;
    n.state             = state;
    n.tries             = tries;
    rep->writes++;
    return fwimg_boot_write(&n);
}

/* No update to carry on with: start the application in internal flash, or
 * if there is none, install one from a slot (the one with id want first,
 * else the previous, else the candidate), each tried once. */
static enum bootctl_result start(bool ext_ok, const uint8_t *want) {
    uint32_t size = 0;
    if (int_app(&size)) {
        return BOOTCTL_JUMP;
    }
    if (!ext_ok) {
        return BOOTCTL_HALT;
    }
    struct fwimg_hdr hc, hp;
    bool             vc         = slot_ok(FLASH_SLOT_CAND, NULL, &hc);
    bool             vp         = slot_ok(FLASH_SLOT_PREV, NULL, &hp);
    bool             cand_first = vc && want && memcmp(hc.sha256, want, FWIMG_ID_LEN) == 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        bool cand = (pass == 0) == cand_first;
        if (cand ? vc : vp) {
            rep->rescue++;
            if (install(cand ? FLASH_SLOT_CAND : FLASH_SLOT_PREV, cand ? &hc : &hp)) {
                return BOOTCTL_JUMP;
            }
        }
    }
    return BOOTCTL_HALT;
}

/* The image to put back first if internal flash holds none, for start().
 * After a rollback that was given up, the previous image: either its slot
 * is not valid (and start() skips it), or its installs were cut short
 * BOOTCTL_MAX_TRIES times, and then the candidate, which failed its test,
 * is the worse choice. */
static const uint8_t *wanted(const struct fwimg_boot *b) {
    switch (b->state) {
        case FWIMG_BOOT_CONFIRMED:
            return b->cand_id;
        case FWIMG_BOOT_ROLLED_BACK:
        case FWIMG_BOOT_REJECTED:
        case FWIMG_BOOT_NO_ROLLBACK:
            return b->prev_id;
        default:
            return NULL;
    }
}

enum bootctl_result bootctl_run(bool ext_ok, struct bootctl_report *r) {
    memset(r, 0, sizeof *r);
    rep = r;
    if (!ext_ok) {
        return start(false, NULL);
    }
    for (unsigned step = 0; step < MAX_STEPS; step++) {
        struct fwimg_boot b;
        (void)fwimg_boot_load(&b); /* FWIMG_BOOT_NONE if there is none */
        if (step == 0) {
            r->state_seen = b.state;
        }
        struct fwimg_hdr hc, hp;
        uint8_t          id[FWIMG_ID_LEN];
        switch (b.state) {
            case FWIMG_BOOT_PENDING:
                /* Both images must be whole: the candidate to install, and
                 * the previous one to fall back to. */
                if (!slot_ok(FLASH_SLOT_CAND, b.cand_id, &hc)
                    || !slot_ok(FLASH_SLOT_PREV, b.prev_id, &hp)) {
                    if (!set_state(&b, FWIMG_BOOT_REJECTED, 0)) {
                        return start(true, b.prev_id);
                    }
                } else if (b.tries >= BOOTCTL_MAX_TRIES) {
                    if (!set_state(&b, FWIMG_BOOT_ROLLBACK, 0)) {
                        return start(true, b.prev_id);
                    }
                } else if (!set_state(&b, FWIMG_BOOT_PENDING, (uint8_t)(b.tries + 1u))) {
                    return start(true, b.prev_id);
                } else if (install(FLASH_SLOT_CAND, &hc) && !set_state(&b, FWIMG_BOOT_TESTING, 0)) {
                    return start(true, b.cand_id);
                }
                break;
            case FWIMG_BOOT_TESTING:
                if (!int_id(id)) {
                    /* Nothing valid where the candidate should be. */
                    if (!set_state(&b, FWIMG_BOOT_ROLLBACK, 0)) {
                        return start(true, b.prev_id);
                    }
                } else if (memcmp(id, b.cand_id, FWIMG_ID_LEN) != 0) {
                    /* Another image was written over SWD: not ours to test. */
                    if (!set_state(&b, FWIMG_BOOT_EXTERNAL, 0)) {
                        return start(true, NULL);
                    }
                } else if (b.tries >= BOOTCTL_MAX_TRIES) {
                    /* Started BOOTCTL_MAX_TRIES times and never confirmed. */
                    if (!set_state(&b, FWIMG_BOOT_ROLLBACK, 0)) {
                        return start(true, b.prev_id);
                    }
                } else {
                    /* Count the boot before it happens. If the count cannot
                     * be written the candidate still starts: it is whole. */
                    (void)set_state(&b, FWIMG_BOOT_TESTING, (uint8_t)(b.tries + 1u));
                    return BOOTCTL_JUMP;
                }
                break;
            case FWIMG_BOOT_ROLLBACK:
                if (b.tries >= BOOTCTL_MAX_TRIES || !slot_ok(FLASH_SLOT_PREV, b.prev_id, &hp)) {
                    if (!set_state(&b, FWIMG_BOOT_NO_ROLLBACK, 0)) {
                        return start(true, b.cand_id);
                    }
                } else if (!set_state(&b, FWIMG_BOOT_ROLLBACK, (uint8_t)(b.tries + 1u))) {
                    return start(true, b.prev_id);
                } else if (install(FLASH_SLOT_PREV, &hp)
                           && !set_state(&b, FWIMG_BOOT_ROLLED_BACK, 0)) {
                    return start(true, b.prev_id);
                }
                break;
            default:
                return start(true, wanted(&b));
        }
    }
    return start(true, NULL);
}
