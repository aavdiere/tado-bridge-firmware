/* Host tests for src/freqoff.c (#11): the mean in Hz, the exact limit, the
 * window carried over quiet minutes, and the warning's lines. */
#include "freqoff.h"

#include "test.h"

#include <stdint.h>
#include <stdlib.h>

static void add_n(struct freqoff *s, int fe, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        freqoff_add(s, fe);
    }
}

static void test_mean(void) {
    /* One unit is 203125 / 128 = 1586.914 Hz (cc110l.txt:4429-4434). */
    CHECK_EQ(freqoff_mean_hz(0, 0), 0);
    CHECK_EQ(freqoff_mean_hz(5, 0), 0);
    CHECK_EQ(freqoff_mean_hz(1, 1), 1587);
    CHECK_EQ(freqoff_mean_hz(-1, 1), -1587);
    CHECK_EQ(freqoff_mean_hz(3, 2), 2380); /* 2380.37 */
    CHECK_EQ(freqoff_mean_hz(-3, 2), -2380);
    CHECK_EQ(freqoff_mean_hz(-6, 1), -9521); /* the bench's worst stock sender */
    CHECK_EQ(freqoff_mean_hz(127, 1), 201538);
    CHECK_EQ(freqoff_mean_hz(-128, 1), -203125);
    /* Half away from zero: 64 units are exactly 101562.5 Hz. */
    CHECK_EQ(freqoff_mean_hz(128, 2), 101563);
    CHECK_EQ(freqoff_mean_hz(-128, 2), -101563);
    /* The largest sums the cap allows do not overflow. */
    CHECK_EQ(freqoff_mean_hz(-128 * (int32_t)FREQOFF_MAX_FRAMES, FREQOFF_MAX_FRAMES), -203125);
}

static void test_limit(void) {
    /* 15000 Hz = 3072 / 325 units exactly (1920000 / 203125). */
    CHECK(!freqoff_over_limit(0, 0));
    CHECK(!freqoff_over_limit(3072, 325));
    CHECK(freqoff_over_limit(3073, 325));
    CHECK(!freqoff_over_limit(-3072, 325));
    CHECK(freqoff_over_limit(-3073, 325));
    CHECK(!freqoff_over_limit(9, 1));  /* 14282 Hz */
    CHECK(freqoff_over_limit(10, 1));  /* 15869 Hz */
    CHECK(!freqoff_over_limit(37, 4)); /* 14679 Hz */
    CHECK(freqoff_over_limit(38, 4));  /* 15076 Hz */
    CHECK(freqoff_over_limit(-38, 4));
    CHECK(!freqoff_over_limit(-128 * 4, 0));
    /* Against floating point, away from the boundary. */
    srand(11);
    for (int i = 0; i < 200000; i++) {
        uint32_t n   = 1u + (uint32_t)rand() % 2000u;
        int32_t  sum = (int32_t)(rand() % (257 * (int)n)) - 128 * (int32_t)n;
        double   hz  = (double)sum * 203125.0 / 128.0 / (double)n;
        double   a   = hz < 0 ? -hz : hz;
        if (a > 15000.001 || a < 14999.999) {
            CHECK_EQ(freqoff_over_limit(sum, n), a > 15000.0);
        }
        double frac = a - (double)(int64_t)a;
        if (frac < 0.499999 || frac > 0.500001) { /* a double may miss an exact half */
            double m = hz < 0 ? hz - 0.5 : hz + 0.5;
            CHECK_EQ(freqoff_mean_hz(sum, n), (int32_t)m);
        }
    }
}

static void test_exact_boundary_frames(void) {
    /* 325 frames averaging exactly 15000 Hz: within; one unit more: over. */
    struct freqoff        s;
    struct freqoff_report r;
    freqoff_init(&s);
    add_n(&s, 10, 147);
    add_n(&s, 9, 178);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WITHIN);
    CHECK_EQ(r.window_n, 325);
    CHECK_EQ(r.window_mean_hz, 15000);
    add_n(&s, 10, 148);
    add_n(&s, 9, 177);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WARN);
    CHECK_EQ(r.window_mean_hz, 15005);
}

static void test_report_cycle(void) {
    struct freqoff        s;
    struct freqoff_report r;
    freqoff_init(&s);

    /* An empty minute: nothing to say. */
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_PENDING);
    CHECK_EQ(r.minute_n, 0);
    CHECK_EQ(r.minute_mean_hz, 0);
    CHECK_EQ(r.minute_min, 0);
    CHECK_EQ(r.minute_max, 0);
    CHECK_EQ(r.window_n, 0);

    /* Three frames far off: too few, carried into the next minute. */
    add_n(&s, 10, 3);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_PENDING);
    CHECK_EQ(r.minute_n, 3);
    CHECK_EQ(r.minute_mean_hz, 15869);
    CHECK_EQ(r.minute_min, 10);
    CHECK_EQ(r.minute_max, 10);
    CHECK_EQ(r.window_n, 3);
    CHECK_EQ(r.window_mean_hz, 15869);

    /* A fourth one a minute later: the window has its four, and warns. The
     * minute line shows only this minute's frame. */
    freqoff_add(&s, 11);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WARN);
    CHECK_EQ(r.minute_n, 1);
    CHECK_EQ(r.minute_mean_hz, 17456);
    CHECK_EQ(r.window_n, 4);
    CHECK_EQ(r.window_mean_hz, 16266); /* 41 / 4 units */

    /* A quiet minute after a warning: no new verdict, no line. */
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_PENDING);
    CHECK_EQ(r.window_n, 0);

    /* Still off: the warning again, once in this report. */
    add_n(&s, -12, 6);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WARN);
    CHECK_EQ(r.window_n, 6);
    CHECK_EQ(r.window_mean_hz, -19043);

    /* Back: one line, then nothing while it stays within. */
    add_n(&s, 2, 4);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_BACK);
    CHECK_EQ(r.window_mean_hz, 3174);
    add_n(&s, 2, 4);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WITHIN);
    add_n(&s, 3, 9);
    freqoff_report(&s, &r);
    CHECK_EQ(r.verdict, FREQOFF_WITHIN);
    CHECK_EQ(r.minute_n, 9);
    CHECK_EQ(r.minute_mean_hz, 4761);

    /* Min and max of the minute, the mean of the mix. */
    freqoff_add(&s, -6);
    freqoff_add(&s, 0);
    freqoff_add(&s, 3);
    freqoff_add(&s, -1);
    freqoff_report(&s, &r);
    CHECK_EQ(r.minute_min, -6);
    CHECK_EQ(r.minute_max, 3);
    CHECK_EQ(r.minute_mean_hz, -1587);
    CHECK_EQ(r.verdict, FREQOFF_WITHIN);
}

static void test_bench_never_warns(void) {
    /* What the bench showed [V]: our network +1..+3 units (channel 16,
     * 2026-11-17 to 2026-11-20), stock senders 0..-6 (channel 26,
     * 2026-11-12). Every mix of them, at any count, stays within. */
    struct freqoff        s;
    struct freqoff_report r;
    freqoff_init(&s);
    srand(26);
    for (int minute = 0; minute < 5000; minute++) {
        unsigned n = (unsigned)rand() % 40u;
        for (unsigned i = 0; i < n; i++) {
            freqoff_add(&s, (rand() % 10) - 6); /* -6..+3 */
        }
        freqoff_report(&s, &r);
        CHECK(r.verdict == FREQOFF_PENDING || r.verdict == FREQOFF_WITHIN);
        CHECK_EQ(r.minute_n, n);
    }
}

static void test_guards(void) {
    struct freqoff        s;
    struct freqoff_report r;
    freqoff_init(&s);
    /* Not a FREQEST value (src/radio.c FE_NONE is 0x7FFF): not counted. */
    freqoff_add(&s, 0x7FFF);
    freqoff_add(&s, 128);
    freqoff_add(&s, -129);
    freqoff_add(&s, 127);
    freqoff_add(&s, -128);
    freqoff_report(&s, &r);
    CHECK_EQ(r.minute_n, 2);
    CHECK_EQ(r.minute_min, -128);
    CHECK_EQ(r.minute_max, 127);
    CHECK_EQ(r.verdict, FREQOFF_PENDING); /* two frames: carried */
    CHECK_EQ(r.window_n, 2);
    /* The cap: further frames are not counted. */
    freqoff_init(&s);
    add_n(&s, -128, FREQOFF_MAX_FRAMES + 5u);
    freqoff_report(&s, &r);
    CHECK_EQ(r.minute_n, FREQOFF_MAX_FRAMES);
    CHECK_EQ(r.window_n, FREQOFF_MAX_FRAMES);
    CHECK_EQ(r.window_mean_hz, -203125);
    CHECK_EQ(r.verdict, FREQOFF_WARN);
}

int main(void) {
    test_mean();
    test_limit();
    test_exact_boundary_frames();
    test_report_cycle();
    test_bench_never_warns();
    test_guards();
    return TEST_REPORT("test_freqoff");
}
