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
