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

int main(void) {
    test_debounce();
    test_hold();
    test_held_at_boot();
    return TEST_REPORT("test_button");
}
