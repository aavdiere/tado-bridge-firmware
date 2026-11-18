#ifndef FWIMG_H
#define FWIMG_H

/* Firmware images, update slots and the boot state (#24): docs/design.md
 * "Self-update and first flash (#20)" and "Implementation: bootloader and
 * boot state (#24)". Shared by the bootloader (src/boot/) and the
 * application. Hardware independent: the external flash is reached through
 * store_port_* (src/store.h), SHA-256 comes from mbedTLS.
 *
 * Internal flash (rm0383.txt:2023-2031): sectors 0-3 (64 KB) hold the
 * bootloader, sectors 4-7 (64 + 3 x 128 KB) the application.
 *
 * The application image begins with its vector table, and at
 * FWIMG_DESC_OFFSET carries a descriptor (struct fwimg_desc) placed there by
 * the linker script (cmake/sections.ld). The descriptor's first word, the
 * magic, is the image's commit mark in internal flash: the bootloader
 * programs it last, after everything else is written and checked, so an
 * install cut short never looks like an application. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Internal flash --------------------------------------------------------- */
#define FWIMG_APP_BASE         0x08010000u
#define FWIMG_APP_MAX          (448u * 1024u)
#define FWIMG_APP_FIRST_SECTOR 4u
#define FWIMG_APP_LAST_SECTOR  7u
#define FWIMG_RAM_BASE         0x20000000u
#define FWIMG_RAM_SIZE         (128u * 1024u)

/* Start address and size of internal flash sector 4..7. */
uint32_t fwimg_sector_addr(unsigned sector);
uint32_t fwimg_sector_size(unsigned sector);

/* --- Application descriptor -------------------------------------------------- */
#define FWIMG_DESC_OFFSET  0x200u      /* after the vector table (428 bytes on the F411) */
#define FWIMG_DESC_MAGIC   0x31445442u /* "BTD1" */
#define FWIMG_DESC_VERSION 1u
#define FWIMG_DESC_LEN     32u
/* Bytes from the image start that hold the vector table and descriptor. */
#define FWIMG_HEAD_LEN (FWIMG_DESC_OFFSET + FWIMG_DESC_LEN)

struct fwimg_desc {
    uint32_t magic;      /* FWIMG_DESC_MAGIC; programmed last at install */
    uint16_t version;    /* FWIMG_DESC_VERSION */
    uint16_t len;        /* FWIMG_DESC_LEN */
    uint32_t image_end;  /* first address after the image (linker: _image_end) */
    uint32_t fw_version; /* informational; 0 for now */
    uint32_t reserved[4];
};

/* Is head (the first FWIMG_HEAD_LEN bytes of an image linked at
 * FWIMG_APP_BASE) an application the bootloader may start? Checks the
 * descriptor (magic, version, length, image_end inside the region, the
 * size a multiple of 4), the initial stack pointer (word aligned, inside
 * RAM or its top) and the reset vector (Thumb, inside the image after the
 * descriptor). *size: the image size the descriptor gives. */
bool fwimg_app_check(const uint8_t head[FWIMG_HEAD_LEN], uint32_t *size);

/* --- Slot header (external flash, FLASH_SLOT_CAND / FLASH_SLOT_PREV) ------- */
/* magic u32 | version u16 | len u16 | size u32 | fw_version u32 |
 * load_addr u32 | sha256[32] | reserved[8] (0) | CRC-32 u32 over bytes 0-59;
 * little endian. The image follows at slot + FLASH_SLOT_IMAGE. */
#define FWIMG_HDR_MAGIC   0x31574654u /* "TFW1" */
#define FWIMG_HDR_VERSION 1u
#define FWIMG_HDR_LEN     64u
#define FWIMG_SHA_LEN     32u
#define FWIMG_ID_LEN      16u /* an image's id: the first 16 bytes of its SHA-256 */

struct fwimg_hdr {
    uint32_t size;
    uint32_t fw_version;
    uint8_t  sha256[FWIMG_SHA_LEN];
};

void fwimg_hdr_build(const struct fwimg_hdr *h, uint8_t raw[FWIMG_HDR_LEN]);
/* Magic, version, length, CRC-32, load address, reserved bytes zero, size
 * 1..FWIMG_APP_MAX past the descriptor and a multiple of 4. */
bool fwimg_hdr_parse(const uint8_t raw[FWIMG_HDR_LEN], struct fwimg_hdr *out);

/* Does the slot hold a whole valid image? Header parses; the image's
 * SHA-256, read back from the slot, matches it; the image's head passes
 * fwimg_app_check() with the header's size. *out: the header. False on any
 * read failure. Calls fwimg_port_progress() between chunks. */
bool fwimg_slot_verify(uint32_t slot, struct fwimg_hdr *out);

/* --- Boot state record (FLASH_BOOT_A / FLASH_BOOT_B) ------------------------ */
/* A slotted record (src/store.h): magic "BOT1", version 1, payload:
 * state u8 | tries u8 | 0 u16 | cand_id[16] | prev_id[16]. */
#define FWIMG_BOOT_MAGIC   0x31544F42u /* "BOT1" */
#define FWIMG_BOOT_VERSION 1u
#define FWIMG_BOOT_LEN     36u

enum fwimg_boot_state {
    FWIMG_BOOT_NONE      = 0,   /* no record (or one this code does not know) */
    FWIMG_BOOT_CONFIRMED = 1,   /* the candidate (cand_id) runs and was confirmed */
    FWIMG_BOOT_PENDING   = 2,   /* install cand_id; prev_id saved; tries: installs begun
                                 * (after BOOTCTL_MAX_TRIES: the update is given up) */
    FWIMG_BOOT_TESTING     = 3, /* cand_id installed, unconfirmed; tries: boots into it */
    FWIMG_BOOT_ROLLBACK    = 4, /* install prev_id; tries: installs begun */
    FWIMG_BOOT_ROLLED_BACK = 5, /* prev_id restored after the candidate failed */
    FWIMG_BOOT_REJECTED    = 6, /* candidate or previous slot not valid at install: nothing done */
    FWIMG_BOOT_NO_ROLLBACK = 7, /* rollback given up (previous slot not valid, or its install
                                 * begun BOOTCTL_MAX_TRIES times): the application in internal
                                 * flash is kept; if there is none, the previous image is put
                                 * in, else the candidate */
    FWIMG_BOOT_EXTERNAL = 8,    /* under test, but another image was written (SWD): left alone */
};

struct fwimg_boot {
    uint8_t state; /* enum fwimg_boot_state */
    uint8_t tries;
    uint8_t cand_id[FWIMG_ID_LEN];
    uint8_t prev_id[FWIMG_ID_LEN];
};

/* The newest valid boot state; state FWIMG_BOOT_NONE (and false) if there
 * is none or its state is not one of the above. */
bool fwimg_boot_load(struct fwimg_boot *out);
/* Write a new generation, then load it back: true only if the newest
 * record is now exactly b. */
bool fwimg_boot_write(const struct fwimg_boot *b);

/* --- Port --------------------------------------------------------------------- */
/* Called between chunks of long work (hashing a slot): the bootloader kicks
 * the IWDG, the application checks in with its watchdog. */
void fwimg_port_progress(void);

#endif
