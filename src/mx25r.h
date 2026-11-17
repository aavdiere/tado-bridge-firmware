#ifndef MX25R_H
#define MX25R_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Macronix MX25R1635F on SPI1, CS# PB9 (#10, #37). Reads anywhere; erases
 * and programs only inside [FLASH_WRITE_BEGIN, FLASH_WRITE_END) of
 * src/flash_layout.h (the network and counter-log sectors) and panics on
 * any other address. No status/configuration register write, no block or
 * chip erase, no security register write exist here.
 *
 * Radio task only (it owns SPI1, docs/design.md "Sharing SPI1 with the
 * radio"). Every function below is a series of short bus transactions; the
 * erase and program waits release the bus between status polls. */

#define MX25R_JEDEC_ID 0xC22815u /* C2 28 15, mx25r1635f.txt:1123-1125 */

/* Largest program or read per bus transaction. SPI1 transactions run in a
 * critical section (src/spi1.c), so they are kept short: 4 + 64 bytes at
 * 6 MHz is about 90 us on the wire. */
#define MX25R_XFER_MAX 64u

/* Releases deep power-down by a CS# pulse (mx25r1635f.txt:2426-2433), waits
 * tRDP, then reads the JEDEC ID. Call from the SPI1 owner (radio task). */
bool mx25r_wake(void);
/* JEDEC ID as 0xMMTTDD (manufacturer, memory type, density). */
bool mx25r_read_id(uint32_t *id);
/* Status register (WIP bit 0, WEL bit 1, BP bits 5:2, QE 6, SRWD 7;
 * mx25r1635f.txt:1380-1394). RDSR opcode 05h (mx25r1635f.txt:2647). */
bool mx25r_read_status(uint8_t *sr);

/* READ 03h, any length, in transactions of at most MX25R_XFER_MAX bytes. */
bool mx25r_read(uint32_t addr, uint8_t *dst, size_t n);

/* Page Program 02h of 1..MX25R_XFER_MAX bytes that do not cross a 256-byte
 * page, then waits for the end and checks WEL cleared and P_FAIL clear.
 * Panics outside the writable range. The caller reads back to verify. */
bool mx25r_program(uint32_t addr, const uint8_t *src, size_t n);

/* Sector Erase 20h of the 4 KB sector starting at addr (aligned), then
 * waits (up to 2 x tSE max) and checks WEL cleared and E_FAIL clear.
 * Panics outside the writable range. The caller reads back to verify. */
bool mx25r_erase_sector(uint32_t addr);

#endif
