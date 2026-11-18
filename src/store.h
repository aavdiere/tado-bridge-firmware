#ifndef STORE_H
#define STORE_H

/* Identity record (#21), network record and counter log in external flash
 * (#37), docs/design.md "Slotted records" and "Counter log". Hardware
 * independent: the flash is reached through the three store_port_*
 * functions below (src/store_port.c on the bridge, flash models with power
 * cuts in tests/host/test_store.c and test_identity.c, the sector image in
 * tools/identity/identity.c).
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

/* --- Identity record (#21) ------------------------------------------------- */
/* The bridge's serial and EUI-64, at FLASH_ID_A / FLASH_ID_B. Written once
 * at provisioning by the host tool (tools/identity/), which links this file
 * and calls store_id_write() against an image of the two sectors; the
 * firmware only loads it (its flash driver refuses writes there). */
#define STORE_ID_MAGIC      0x314E4449u /* "IDN1" */
#define STORE_ID_VERSION    1u
#define STORE_ID_SERIAL_MAX 16u
#define STORE_ID_LEN        24u /* serial[16] ASCII, zero padded | EUI-64[8] */

struct store_id {
    char    serial[STORE_ID_SERIAL_MAX + 1u]; /* NUL terminated */
    uint8_t eui64[8];                         /* stored order, 00 1b c5 first (docs/build.md P4) */
};

/* Is this an identity the bridge may use? Serial: 3 to 16 characters, two
 * capital letters then capital letters or digits, as tanoclo keys its
 * `devices` table ("IB" and 10 digits for a bridge), NUL after it. EUI-64:
 * tado's OUI 00 1b c5 first (the wire suffix the radio checks,
 * src/mac.c mac_eui_suffix). */
bool store_id_valid(const struct store_id *id);
/* Payload layout: false (and p zeroed) if !store_id_valid(id). */
bool store_id_pack(const struct store_id *id, uint8_t p[STORE_ID_LEN]);
/* False (and *out zeroed) if the payload does not make a valid identity:
 * padding not zero, serial or EUI-64 refused by store_id_valid(). */
bool store_id_unpack(const uint8_t p[STORE_ID_LEN], struct store_id *out);

/* The newest valid identity record (highest seq), if store_id_unpack()
 * accepts it. An older slot is not used in its place: a newest record
 * that is refused leaves the bridge without identity. */
bool store_id_load(struct store_id *out, uint32_t *seq);
/* Host provisioning tool only: on the bridge, src/mx25r.c panics on a
 * write to the identity sectors. Refuses an invalid identity. */
bool store_id_write(const struct store_id *id, uint32_t *seq);

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
/* Is there any network record at all, valid or not? Reads the first four
 * bytes of both slots: *any is true if either holds the record's magic,
 * which is programmed last (the commit mark), so a write that was cut
 * before it left no magic. False if a read failed. A network may be
 * created only where there has never been one (#19): with *any true and no
 * valid record, the record is damaged or refused (another channel), and
 * devices may still be paired to the network it held. */
bool store_net_probe(bool *any);
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

/* --- Factory reset (P6, docs/design.md "Lifecycle") ------------------------- */
/* Erases both network slots, then, only once both read back blank, both
 * counter-log sectors. The identity sectors are not touched (and lie
 * outside the flash driver's write range, src/flash_layout.h). The order
 * keeps the counter log as long as any network record can still be loaded,
 * so a power cut at any step never leaves a network whose counters restart
 * below ones already used on the air: a cut leaves the old network (with
 * its whole counter log), a damaged record (no key, no new network over
 * it), or no network. False if an erase failed or a sector did not read
 * back blank; the counter log is then untouched unless both network slots
 * were already blank. Call again to finish. */
bool store_factory_reset(void);

#endif
