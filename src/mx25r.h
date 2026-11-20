#ifndef MX25R_H
#define MX25R_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Macronix MX25R1635F on SPI1, CS# PB9 (#10, #37). Reads anywhere; erases
 * and programs only inside the writable area selected with mx25r_area()
 * (src/flash_layout.h: the records by default, else the boot state or the
 * update slots, #24) and panics on any other address. No status/
 * configuration register write, no block or chip erase, no security
 * register write exist here.
 *
 * Radio task only (it owns SPI1, docs/design.md "Sharing SPI1 with the
 * radio"). Every function below is a series of short bus transactions; the
 * erase and program waits release the bus between status polls.
 *
 * Flash work while frames arrive (#22). The CC110L and this chip share
 * SPI1, and the Enhanced ACK depends on the radio task reading a Data
 * frame's header out of the RX FIFO while the frame is still arriving
 * (src/radio.c, ack_consider()). So the driver never holds the radio up:
 *   - no bus transaction starts while mx25r_port_radio_busy() (a frame
 *     arriving, or a frame of ours on the air): the driver serves the
 *     radio instead (gate); after MX25R_GATE_MAX_US of continuous busy it
 *     goes ahead anyway, since a pin stuck high must not stop the flash;
 *   - between the status polls of a program or erase cycle, which run in
 *     the chip with CS# high (mx25r1635f.txt:2149-2151, 2323-2325), it
 *     serves the radio, about once per 1 ms tick.
 * Erase/Program Suspend (mx25r1635f.txt:2581-2607) is not used: it exists to
 * read the array during a cycle (:2583-2586), and the ACK path never
 * touches the flash, only the bus, which is free while CS# is high. */

#define MX25R_JEDEC_ID 0xC22815u /* C2 28 15, mx25r1635f.txt:1123-1125 */

/* Longest wait for a quiet radio before a bus transaction: longer than the
 * radio's own stale-frame limit (RX_STALE_US, 60 ms, src/radio.c) plus an
 * Enhanced ACK on the air (8.5 ms with its margin, src/cc110l.c). */
#define MX25R_GATE_MAX_US 100000u

/* --- Radio port (src/radio.c; a simulated radio in tests/host) ------------- */
/* True while no flash transaction may start: a frame is arriving or not yet
 * taken from the FIFO, or a frame of ours is on the air. False before the
 * radio runs. */
bool mx25r_port_radio_busy(void);
/* Serve the radio once and give the CPU away: wait for a radio interrupt or
 * the next 1 ms tick, pump the RX FIFO (which prepares Enhanced ACKs and
 * completes frames), check in with the watchdog. Never touches the flash.
 * Before the radio runs: a one-tick delay and the watchdog check-in. */
void mx25r_port_radio_serve(void);

/* Counters since boot, for the radio task's minute log. */
struct mx25r_stats {
    uint32_t held;      /* transactions that waited for a quiet radio */
    uint32_t gave_up;   /* ... and went ahead after MX25R_GATE_MAX_US */
    uint32_t held_max;  /* longest such wait, us */
    uint32_t cycles;    /* program and erase cycles waited out */
    uint32_t cycle_max; /* longest cycle, us, from the command to WIP = 0 */
};
const struct mx25r_stats *mx25r_stats(void);

/* Writable areas (#24). One is selected at a time; the default, and the
 * only one the store (src/store.c) ever runs in, is the records area. The
 * update's flash work (src/fwupd.h, run by the radio task) selects the
 * slots or the boot state for one operation and then the records again.
 * The identity sectors and everything above the boot state are in no
 * area. */
enum mx25r_area {
    MX25R_AREA_RECORDS, /* FLASH_RECORDS_BEGIN..END: network record, counter log */
    MX25R_AREA_BOOT,    /* FLASH_BOOT_BEGIN..END: the boot state record */
    MX25R_AREA_SLOTS,   /* FLASH_SLOTS_BEGIN..END: candidate and previous image */
};
/* Select the area later erases and programs must lie in; returns the one
 * selected before. Radio task only. */
enum mx25r_area mx25r_area(enum mx25r_area a);

/* Largest program or read per bus transaction. SPI1 transactions run in a
 * critical section (src/spi1.c), so they are kept short: 4 + 64 bytes at
 * 6 MHz is about 90 us on the wire. */
#define MX25R_XFER_MAX 64u

/* Releases deep power-down by a CS# pulse (mx25r1635f.txt:2426-2433), waits
 * tRDP, then reads the JEDEC ID. Call from the SPI1 owner (radio task). */
bool mx25r_wake(void);
/* JEDEC ID as 0xMMTTDD (manufacturer, memory type, density). */
uint32_t mx25r_read_id(void);
/* Status register (WIP bit 0, WEL bit 1, BP bits 5:2, QE 6, SRWD 7;
 * mx25r1635f.txt:1380-1394). RDSR opcode 05h (mx25r1635f.txt:2647). */
uint8_t mx25r_read_status(void);

/* RDSR until WIP = 0, for up to 2 x tSE max (480 ms; a program or erase
 * the MCU did not see end, e.g. one cut short by an MCU reset, which does
 * not reset the chip). While WIP = 1 the chip ignores reads of the array
 * and does not decode RDID (mx25r1635f.txt:556-557, 968-969); RDSR works
 * at any time (:1142-1144). False if WIP is still set: treat the flash as
 * unavailable. Serves the radio between polls (radio task only). */
bool mx25r_wait_idle(void);

/* READ 03h, any length, in transactions of at most MX25R_XFER_MAX bytes. */
bool mx25r_read(uint32_t addr, uint8_t *dst, size_t n);

/* Page Program 02h of 1..MX25R_XFER_MAX bytes that do not cross a 256-byte
 * page, then waits for the end and checks WEL cleared and P_FAIL clear.
 * Panics outside the selected area. The caller reads back to verify. */
bool mx25r_program(uint32_t addr, const uint8_t *src, size_t n);

/* Sector Erase 20h of the 4 KB sector starting at addr (aligned), then
 * waits (up to 2 x tSE max) and checks WEL cleared and E_FAIL clear.
 * Panics outside the selected area. The caller reads back to verify. */
bool mx25r_erase_sector(uint32_t addr);

#endif
