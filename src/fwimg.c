/* Firmware images, update slots and the boot state (#24). See fwimg.h. */
#include "fwimg.h"

#include "flash_layout.h"
#include "le.h"
#include "store.h"

#include "mbedtls/sha256.h"

#include <string.h>

_Static_assert(sizeof(struct fwimg_desc) == FWIMG_DESC_LEN, "descriptor layout");
_Static_assert(FWIMG_HDR_LEN <= FLASH_SLOT_IMAGE, "header before the image");
_Static_assert(FLASH_SLOT_IMAGE + FWIMG_APP_MAX <= FLASH_SLOT_SIZE, "a full image fits a slot");
_Static_assert(FWIMG_BOOT_LEN <= STORE_REC_PAYLOAD, "boot state fits a record");
_Static_assert(FWIMG_DESC_OFFSET % 4u == 0, "descriptor word aligned");

/* Internal flash sectors of the application (rm0383.txt:2028-2031): sector 4
 * is 64 KB at FWIMG_APP_BASE, sectors 5, 6 and 7 are 128 KB each from
 * 0x08020000. */
#define SECTOR4_SIZE 0x10000u
#define SECTOR5_BASE 0x08020000u
#define SECTOR5_SIZE 0x20000u /* also sectors 6 and 7 */
_Static_assert(FWIMG_APP_BASE + SECTOR4_SIZE == SECTOR5_BASE, "sector 5 follows sector 4");
_Static_assert(SECTOR4_SIZE + 3u * SECTOR5_SIZE == FWIMG_APP_MAX, "sectors 4-7 are the region");

uint32_t fwimg_sector_addr(unsigned sector) {
    return sector <= FWIMG_APP_FIRST_SECTOR
               ? FWIMG_APP_BASE
               : SECTOR5_BASE + (sector - (FWIMG_APP_FIRST_SECTOR + 1u)) * SECTOR5_SIZE;
}

uint32_t fwimg_sector_size(unsigned sector) {
    return sector <= FWIMG_APP_FIRST_SECTOR ? SECTOR4_SIZE : SECTOR5_SIZE;
}

/* The image's first two vector table words: initial stack pointer, reset
 * vector. */
#define VT_OFF_SP    0u
#define VT_OFF_RESET 4u

/* The descriptor is read by its struct's offsets; a bootloader in the field
 * reads these. */
_Static_assert(offsetof(struct fwimg_desc, magic) == 0u
                   && offsetof(struct fwimg_desc, version) == 4u
                   && offsetof(struct fwimg_desc, len) == 6u
                   && offsetof(struct fwimg_desc, image_end) == 8u,
               "descriptor field offsets");

bool fwimg_app_check(const uint8_t head[FWIMG_HEAD_LEN], uint32_t *size) {
    const uint8_t *d       = head + FWIMG_DESC_OFFSET;
    uint32_t       end     = le_get32(d + offsetof(struct fwimg_desc, image_end));
    uint32_t       min_end = FWIMG_APP_BASE + FWIMG_HEAD_LEN;
    if (le_get32(d + offsetof(struct fwimg_desc, magic)) != FWIMG_DESC_MAGIC
        || le_get16(d + offsetof(struct fwimg_desc, version)) != FWIMG_DESC_VERSION
        || le_get16(d + offsetof(struct fwimg_desc, len)) != FWIMG_DESC_LEN || end <= min_end
        || end > FWIMG_APP_BASE + FWIMG_APP_MAX || end % 4u != 0) {
        return false;
    }
    uint32_t sp = le_get32(head + VT_OFF_SP);
    uint32_t pc = le_get32(head + VT_OFF_RESET);
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
/* Field offsets of the layout in fwimg.h. */
#define HDR_OFF_MAGIC      0u
#define HDR_OFF_VERSION    4u
#define HDR_OFF_LEN        6u
#define HDR_OFF_SIZE       8u
#define HDR_OFF_FW_VERSION 12u
#define HDR_OFF_LOAD_ADDR  16u
#define HDR_OFF_SHA        20u
#define HDR_OFF_RESERVED   (HDR_OFF_SHA + FWIMG_SHA_LEN) /* 8 bytes, 0 */
#define HDR_OFF_CRC        60u                           /* CRC-32 over the bytes before it */
_Static_assert(HDR_OFF_RESERVED == 52u && HDR_OFF_CRC + 4u == FWIMG_HDR_LEN, "slot header layout");

void fwimg_hdr_build(const struct fwimg_hdr *h, uint8_t raw[FWIMG_HDR_LEN]) {
    memset(raw, 0, FWIMG_HDR_LEN);
    le_put32(raw + HDR_OFF_MAGIC, FWIMG_HDR_MAGIC);
    le_put16(raw + HDR_OFF_VERSION, FWIMG_HDR_VERSION);
    le_put16(raw + HDR_OFF_LEN, FWIMG_HDR_LEN);
    le_put32(raw + HDR_OFF_SIZE, h->size);
    le_put32(raw + HDR_OFF_FW_VERSION, h->fw_version);
    le_put32(raw + HDR_OFF_LOAD_ADDR, FWIMG_APP_BASE);
    memcpy(raw + HDR_OFF_SHA, h->sha256, FWIMG_SHA_LEN);
    le_put32(raw + HDR_OFF_CRC, store_crc32(raw, HDR_OFF_CRC));
}

bool fwimg_hdr_parse(const uint8_t raw[FWIMG_HDR_LEN], struct fwimg_hdr *out) {
    if (le_get32(raw + HDR_OFF_MAGIC) != FWIMG_HDR_MAGIC
        || le_get16(raw + HDR_OFF_VERSION) != FWIMG_HDR_VERSION
        || le_get16(raw + HDR_OFF_LEN) != FWIMG_HDR_LEN
        || le_get32(raw + HDR_OFF_CRC) != store_crc32(raw, HDR_OFF_CRC)
        || le_get32(raw + HDR_OFF_LOAD_ADDR) != FWIMG_APP_BASE) {
        return false;
    }
    for (unsigned i = HDR_OFF_RESERVED; i < HDR_OFF_CRC; i++) {
        if (raw[i] != 0) {
            return false;
        }
    }
    uint32_t size = le_get32(raw + HDR_OFF_SIZE);
    if (size <= FWIMG_HEAD_LEN || size > FWIMG_APP_MAX || size % 4u != 0) {
        return false;
    }
    out->size       = size;
    out->fw_version = le_get32(raw + HDR_OFF_FW_VERSION);
    memcpy(out->sha256, raw + HDR_OFF_SHA, FWIMG_SHA_LEN);
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
