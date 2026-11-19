#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

/* External flash (MX25R1635F, 2 MB) layout, docs/design.md "Layout
 * (external flash)". Only the network and counter-log sectors are ever
 * erased or programmed by this firmware (#37): src/mx25r.c panics on any
 * erase or program outside [FLASH_WRITE_BEGIN, FLASH_WRITE_END). The
 * identity sectors are read only (#21).
 *
 * Erase unit 4 KB sector, program unit 256-byte page (mx25r1635f.txt:219,
 * 2136-2139, 2308-2316). */

#define FLASH_SIZE   0x200000u /* 16 Mbit */
#define FLASH_SECTOR 4096u
#define FLASH_PAGE   256u

/* Update slots (#20, #24; docs/design.md "Self-update and first flash"):
 * the candidate image and the previous image, 512 KB each. A slot's first
 * 4 KB sector holds the slot header (src/fwimg.h), the image starts at
 * FLASH_SLOT_IMAGE. The bench unit's stock image has data in both slots
 * (0x000000-0x03DFFF, 0x080000-0x0BEFFF, docs/design.md #37 notes); it is
 * never taken for an image of ours (header magic, CRC-32 and SHA-256). */
#define FLASH_SLOT_CAND  0x000000u
#define FLASH_SLOT_PREV  0x080000u
#define FLASH_SLOT_SIZE  0x080000u
#define FLASH_SLOT_IMAGE 0x1000u /* image offset inside a slot */

/* Identity record, slots A and B (#21, docs/design.md "Slotted records").
 * Written only by the host provisioning tool over SWD (tools/identity/),
 * never by this firmware: both sectors lie below FLASH_WRITE_BEGIN, so
 * src/mx25r.c panics on any erase or program there. The bench unit's
 * stock image has data in 0x100000-0x100FFF and nothing in 0x101000-
 * 0x101FFF (docs/design.md, #37 notes); the first identity write goes to
 * slot A and replaces that stock sector, which the layout accepts because
 * a backup exists. */
#define FLASH_ID_A 0x100000u
#define FLASH_ID_B 0x101000u

/* Network record, slots A and B (docs/design.md "Slotted records"). The
 * stock firmware keeps its active configuration partition at 0x102000 and
 * its backup at 0x103000 (tanoclo/patch_ib_firmware/extract_channel.js:6,
 * 66-67), so the first network record written here replaces the stock
 * configuration; docs/design.md "Layout (external flash)" accepts that
 * because a backup exists. */
#define FLASH_NET_A 0x102000u
#define FLASH_NET_B 0x103000u
/* Counter log, sectors A and B (docs/design.md "Counter log"). Blank in the
 * bench unit's stock image (docs/design.md, #37 notes). */
#define FLASH_CTR_A 0x104000u
#define FLASH_CTR_B 0x105000u

/* Boot state, slots A and B (#24): a slotted record (src/fwimg.h) that the
 * bootloader and the application hand the update over with. The bootloader
 * erases and programs only these two sectors (src/boot/bl_hw.c). */
#define FLASH_BOOT_A 0x106000u
#define FLASH_BOOT_B 0x107000u

#define FLASH_WRITE_BEGIN FLASH_NET_A
#define FLASH_WRITE_END   (FLASH_CTR_B + FLASH_SECTOR) /* 0x106000, exclusive */

_Static_assert(FLASH_SLOT_PREV == FLASH_SLOT_CAND + FLASH_SLOT_SIZE, "slots adjacent");
_Static_assert(FLASH_SLOT_PREV + FLASH_SLOT_SIZE == FLASH_ID_A, "identity after the slots");
_Static_assert(FLASH_SLOT_CAND % 0x10000u == 0 && FLASH_SLOT_PREV % 0x10000u == 0,
               "slots 64 KB-block aligned");
_Static_assert(FLASH_BOOT_A == FLASH_CTR_B + FLASH_SECTOR, "boot state after the counter log");
_Static_assert(FLASH_BOOT_B == FLASH_BOOT_A + FLASH_SECTOR, "boot state slots adjacent");

_Static_assert(FLASH_ID_B == FLASH_ID_A + FLASH_SECTOR, "identity slots adjacent");
_Static_assert(FLASH_NET_A == FLASH_ID_B + FLASH_SECTOR, "network after identity");
_Static_assert(FLASH_ID_B + FLASH_SECTOR <= FLASH_WRITE_BEGIN,
               "the firmware cannot erase or program the identity sectors");
_Static_assert(FLASH_NET_B == FLASH_NET_A + FLASH_SECTOR, "network slots adjacent");
_Static_assert(FLASH_CTR_A == FLASH_NET_B + FLASH_SECTOR, "counter log after network");
_Static_assert(FLASH_WRITE_END == 0x106000u, "boot state (#20) starts at 0x106000");
_Static_assert(FLASH_WRITE_BEGIN % FLASH_SECTOR == 0 && FLASH_WRITE_END % FLASH_SECTOR == 0,
               "sector aligned");

#endif
