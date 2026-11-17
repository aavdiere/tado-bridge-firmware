#ifndef AIRTIME_H
#define AIRTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rolling airtime limit for the transmitter (#17, duty cycle).
 *
 * The test channel 10 (865.1245 MHz) lies in the 865-868 MHz SRD band:
 * 25 mW e.r.p. and a duty cycle of at most 1 %, or listen-before-talk with
 * adaptive frequency agility instead [I: ERC Recommendation 70-03, Annex
 * 1, band h1.4, as recalled; not checked against a copy of the
 * Recommendation in this repository]. The duty cycle is taken over one
 * hour (EN 300 220 [I]): at most 36 s of transmission per hour. The
 * bridge's listen-before-transmit (docs/radio-runtime.md) is not the
 * regulated LBT+AFA, so the 1 % limit is the one that applies.
 *
 * The limit here is AIRTIME_BUDGET_US = 30 s per rolling hour, 6 s below
 * the 1 %. Every frame the driver sends is charged before it is keyed:
 * Enhanced ACKs, wake-up trains, Router Advertisements, Data frames. A
 * frame that does not fit is refused, not delayed.
 *
 * Window: AIRTIME_BUCKETS one-minute buckets, the current one and 60 full
 * ones before it, so a charge is weighed against everything charged in at
 * least the last 60 minutes (at most 61). Time comes from the 32-bit
 * microsecond clock. Elapsed time is taken modulo 2^32: after a gap of
 * more than 71 minutes between two calls the limiter sees less time than
 * has passed and keeps old charges longer, never shorter. Between calls
 * nothing is charged, so this errs only towards refusing.
 *
 * Plain C, no locking: one caller context only (the radio task; the
 * Enhanced ACK is charged when the task prepares it, not in the interrupt
 * that sends it). */

#define AIRTIME_BUDGET_US 30000000u /* 30 s; 1 % of an hour would be 36 s */
#define AIRTIME_BUCKET_US 60000000u
#define AIRTIME_BUCKETS   61u

/* Charge for a frame of len bytes (the MAC frame without the radio's
 * length byte and CRC): 4 preamble, 4 sync, the length byte and 2 CRC
 * bytes around it (docs/radio.md:24,82), at 161 us per byte (8 bits at
 * 49.99 kBaud is 160.03 us), plus 100 us for the PA ramp from IDLE (IDLE to
 * TX 75 us, cc110l.txt Table 5-11). */
#define AIRTIME_US_PER_BYTE 161u
#define AIRTIME_FRAME_EXTRA 11u
#define AIRTIME_RAMP_US     100u
#define AIRTIME_FRAME_US(len) \
    ((uint32_t)(AIRTIME_FRAME_EXTRA + (len)) * AIRTIME_US_PER_BYTE + AIRTIME_RAMP_US)

struct airtime {
    uint32_t last_us; /* clock at the last call */
    uint32_t into_us; /* time spent in the current bucket */
    uint32_t used_us; /* sum of all buckets */
    uint32_t bucket[AIRTIME_BUCKETS];
    uint8_t  cur;
};

/* Zero-initialised is a valid, empty limiter. */

/* Let time pass. */
void airtime_advance(struct airtime *a, uint32_t now_us);

/* Charge cost_us if it fits in the budget: true and charged, or false and
 * nothing charged. */
bool airtime_charge(struct airtime *a, uint32_t now_us, uint32_t cost_us);

/* Airtime charged in the window, after advancing to now_us. */
uint32_t airtime_used_us(struct airtime *a, uint32_t now_us);

#endif
