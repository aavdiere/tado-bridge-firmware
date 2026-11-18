#ifndef BUTTON_H
#define BUTTON_H

/* Debounce and long-press detection for the factory reset (PB10) and pair
 * (PC15) buttons, polled by the housekeeping task (src/app.c).
 * Hardware independent (tests/host/test_button.c).
 *
 * A press or release counts once the raw reading has agreed for
 * BUTTON_DEBOUNCE polls in a row. A hold fires once per press, when the
 * debounced press has lasted hold_polls polls with a pressed reading (a
 * bounce shorter than BUTTON_DEBOUNCE does not end the press; its polls
 * do not count). With need_release, a hold fires
 * only for a press that began after the button had read released for
 * BUTTON_DEBOUNCE polls in a row: a button held, or a pin stuck low, from
 * power-on never fires. */

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_DEBOUNCE 3u /* 30 ms at the 10 ms poll */

struct button {
    bool     state;   /* debounced, true = pressed */
    bool     armed;   /* seen released (or not required) */
    bool     hold_ok; /* the current press may fire its hold */
    uint8_t  pending; /* consecutive readings that disagree with state */
    uint8_t  up;      /* consecutive released readings, up to BUTTON_DEBOUNCE */
    uint32_t held;    /* polls the current press has lasted */
};

/* Events of one poll, OR-ed. */
#define BUTTON_PRESS   1u
#define BUTTON_RELEASE 2u
#define BUTTON_HOLD    4u

void button_init(struct button *b, bool need_release);
/* One poll. raw: true = pressed. hold_polls: 0 for no hold. */
unsigned button_poll(struct button *b, bool raw, uint32_t hold_polls);

#endif
