/* Firmware images, update slots and the boot state (#24). See fwimg.h. */
#include "fwimg.h"

#include "flash_layout.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include <string.h>

_Static_assert(sizeof(struct fwimg_desc) == FWIMG_DESC_LEN, "descriptor layout");
_Static_assert(FWIMG_HDR_LEN <= FLASH_SLOT_IMAGE, "header before the image");
_Static_assert(FLASH_SLOT_IMAGE + FWIMG_APP_MAX <= FLASH_SLOT_SIZE, "a full image fits a slot");
_Static_assert(FWIMG_BOOT_LEN <= STORE_REC_PAYLOAD, "boot state fits a record");
_Static_assert(FWIMG_DESC_OFFSET % 4u == 0, "descriptor word aligned");

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint32_t fwimg_sector_addr(unsigned sector) {
    /* 4: 0x08010000 (64 KB); 5, 6, 7: 0x08020000 + 128 KB each */
    return sector <= 4u ? FWIMG_APP_BASE : 0x08020000u + (sector - 5u) * 0x20000u;
}

uint32_t fwimg_sector_size(unsigned sector) {
    return sector <= 4u ? 0x10000u : 0x20000u;
}

bool fwimg_app_check(const uint8_t head[FWIMG_HEAD_LEN], uint32_t *size) {
    const uint8_t *d       = head + FWIMG_DESC_OFFSET;
    uint32_t       end     = get32(d + 8);
    uint32_t       min_end = FWIMG_APP_BASE + FWIMG_HEAD_LEN;
    if (get32(d) != FWIMG_DESC_MAGIC || get16(d + 4) != FWIMG_DESC_VERSION
        || get16(d + 6) != FWIMG_DESC_LEN || end <= min_end || end > FWIMG_APP_BASE + FWIMG_APP_MAX
        || end % 4u != 0) {
        return false;
    }
    uint32_t sp = get32(head);
    uint32_t pc = get32(head + 4);
    if (sp <= FWIMG_RAM_BASE || sp > FWIMG_RAM_BASE + FWIMG_RAM_SIZE || sp % 4u != 0) {
        return false;
    }
    if (!(pc & 1u) || (pc & ~1u) < min_end || (pc & ~1u) >= end) {
        return false;
    }
    *size = end - FWIMG_APP_BASE;
    return true;
}

/* --- Slot header ---------------------------------------------------------- */
void fwimg_hdr_build(const struct fwimg_hdr *h, uint8_t raw[FWIMG_HDR_LEN]) {
    memset(raw, 0, FWIMG_HDR_LEN);
    put32(raw, FWIMG_HDR_MAGIC);
    put16(raw + 4, FWIMG_HDR_VERSION);
    put16(raw + 6, FWIMG_HDR_LEN);
    put32(raw + 8, h->size);
    put32(raw + 12, h->fw_version);
    put32(raw + 16, FWIMG_APP_BASE);
    memcpy(raw + 20, h->sha256, FWIMG_SHA_LEN);
    put32(raw + 60, store_crc32(raw, 60));
}

bool fwimg_hdr_parse(const uint8_t raw[FWIMG_HDR_LEN], struct fwimg_hdr *out) {
    if (get32(raw) != FWIMG_HDR_MAGIC || get16(raw + 4) != FWIMG_HDR_VERSION
        || get16(raw + 6) != FWIMG_HDR_LEN || get32(raw + 60) != store_crc32(raw, 60)
        || get32(raw + 16) != FWIMG_APP_BASE) {
        return false;
    }
    for (unsigned i = 52; i < 60; i++) {
        if (raw[i] != 0) {
            return false;
        }
    }
    uint32_t size = get32(raw + 8);
    if (size <= FWIMG_HEAD_LEN || size > FWIMG_APP_MAX || size % 4u != 0) {
        return false;
    }
    out->size       = size;
    out->fw_version = get32(raw + 12);
    memcpy(out->sha256, raw + 20, FWIMG_SHA_LEN);
    return true;
}

#define CHUNK 256u

bool fwimg_slot_verify(uint32_t slot, struct fwimg_hdr *out) {
    if (slot != FLASH_SLOT_CAND && slot != FLASH_SLOT_PREV) {
        return false;
    }
    uint8_t          raw[FWIMG_HDR_LEN];
    struct fwimg_hdr h;
    if (!store_port_read(slot, raw, sizeof raw) || !fwimg_hdr_parse(raw, &h)) {
        return false;
    }
    /* The head first: a slot that does not even hold an application is
     * refused without hashing up to 448 KB. */
    static uint8_t buf[FWIMG_HEAD_LEN > CHUNK ? FWIMG_HEAD_LEN : CHUNK];
    uint32_t       size = 0;
    if (!store_port_read(slot + FLASH_SLOT_IMAGE, buf, FWIMG_HEAD_LEN)
        || !fwimg_app_check(buf, &size) || size != h.size) {
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    for (uint32_t off = 0; ok && off < h.size; off += CHUNK) {
        uint32_t n = h.size - off < CHUNK ? h.size - off : CHUNK;
        ok         = store_port_read(slot + FLASH_SLOT_IMAGE + off, buf, n)
                     && mbedtls_sha256_update(&c, buf, n) == 0;
        if ((off & 0xFFFu) == 0) {
            fwimg_port_progress();
        }
    }
    uint8_t sha[FWIMG_SHA_LEN];
    ok = ok && mbedtls_sha256_finish(&c, sha) == 0 && memcmp(sha, h.sha256, FWIMG_SHA_LEN) == 0;
    mbedtls_sha256_free(&c);
    if (ok) {
        *out = h;
    }
    return ok;
}

/* --- Boot state ------------------------------------------------------------- */
static bool state_known(uint8_t s) {
    return s >= FWIMG_BOOT_CONFIRMED && s <= FWIMG_BOOT_EXTERNAL;
}

bool fwimg_boot_load(struct fwimg_boot *out) {
    uint8_t  p[FWIMG_BOOT_LEN];
    uint32_t seq = 0;
    memset(out, 0, sizeof *out);
    if (!store_slot_load(
            FLASH_BOOT_A, FWIMG_BOOT_MAGIC, FWIMG_BOOT_VERSION, p, FWIMG_BOOT_LEN, &seq)
        || !state_known(p[0]) || p[2] != 0 || p[3] != 0) {
        return false;
    }
    out->state = p[0];
    out->tries = p[1];
    memcpy(out->cand_id, p + 4, FWIMG_ID_LEN);
    memcpy(out->prev_id, p + 4 + FWIMG_ID_LEN, FWIMG_ID_LEN);
    return true;
}

bool fwimg_boot_write(const struct fwimg_boot *b) {
    if (!state_known(b->state)) {
        return false;
    }
    uint8_t p[FWIMG_BOOT_LEN] = {b->state, b->tries, 0, 0};
    memcpy(p + 4, b->cand_id, FWIMG_ID_LEN);
    memcpy(p + 4 + FWIMG_ID_LEN, b->prev_id, FWIMG_ID_LEN);
    if (!store_slot_write(
            FLASH_BOOT_A, FWIMG_BOOT_MAGIC, FWIMG_BOOT_VERSION, p, FWIMG_BOOT_LEN, NULL)) {
        return false;
    }
    struct fwimg_boot back;
    return fwimg_boot_load(&back) && memcmp(&back, b, sizeof back) == 0;
}
