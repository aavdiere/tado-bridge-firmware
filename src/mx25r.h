#ifndef MX25R_H
#define MX25R_H

#include <stdbool.h>
#include <stdint.h>

/* Macronix MX25R1635F on SPI1, CS# PB9 (#10). READ-ONLY in P2: the chip holds
 * the stock configuration, so only RDID (0x9F) and RDSR (0x05) exist here.
 * There is no write enable, program, erase or status/config write path. */

#define MX25R_JEDEC_ID 0xC22815u /* C2 28 15, mx25r1635f.txt:1123-1125 */

/* Releases deep power-down by a CS# pulse (mx25r1635f.txt:2426-2433), waits
 * tRDP, then reads the JEDEC ID. Call from the SPI1 owner (radio task). */
bool mx25r_wake(void);
/* JEDEC ID as 0xMMTTDD (manufacturer, memory type, density). */
bool mx25r_read_id(uint32_t *id);
/* Status register (WIP bit 0, WEL bit 1, BP bits 5:2, QE 6, SRWD 7;
 * mx25r1635f.txt:1380-1394). RDSR opcode 05h (mx25r1635f.txt:2647). */
bool mx25r_read_status(uint8_t *sr);

#endif
