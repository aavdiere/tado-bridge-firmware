#ifndef PAIR_LED_H
#define PAIR_LED_H

/* The pairing LED's phase (#19, owner spec 2026-11-17, amended 2026-11-18),
 * a pure function of the pairing state and the time (tests/host/test_pair_led.c):
 *
 *   window open, waiting      slow flash, 500 ms on / 500 ms off
 *   exchange in progress      quick flash, 100 ms on / 100 ms off
 *   a pairing succeeded       fully on for 2 s, then back to the rules above
 *   a failed exchange         straight back to the slow flash (no state kept)
 *   no window                 off
 *
 * Success is a change of the paired counter (struct pair_stats). The 2 s
 * hold runs to its end whatever happens meanwhile: a window that ends or
 * closes during it, or a second exchange that starts during it (the quick
 * flash follows the hold) [I]. Flashes follow the millisecond clock the
 * caller passes (absolute phase, no state), so a late call only shows a
 * late phase. No logging, no lock, no wait. */

#include <stdbool.h>
#include <stdint.h>

#define PAIR_LED_SLOW_HALF_MS  500u
#define PAIR_LED_QUICK_HALF_MS 100u
#define PAIR_LED_SUCCESS_MS    2000u

struct pair_led {
    uint32_t paired_seen; /* the paired counter at the last call */
    uint32_t hold_until;  /* ms clock, valid while holding */
    bool     holding;
};

/* paired: the current paired counter (no hold for pairings before this). */
void pair_led_init(struct pair_led *s, uint32_t paired);

/* The level (true = lit) at now_ms. open: the window is open; engaged: an
 * exchange is in progress (only meaningful with open); paired: the paired
 * counter. Call it with the same clock each time (wrap-safe). */
bool pair_led_level(struct pair_led *s, bool open, bool engaged, uint32_t paired, uint32_t now_ms);

#endif
