#include "pair_led.h"

void pair_led_init(struct pair_led *s, uint32_t paired) {
    s->paired_seen = paired;
    s->hold_until  = 0;
    s->holding     = false;
}

bool pair_led_level(struct pair_led *s, bool open, bool engaged, uint32_t paired, uint32_t now_ms) {
    if (paired != s->paired_seen) {
        s->paired_seen = paired;
        s->holding     = true;
        s->hold_until  = now_ms + PAIR_LED_SUCCESS_MS; /* a second success restarts it */
    }
    if (s->holding) {
        if ((int32_t)(now_ms - s->hold_until) < 0) {
            return true;
        }
        s->holding = false;
    }
    if (!open) {
        return false;
    }
    if (engaged) {
        return now_ms % (2u * PAIR_LED_QUICK_HALF_MS) < PAIR_LED_QUICK_HALF_MS;
    }
    return now_ms % (2u * PAIR_LED_SLOW_HALF_MS) < PAIR_LED_SLOW_HALF_MS;
}
