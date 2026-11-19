/* Host tests for the update task's hand-over of one flash operation to the
 * radio task (#24 review M1, src/handoff.c), against a scripted runner and
 * a model clock.
 *
 * The runner (the radio task) claims the operation a given time after it
 * is posted, or never (its loop does not run), and finishes it a given
 * time after the claim, which wakes the poster. Checked:
 *   - an operation never claimed is withdrawn after the limit, to the
 *     millisecond, and never runs (a claim after that finds nothing);
 *   - a claimed operation is always waited for to its end, also past the
 *     limit, and its result returned;
 *   - every claim time around the limit, and random ones: an operation runs
 *     at most once, and the poster returns true only for one that ran and
 *     succeeded;
 *   - a second post while the slot is full is refused at once.
 * Before the fix the poster had no limit: the never-claimed case waited
 * for ever (here: the model clock's guard). */
#include "handoff.h"

#include "fwupd.h"

#include "test.h"

#include <stdlib.h>

static struct {
    uint64_t         now;       /* model clock, ms */
    uint64_t         posted_at; /* handoff_port_wake() */
    long             claim_in;  /* ms after the post; < 0: never */
    long             finish_in; /* ms after the claim; < 0: never */
    bool             result;    /* op->ok the runner sets */
    bool             tried;     /* the runner tried its claim */
    struct fwupd_op *op;        /* claimed, not finished */
    uint64_t         claimed_at;
    unsigned         ran, wakes, guard;
    bool             nested; /* try a second post from inside a sleep */
    bool             nested_rc;
} r;

uint32_t handoff_port_ms(void) {
    return (uint32_t)r.now;
}

void handoff_port_wake(void) {
    r.wakes++;
    r.posted_at = r.now;
}

/* One millisecond of the runner; true if it finished (the poster wakes). */
static bool runner_step(void) {
    if (!r.tried && r.claim_in >= 0 && r.now >= r.posted_at + (uint64_t)r.claim_in) {
        r.tried = true;
        r.op    = handoff_claim();
        if (r.op) {
            r.ran++;
            r.claimed_at = r.now;
            CHECK(handoff_pending()); /* claimed: still pending, the coordinator held */
        }
    }
    if (r.op && r.finish_in >= 0 && r.now >= r.claimed_at + (uint64_t)r.finish_in) {
        r.op->ok = r.result;
        handoff_finish(r.op);
        r.op = NULL;
        return true;
    }
    return false;
}

void handoff_port_sleep(uint32_t ms) {
    CHECK(ms >= 1u && ms <= 1000u);
    if (r.nested) {
        struct fwupd_op other = {.kind = FWUPD_OP_BOOT_LOAD};
        r.nested              = false;
        r.nested_rc           = handoff_run(&other, 0);
        CHECK(!other.done);
    }
    for (uint32_t i = 0; i < ms; i++) {
        if (runner_step()) {
            return;
        }
        r.now++;
        if (r.now > 3600u * 1000u) {
            printf("FAIL test_handoff: the poster waited over an hour\n");
            exit(1);
        }
    }
    (void)runner_step();
}

/* Post one operation with the runner scripted; the poster's result. */
static bool run(long claim_in, long finish_in, bool result, uint32_t wait_ms, uint64_t *took) {
    memset(&r, 0, sizeof r);
    r.now              = 1000;
    r.claim_in         = claim_in;
    r.finish_in        = finish_in;
    r.result           = result;
    struct fwupd_op o  = {.kind = FWUPD_OP_BOOT_LOAD, .ok = true, .done = true};
    uint64_t        t0 = r.now;
    bool            rc = handoff_run(&o, wait_ms);
    *took              = r.now - t0;
    CHECK_EQ(r.wakes, 1);
    CHECK(!handoff_pending()); /* the slot is empty after every return */
    CHECK(r.op == NULL);       /* nothing claimed is left running */
    if (r.ran) {
        CHECK(o.done);
        CHECK_EQ(rc, result);
    } else {
        CHECK(!o.done && !o.ok && !rc);
        CHECK(handoff_claim() == NULL); /* withdrawn: a late claim finds nothing */
    }
    return rc;
}

static void test_never_claimed(void) {
    uint64_t took = 0;
    CHECK(!run(-1, 0, true, 10000, &took));
    CHECK_EQ(r.ran, 0);
    CHECK_EQ(took, 10000);
    CHECK(!run(-1, 0, true, 1, &took));
    CHECK_EQ(took, 1);
}

static void test_claimed(void) {
    uint64_t took = 0;
    /* Taken at once, done 250 ms later (an erase). */
    CHECK(run(0, 250, true, 10000, &took));
    CHECK_EQ(r.ran, 1);
    CHECK_EQ(took, 250);
    /* Its failure comes back as false. */
    CHECK(!run(3000, 20, false, 10000, &took));
    CHECK_EQ(r.ran, 1);
    CHECK_EQ(took, 3020);
    /* Claimed just before the limit and done long after it: waited for. */
    CHECK(run(9999, 4000, true, 10000, &took));
    CHECK_EQ(r.ran, 1);
    CHECK_EQ(took, 13999);
    /* Done in no time. */
    CHECK(run(0, 0, true, 10000, &took));
    CHECK_EQ(took, 0);
}

static void test_around_the_limit(void) {
    unsigned ran = 0, withdrawn = 0;
    for (long c = 9980; c <= 10020; c++) {
        for (long f = 0; f <= 300; f += 100) {
            uint64_t took = 0;
            bool     rc   = run(c, f, true, 10000, &took);
            if (r.ran) {
                ran++;
                CHECK(rc);
                CHECK_EQ(took, (uint64_t)(c + f));
                CHECK(c <= 10000);
            } else {
                withdrawn++;
                CHECK(!rc);
                CHECK_EQ(took, 10000);
                CHECK(c >= 10000);
            }
        }
    }
    CHECK(ran > 0 && withdrawn > 0);
}

static void test_random(void) {
    uint32_t x = 12345;
    for (unsigned i = 0; i < 5000; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        long     c    = (x & 3u) == 0 ? -1 : (long)(x % 14000u);
        long     f    = (long)((x >> 8) % 600u);
        bool     ok   = (x >> 20) & 1u;
        uint64_t took = 0;
        bool     rc   = run(c, f, ok, 10000, &took);
        CHECK(r.ran <= 1u);
        CHECK(rc == (r.ran == 1u && ok));
        CHECK(took <= (r.ran ? (uint64_t)(c + f) : 10000u));
    }
}

static void test_second_post(void) {
    memset(&r, 0, sizeof r);
    r.now             = 0;
    r.claim_in        = 50;
    r.finish_in       = 10;
    r.result          = true;
    r.nested          = true;
    struct fwupd_op o = {.kind = FWUPD_OP_BOOT_LOAD};
    CHECK(handoff_run(&o, 10000));
    CHECK(!r.nested_rc); /* refused while the first one was posted */
    CHECK_EQ(r.ran, 1);
    CHECK_EQ(r.wakes, 1);
}

int main(void) {
    test_never_claimed();
    test_claimed();
    test_around_the_limit();
    test_random();
    test_second_post();
    return TEST_REPORT("test_handoff");
}
