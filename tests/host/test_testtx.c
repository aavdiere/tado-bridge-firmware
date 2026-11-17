/* TEMPORARY (#11): host tests for the test-frame sender, src/testtx.c.
 * Revert with it after the test. The frame must pass the transmit gate
 * unchanged as a unicast Data frame of ours to an address that is neither
 * ours nor a tado address; the schedule must never start unarmed, never
 * send two slots closer than 5 s, and stop by itself. */
#include "testtx.h"

#include "mac.h"
#include "txgate.h"

#include "test.h"

static const uint8_t own[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC5, 0x1B, 0x00};

static void test_frame(void) {
    struct txgate_radio ch10 = {0x21, 0x32, 0x76, TXGATE_TEST_CHANNEL, 0x22, 0xF8, 0x00};
    struct txgate_radio ch26 = ch10;
    ch26.channr              = TXGATE_PROTECTED_CHANNEL;
    uint8_t f[TESTTX_LEN];
    for (unsigned seq = 0; seq < 256; seq++) {
        testtx_build(f, own, (uint8_t)seq);
        CHECK_EQ(f[0], 0x69);
        CHECK_EQ(f[1], 0xEC);
        CHECK_EQ(f[2], seq);
        CHECK_EQ(mac_classify(f, sizeof f), MAC_DATA);
        CHECK_MEM(f + 3, testtx_dst, 8);
        CHECK_MEM(f + 11, own, 5);
        CHECK_EQ(txgate_check_frame(&ch10, own, f, sizeof f, NULL, 0), TXGATE_OK);
        CHECK_EQ(txgate_check_frame(&ch26, own, f, sizeof f, NULL, 0), TXGATE_PROTECTED);
    }
    CHECK(TESTTX_LEN <= 64u);
    /* The destination: not ours, not a tado address (wire tail c5 1b 00),
     * not broadcast, locally administered (EUI-64 first byte bit 1). */
    CHECK(memcmp(testtx_dst, own, 8) != 0);
    CHECK(memcmp(testtx_dst + 5, mac_eui_suffix, 3) != 0);
    CHECK(!(testtx_dst[0] == 0xFF && testtx_dst[1] == 0xFF));
    CHECK((testtx_dst[7] & 0x02u) != 0);
    /* Another sender's address as source is refused: the gate is what
     * stands between this frame and the air. */
    uint8_t other[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
    testtx_build(f, other, 0);
    CHECK_EQ(txgate_check_frame(&ch10, own, f, sizeof f, NULL, 0), TXGATE_NOT_OUR_SRC);
}

static void test_never_unarmed(void) {
    struct testtx t = {0};
    for (uint64_t now = 0; now < 1000000000ull; now += 100000u) {
        CHECK_EQ(testtx_poll(&t, now, false), TESTTX_IDLE);
    }
    CHECK_EQ(t.slots, 0);
    CHECK_EQ(testtx_arm, 0u); /* .bss */
}

/* Polled every step_us with every slot sent: at most 60 slots, at least 5 s
 * apart, none at or after 5 minutes from the start, and then never again,
 * armed or not. */
static void run(uint64_t t0, uint64_t step_us, bool send) {
    struct testtx t = {0};
    CHECK_EQ(testtx_poll(&t, t0, true), TESTTX_START);
    uint64_t         last = 0, now = t0;
    unsigned         slots     = 0;
    bool             have_last = false, stopped = false;
    enum testtx_step stop = TESTTX_IDLE;
    for (; now < t0 + 2u * TESTTX_MAX_RUN_US; now += step_us) {
        enum testtx_step s = testtx_poll(&t, now, true);
        if (s == TESTTX_SLOT) {
            CHECK(!stopped);
            CHECK(now - t0 < TESTTX_MAX_RUN_US);
            if (have_last) {
                CHECK(now - last >= TESTTX_PERIOD_US);
            }
            last      = now;
            have_last = true;
            slots++;
            if (send) {
                testtx_sent(&t);
            }
        } else if (s != TESTTX_IDLE) {
            CHECK(!stopped);
            stopped = true;
            stop    = s;
        }
    }
    CHECK(stopped);
    CHECK(slots <= TESTTX_MAX_FRAMES);
    CHECK(t.sent <= TESTTX_MAX_FRAMES);
    CHECK(stop == TESTTX_STOP_COUNT || stop == TESTTX_STOP_TIME);
    CHECK_EQ(testtx_poll(&t, now, true), TESTTX_IDLE);
    CHECK_EQ(testtx_poll(&t, now + 10u * TESTTX_PERIOD_US, true), TESTTX_IDLE);
}

static void test_disarm(void) {
    struct testtx t = {0};
    CHECK_EQ(testtx_poll(&t, 0, true), TESTTX_START);
    CHECK_EQ(testtx_poll(&t, 0, true), TESTTX_SLOT);
    CHECK_EQ(testtx_poll(&t, TESTTX_PERIOD_US, false), TESTTX_STOP_DISARM);
    CHECK_EQ(testtx_poll(&t, 2u * TESTTX_PERIOD_US, true), TESTTX_IDLE);
    CHECK_EQ(t.slots, 1);
}

int main(void) {
    test_frame();
    test_never_unarmed();
    run(0, 1000u, true);
    run(123456789u, 100000u, true);
    run(0, 4999999u, true);
    run(7, 1000u, false); /* every slot busy or refused: the time limit ends it */
    test_disarm();
    return TEST_REPORT("test_testtx");
}
