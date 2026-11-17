/* Host tests for the rolling airtime limit, src/airtime.c (#17). The
 * property that matters: whatever the pattern of frames and pauses, the
 * airtime accepted in ANY 3600 s window never exceeds the budget. Checked
 * against a reference that keeps every accepted charge with a 64-bit
 * time. */
#include "airtime.h"

#include "test.h"

#include <stdint.h>
#include <stdlib.h>

#define HOUR_US 3600000000ull
#define MIN_US  60000000ull

static uint32_t rnd_state = 0x2468ace1u;

static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* Accepted charges, oldest first. */
#define LOG_MAX 200000u
static struct {
    uint64_t t;
    uint32_t cost;
} hist[LOG_MAX];
static unsigned hist_n;

/* Reference: airtime accepted in (now - span, now]. */
static uint64_t sum_since(uint64_t now, uint64_t span) {
    uint64_t s = 0;
    for (unsigned i = hist_n; i-- > 0;) {
        if (now - hist[i].t >= span) {
            break;
        }
        s += hist[i].cost;
    }
    return s;
}

static void test_frame_cost(void) {
    /* An Enhanced ACK (29 bytes), a wake-up frame (12), the longest frame. */
    CHECK_EQ(AIRTIME_FRAME_US(29), 40u * 161u + 100u);
    CHECK_EQ(AIRTIME_FRAME_US(12), 23u * 161u + 100u);
    CHECK_EQ(AIRTIME_FRAME_US(127), 138u * 161u + 100u);
    /* Never less than the airtime at the real 49.99 kBaud (160.03 us per
     * byte) plus IDLE to TX (75 us). */
    for (unsigned len = 1; len <= 127; len++) {
        CHECK(AIRTIME_FRAME_US(len) * 1000ull >= (11u + len) * 160030ull + 75000ull);
    }
}

static void test_budget(void) {
    struct airtime a = {0};
    uint32_t       t = 12345u;
    /* 30 s exactly, as 1000 charges of 30 ms. */
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(airtime_charge(&a, t, 30000u));
        t += 1000u;
    }
    CHECK_EQ(airtime_used_us(&a, t), AIRTIME_BUDGET_US);
    CHECK(!airtime_charge(&a, t, 1u));
    CHECK_EQ(airtime_used_us(&a, t), AIRTIME_BUDGET_US);
    /* A charge larger than the whole budget never fits. */
    struct airtime b = {0};
    CHECK(!airtime_charge(&b, 0, AIRTIME_BUDGET_US + 1u));
    CHECK(airtime_charge(&b, 0, AIRTIME_BUDGET_US));

    /* Still full 59 minutes later... */
    t += (uint32_t)(59u * MIN_US);
    CHECK(!airtime_charge(&a, t, 1u));
    /* ...and a little before the hour is over: the window covers at least
     * 60 minutes. */
    t += (uint32_t)(MIN_US - 2000000u);
    CHECK(!airtime_charge(&a, t, 1u));
    /* After 61 minutes everything has left the window. */
    t += (uint32_t)(2u * MIN_US);
    CHECK_EQ(airtime_used_us(&a, t), 0);
    CHECK(airtime_charge(&a, t, AIRTIME_BUDGET_US));
}

/* Minutes leave one at a time. */
static void test_rolling(void) {
    struct airtime a = {0};
    uint32_t       t = 0;
    for (unsigned m = 0; m < 30; m++) {
        CHECK(airtime_charge(&a, t, 1000000u)); /* 1 s per minute */
        t += (uint32_t)MIN_US;
    }
    CHECK_EQ(airtime_used_us(&a, t), 30000000u);
    CHECK(!airtime_charge(&a, t, 1u));
    /* The first second leaves 60-61 minutes after it was charged. */
    t += (uint32_t)(31u * MIN_US);
    CHECK_EQ(airtime_used_us(&a, t), 29000000u);
    CHECK(airtime_charge(&a, t, 1000000u));
    CHECK(!airtime_charge(&a, t, 1u));
}

/* A gap longer than the 32-bit clock's 71.6 minutes: the limiter sees the
 * gap modulo 2^32 and errs towards keeping old charges. */
static void test_wrap(void) {
    struct airtime a = {0};
    uint32_t       t = 1000u;
    CHECK(airtime_charge(&a, t, AIRTIME_BUDGET_US));
    /* 2^32 us + 1 minute later the limiter sees 1 minute: still full. */
    t += (uint32_t)MIN_US;
    CHECK(!airtime_charge(&a, t, 1u));
    /* Calls at least every 71 minutes keep it exact. */
    t += (uint32_t)(40u * MIN_US);
    CHECK(!airtime_charge(&a, t, 1u));
    t += (uint32_t)(25u * MIN_US);
    CHECK(airtime_charge(&a, t, 1u));
}

/* Random frames and pauses: bursts of wake trains, single ACKs, long
 * pauses, gaps beyond the 32-bit wrap. In every window of 3600 s the
 * accepted airtime stays within the budget, and the limiter refuses only
 * when the last 61 minutes are full. */
static void test_random(void) {
    struct airtime a   = {0};
    uint64_t       now = 777u;
    bool           wrap_in_window = false; /* a gap beyond 2^32 us in the last 61 minutes */
    uint64_t       last_wrap      = 0;
    unsigned       accepted = 0, refused = 0, refused_legit = 0;
    hist_n                  = 0;
    for (unsigned step = 0; step < 150000 && hist_n < LOG_MAX; step++) {
        /* Busy phases (frames back to back, 0-40 ms apart: the budget runs
         * out) alternate with quiet ones (pauses of up to 20 minutes, and
         * now and then one beyond the 32-bit wrap). */
        bool     busy = (step / 5000u) % 2u == 0;
        unsigned r    = rnd() % 16u;
        if (!busy && r == 0) {
            now += rnd() % (20u * (uint32_t)MIN_US);
        } else if (!busy && r == 1 && rnd() % 50u == 0) {
            now += (1ull << 32) + rnd() % (10u * (uint32_t)MIN_US);
            last_wrap = now;
        } else {
            now += rnd() % 40000u;
        }
        uint32_t cost = AIRTIME_FRAME_US(1u + rnd() % 127u);
        if (rnd() % 64u == 0) {
            cost = 1u + rnd() % 2000000u; /* any size, e.g. a train counted as one */
        }
        wrap_in_window = last_wrap && now - last_wrap < 61u * MIN_US;
        bool ok        = airtime_charge(&a, (uint32_t)now, cost);
        if (ok) {
            accepted++;
            hist[hist_n].t    = now;
            hist[hist_n].cost = cost;
            hist_n++;
            /* The hour that ends now, and also every hour that ends at an
             * earlier charge, since the newest charge is the only new one. */
            CHECK(sum_since(now, HOUR_US) <= AIRTIME_BUDGET_US);
        } else {
            refused++;
            /* Refused although the last 61 minutes have room: only after a
             * clock wrap (the conservative direction). */
            if (sum_since(now, 61u * MIN_US) + cost <= AIRTIME_BUDGET_US) {
                CHECK(wrap_in_window);
            } else {
                refused_legit++;
            }
        }
        CHECK(airtime_used_us(&a, (uint32_t)now) <= AIRTIME_BUDGET_US);
    }
    CHECK(accepted > 10000);
    CHECK(refused_legit > 1000);
    (void)refused;
}

int main(void) {
    test_frame_cost();
    test_budget();
    test_rolling();
    test_wrap();
    test_random();
    return TEST_REPORT("test_airtime");
}
