/* Debounce and long press (src/button.h). */
#include "button.h"

#include <string.h>

void button_init(struct button *b, bool need_release) {
    memset(b, 0, sizeof *b);
    b->armed = !need_release;
}

unsigned button_poll(struct button *b, bool raw, uint32_t hold_polls) {
    unsigned ev = 0;
    if (raw) {
        b->up = 0;
    } else if (b->up < BUTTON_DEBOUNCE && ++b->up == BUTTON_DEBOUNCE) {
        b->armed = true;
    }
    if (raw == b->state) {
        b->pending = 0;
    } else if (++b->pending >= BUTTON_DEBOUNCE) {
        b->state   = raw;
        b->pending = 0;
        ev |= raw ? BUTTON_PRESS : BUTTON_RELEASE;
        b->held    = 0;
        b->hold_ok = raw && b->armed;
    }
    /* Not while a release is being debounced (pending: the raw reading
     * says released). */
    if (b->state && b->pending == 0 && b->hold_ok && hold_polls && b->held < hold_polls
        && ++b->held == hold_polls) {
        ev |= BUTTON_HOLD;
    }
    return ev;
}

enum button_led button_warn_led(uint32_t held_ms, uint32_t hold_ms) {
    if (hold_ms < BUTTON_WARN_MS || held_ms < hold_ms - BUTTON_WARN_MS || held_ms >= hold_ms) {
        return BUTTON_LED_NORMAL;
    }
    return ((held_ms - (hold_ms - BUTTON_WARN_MS)) / BUTTON_WARN_HALF_MS) % 2u == 0 ? BUTTON_LED_OFF
                                                                                    : BUTTON_LED_ON;
}

enum button_led button_warn_poll(const struct button *b, uint32_t poll_ms, uint32_t hold_polls) {
    if (!b->state || !b->hold_ok || b->pending != 0 || hold_polls == 0) {
        return BUTTON_LED_NORMAL;
    }
    return button_warn_led(b->held * poll_ms, hold_polls * poll_ms);
}
