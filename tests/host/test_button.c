/* Host tests for src/button.c: debounce and long press of the factory
 * reset and pair buttons (P6, #19). One poll is 10 ms (src/app.c). */
#include "button.h"

#include "test.h"

#define HOLD 1000u /* 10 s, the factory reset */

/* Feeds n polls of raw; returns the events OR-ed, *at the poll (from 1) of
 * the first hold, if any. */
static unsigned feed(struct button *b, bool raw, unsigned n, uint32_t hold, unsigned *at) {
    unsigned all = 0;
    for (unsigned i = 1; i <= n; i++) {
        unsigned ev = button_poll(b, raw, hold);
        if ((ev & BUTTON_HOLD) && at && !*at) {
            *at = i;
        }
        all |= ev;
    }
    return all;
}

static void test_debounce(void) {
    struct button b;
    button_init(&b, true);
    CHECK_EQ(feed(&b, false, 10, HOLD, NULL), 0);
    /* Glitches of 1 and 2 readings are not presses. */
    CHECK_EQ(feed(&b, true, 2, HOLD, NULL), 0);
    CHECK_EQ(feed(&b, false, 1, HOLD, NULL), 0);
    CHECK_EQ(feed(&b, true, 1, HOLD, NULL), 0);
    CHECK_EQ(feed(&b, false, 5, HOLD, NULL), 0);
    CHECK(!b.state);
    /* Three readings make a press, three a release. */
    CHECK_EQ(button_poll(&b, true, HOLD), 0);
    CHECK_EQ(button_poll(&b, true, HOLD), 0);
    CHECK_EQ(button_poll(&b, true, HOLD), BUTTON_PRESS);
    CHECK(b.state);
    CHECK_EQ(feed(&b, false, 2, HOLD, NULL), 0);
    CHECK_EQ(button_poll(&b, false, HOLD), BUTTON_RELEASE);
}

static void test_hold(void) {
    struct button b;
    button_init(&b, true);
    feed(&b, false, 3, HOLD, NULL);
    /* The hold fires once, HOLD polls after the debounced press. */
    unsigned at = 0;
    CHECK_EQ(feed(&b, true, 3u + HOLD + 500u, HOLD, &at), BUTTON_PRESS | BUTTON_HOLD);
    CHECK_EQ(at, 3u + HOLD - 1u); /* the press poll counts as the first */
    CHECK_EQ(feed(&b, false, 3, HOLD, NULL), BUTTON_RELEASE);
    /* Released one poll early: nothing. */
    CHECK_EQ(feed(&b, true, 3u + HOLD - 2u, HOLD, NULL), BUTTON_PRESS);
    CHECK_EQ(feed(&b, false, 3, HOLD, NULL), BUTTON_RELEASE);
    /* Bounces of 1-2 readings during the hold do not restart it. */
    at          = 0;
    unsigned ev = feed(&b, true, 400, HOLD, &at);
    ev |= feed(&b, false, 2, HOLD, &at);
    ev |= feed(&b, true, 300, HOLD, &at);
    ev |= feed(&b, false, 1, HOLD, &at);
    ev |= feed(&b, true, 400, HOLD, &at);
    CHECK_EQ(ev, BUTTON_PRESS | BUTTON_HOLD);
    /* A real release (3 readings) does restart it. */
    feed(&b, false, 3, HOLD, NULL);
    ev = feed(&b, true, 600, HOLD, NULL);
    ev |= feed(&b, false, 3, HOLD, NULL);
    ev |= feed(&b, true, 600, HOLD, NULL);
    CHECK_EQ(ev & BUTTON_HOLD, 0);
    /* hold_polls 0: never. */
    feed(&b, false, 3, 0, NULL);
    CHECK_EQ(feed(&b, true, 5000, 0, NULL) & BUTTON_HOLD, 0);
}

/* Held from power-on, or a pin stuck low: no factory reset however long,
 * until the button has been released. */
static void test_held_at_boot(void) {
    struct button b;
    button_init(&b, true);
    CHECK_EQ(feed(&b, true, 10u * HOLD, HOLD, NULL), BUTTON_PRESS);
    /* Released for two readings only: still not armed. */
    feed(&b, false, 2, HOLD, NULL);
    CHECK_EQ(feed(&b, true, 2u * HOLD, HOLD, NULL) & BUTTON_HOLD, 0);
    feed(&b, false, 3, HOLD, NULL);
    unsigned at = 0;
    CHECK_EQ(feed(&b, true, 2u * HOLD, HOLD, &at), BUTTON_PRESS | BUTTON_HOLD);
    CHECK_EQ(at, 3u + HOLD - 1u);
    /* The pair button keeps its behaviour: no release needed. */
    struct button p;
    button_init(&p, false);
    at = 0;
    CHECK_EQ(feed(&p, true, 200, 100, &at), BUTTON_PRESS | BUTTON_HOLD);
    CHECK_EQ(at, 3u + 100u - 1u);
}

/* Warning flicker: last 3 s of the 10 s hold, 500 ms off then 500 ms on. */
static void test_warn(void) {
    CHECK_EQ(button_warn_led(0, 10000), BUTTON_LED_NORMAL);
    CHECK_EQ(button_warn_led(6990, 10000), BUTTON_LED_NORMAL);
    CHECK_EQ(button_warn_led(7000, 10000), BUTTON_LED_OFF);
    CHECK_EQ(button_warn_led(7499, 10000), BUTTON_LED_OFF);
    CHECK_EQ(button_warn_led(7500, 10000), BUTTON_LED_ON);
    CHECK_EQ(button_warn_led(7999, 10000), BUTTON_LED_ON);
    CHECK_EQ(button_warn_led(8000, 10000), BUTTON_LED_OFF);
    CHECK_EQ(button_warn_led(9500, 10000), BUTTON_LED_ON);
    CHECK_EQ(button_warn_led(9990, 10000), BUTTON_LED_ON);
    CHECK_EQ(button_warn_led(10000, 10000), BUTTON_LED_NORMAL);
    CHECK_EQ(button_warn_led(20000, 10000), BUTTON_LED_NORMAL);
    CHECK_EQ(button_warn_led(500, 1000), BUTTON_LED_NORMAL); /* hold shorter than the warning */

    /* Through the button: normal until 7 s, flicker, normal at the hold,
     * and normal at once when the release starts (first released reading). */
    struct button b;
    button_init(&b, true);
    feed(&b, false, 3, HOLD, NULL);
    feed(&b, true, 3, HOLD, NULL);
    unsigned flick = 0, first = 0, last = 0;
    enum button_led prev = BUTTON_LED_NORMAL;
    for (unsigned i = 4; i <= HOLD + 50u; i++) {
        button_poll(&b, true, HOLD);
        enum button_led l = button_warn_poll(&b, 10, HOLD);
        if (l != BUTTON_LED_NORMAL) {
            first = first ? first : i;
            last  = i;
        }
        flick += l != prev && l != BUTTON_LED_NORMAL;
        prev = l;
    }
    CHECK_EQ(flick, 6); /* off, on, off, on, off, on */
    CHECK_EQ(first, 700u + 2u);
    CHECK_EQ(last, HOLD - 1u + 2u);
    CHECK_EQ(button_warn_poll(&b, 10, HOLD), BUTTON_LED_NORMAL);
    /* Released during the flicker: normal on the first released reading. */
    button_init(&b, true);
    feed(&b, false, 3, HOLD, NULL);
    feed(&b, true, 800, HOLD, NULL);
    CHECK(button_warn_poll(&b, 10, HOLD) != BUTTON_LED_NORMAL);
    button_poll(&b, false, HOLD);
    CHECK_EQ(button_warn_poll(&b, 10, HOLD), BUTTON_LED_NORMAL);
    feed(&b, false, 5, HOLD, NULL);
    CHECK_EQ(button_warn_poll(&b, 10, HOLD), BUTTON_LED_NORMAL);
    /* Held from power-on: no flicker, no erase. */
    button_init(&b, true);
    feed(&b, true, 800, HOLD, NULL);
    CHECK_EQ(button_warn_poll(&b, 10, HOLD), BUTTON_LED_NORMAL);
}

int main(void) {
    test_debounce();
    test_hold();
    test_held_at_boot();
    test_warn();
    return TEST_REPORT("test_button");
}
