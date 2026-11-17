#ifndef STORE_H
#define STORE_H

/* Network record and counter log in external flash (#37), docs/design.md
 * "Slotted records" and "Counter log". Hardware independent: the flash is
 * reached through the three store_port_* functions below (src/store_port.c
 * on the bridge, a flash model with power cuts in tests/host/test_store.c).
 * Radio task only on the bridge (it owns SPI1).
 *
 * Power-cut rules [I]:
 *   - A slotted record is written to the older or invalid slot: erase,
 *     check blank, program everything but the magic, read back, then
 *     program the magic (the commit mark) and read back the whole record.
 *     A record without its magic, or with a wrong CRC-32, does not exist,
 *     so a cut at any step leaves the other slot as the newest valid one.
 *   - A counter-log entry is a value and its bitwise inverse. Programming
 *     only clears bits, so a torn entry never has a matching pair; the
 *     highest valid entry is the reservation. A full sector moves on to the
 *     other one, which is erased first; the full one keeps the highest
 *     value until the first entry of the new one is written. The sector
 *     holding the highest valid entry is never erased: if every slot of
 *     the new sector fails, appends fail until the next boot.
 *
 * Nothing here logs, and no key byte leaves the structs the caller passes. */

#include "ccm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Port (provided by the platform) ---------------------------------------- */
/* Read any range. */
bool store_port_read(uint32_t addr, uint8_t *dst, size_t n);
/* Program 1..STORE_XFER_MAX bytes within one 256-byte page. */
bool store_port_program(uint32_t addr, const uint8_t *src, size_t n);
/* Erase the 4 KB sector at addr. */
bool store_port_erase(uint32_t addr);

#define STORE_XFER_MAX 64u /* = MX25R_XFER_MAX */

/* --- Slotted records ------------------------------------------------------- */
/* magic u32 | version u16 | length u16 | seq u32 | payload | CRC-32, all
 * little endian; CRC-32 (IEEE 802.3, reflected) over everything before it. */
#define STORE_REC_HDR     12u
#define STORE_REC_PAYLOAD 64u /* largest payload */

/* The valid slot with the highest seq at slot_a or slot_a + 4 KB: true and
 * *seq set if there is one. Reads only the magic of a slot that is not ours
 * (the stock configuration may sit there). */
bool store_slot_load(uint32_t  slot_a,
                     uint32_t  magic,
                     uint16_t  version,
                     uint8_t  *payload,
                     uint16_t  len,
                     uint32_t *seq);
/* Writes a new generation into the older or invalid slot; *written (if
 * not NULL) is its seq. */
bool store_slot_write(uint32_t       slot_a,
                      uint32_t       magic,
                      uint16_t       version,
                      const uint8_t *payload,
                      uint16_t       len,
                      uint32_t      *written);

uint32_t store_crc32(const uint8_t *p, size_t n);

/* --- Network record -------------------------------------------------------- */
#define STORE_NET_MAGIC   0x3154454Eu /* "NET1" */
#define STORE_NET_VERSION 1u
#define STORE_NET_LEN     24u /* pan u16 | channel u8 | 0 u8 | key[16] | counter floor u32 */

struct store_net {
    uint16_t pan_id;
    uint8_t  channel;
    uint8_t  key[CCM_KEY_LEN];
    uint32_t counter_floor; /* counters below this were possibly used */
};

/* Is this a network the bridge may run? For now only the test channel and
 * the tado PAN (docs/test-bench.md; never the protected channel 26). */
bool store_net_allowed(const struct store_net *n);

/* The newest valid network record that store_net_allowed() accepts. */
bool store_net_load(struct store_net *out, uint32_t *seq);
/* Refuses a network store_net_allowed() rejects. *seq (if not NULL): the
 * seq written. */
bool store_net_write(const struct store_net *n, uint32_t *seq);
/* Writes n, then loads the network as a boot would: true only if the
 * newest valid record is the one just written (its seq) and equal to n.
 * *out is the record read back (wiped on false), *seq its seq. */
bool store_net_commit(const struct store_net *n, struct store_net *out, uint32_t *seq);

/* --- Counter log ----------------------------------------------------------- */
#define STORE_CTR_ENTRY   8u
#define STORE_CTR_ENTRIES (4096u / STORE_CTR_ENTRY) /* 512 per sector */
#define STORE_CTR_AHEAD   4096u /* reserve this far ahead (freedo docs/PROTOCOL.md:1296-1298) */
#define STORE_CTR_LOW     2048u /* extend once less than this remains */

/* Forget what is known about the log (the next call scans it again). */
void store_ctr_forget(void);
/* Highest reservation in the log: false if the log cannot be read; *found
 * false if it holds no valid entry. */
bool store_ctr_highest(uint32_t *max, bool *found);

/* Boot, before the first frame: start from the highest reservation (or
 * floor if higher), then persist start + STORE_CTR_AHEAD. False, and
 * nothing may be sent, if that write fails or the counter space is used up.
 * *first: the first counter to use; *limit: the first one not reserved. */
bool store_counter_boot(uint32_t floor, uint32_t *first, uint32_t *limit);

/* Running: next is the next counter the coordinator would use (<= *limit).
 * Once less than STORE_CTR_LOW remains, persists next + STORE_CTR_AHEAD and
 * moves *limit there. False if that write failed (*limit unchanged; try
 * again later; once failed writes have used up the slots of the sector
 * after the highest entry's, it stays false until the next boot). */
bool store_counter_extend(uint32_t next, uint32_t *limit);

#endif
