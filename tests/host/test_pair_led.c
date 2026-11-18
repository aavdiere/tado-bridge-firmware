/* The pairing LED's phase (#19, owner spec 2026-11-17/18). */
#include "pair_led.h"
#include "test.h"

#include <stdint.h>

#define L(open, eng, pc, t) pair_led_level(&s, (open), (eng), (pc), (t))

static void test_flashes(void) {
    struct pair_led s;
    pair_led_init(&s, 0);
    CHECK(!L(false, false, 0, 0));
    CHECK(!L(false, false, 0, 250)); /* no window: off whatever the phase */
    CHECK(L(true, false, 0, 1000));  /* slow: 500 on, 500 off */
    CHECK(L(true, false, 0, 1499));
    CHECK(!L(true, false, 0, 1500));
    CHECK(!L(true, false, 0, 1999));
    CHECK(L(true, false, 0, 2000));
}

static void test_quick(void) {
    struct pair_led s;
    pair_led_init(&s, 0);
    for (uint32_t t = 3000; t < 3600; t++) {
        CHECK_EQ(L(true, true, 0, t), (t % 200u) < 100u);
    }
    /* A failed exchange: straight back to the slow flash, nothing held. */
    CHECK(L(true, false, 0, 4100)); /* 4100 % 1000 = 100: slow on */
    CHECK(!L(true, false, 0, 4600));
    /* The window ends mid-exchange: off. */
    CHECK(!L(false, true, 0, 4700));
}

static void test_success(void) {
    struct pair_led s;
    pair_led_init(&s, 3);
    /* Counter unchanged at the start: no hold for earlier pairings. */
    CHECK(!L(true, false, 3, 1500));
    /* Success at 10 500 (off phase of the slow flash): on for 2 s. */
    CHECK(L(true, false, 4, 10500));
    CHECK(L(true, false, 4, 12499));
    /* 12500: hold over; slow flash again (12500 % 1000 = 500: off phase). */
    CHECK(!L(true, false, 4, 12500));
    CHECK(L(true, false, 4, 13000));
    /* The exchange ending with success while engaged: hold, then flash. */
    CHECK(L(true, true, 4, 20000));
    CHECK(L(true, false, 5, 20150)); /* paired: on, though 150 is an off phase of quick */
    CHECK(L(true, false, 5, 22149));
    CHECK(!L(true, false, 5, 22500));
}

static void test_window_ends_in_hold(void) {
    struct pair_led s;
    pair_led_init(&s, 0);
    CHECK(L(true, false, 1, 600000 - 500)); /* success 0.5 s before the end */
    CHECK(L(false, false, 1, 600000));      /* window over: hold runs on */
    CHECK(L(false, false, 1, 600000 + 1499));
    CHECK(!L(false, false, 1, 600000 + 1500)); /* hold over, window gone: off */
    CHECK(!L(false, false, 1, 600000 + 3000));
}

static void test_second_exchange_in_hold(void) {
    struct pair_led s;
    pair_led_init(&s, 0);
    CHECK(L(true, false, 1, 1000));
    /* The target starts a second exchange 0.5 s in: still fully on. */
    for (uint32_t t = 1500; t < 3000; t += 10) {
        CHECK(L(true, true, 1, t));
    }
    /* Hold over at 3000, exchange still on: quick flash. */
    for (uint32_t t = 3000; t < 3400; t++) {
        CHECK_EQ(L(true, true, 1, t), (t % 200u) < 100u);
    }
    /* A second success in the hold restarts the 2 s. */
    pair_led_init(&s, 0);
    CHECK(L(true, false, 1, 1000));
    CHECK(L(true, true, 2, 2500));
    CHECK(L(true, false, 2, 4499));
    CHECK(!L(true, false, 2, 4500));
}

static void test_wrap(void) {
    struct pair_led s;
    pair_led_init(&s, 0);
    uint32_t t0 = UINT32_MAX - 999u; /* hold spans the 32-bit wrap */
    CHECK(L(true, false, 1, t0));
    CHECK(L(true, false, 1, t0 + 1999u));
    CHECK(!L(false, false, 1, t0 + 2000u));
}

int main(void) {
    test_flashes();
    test_quick();
    test_success();
    test_window_ends_in_hold();
    test_second_exchange_in_hold();
    test_wrap();
    return TEST_REPORT("test_pair_led");
}
