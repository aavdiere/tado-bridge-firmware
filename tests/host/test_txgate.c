/* Host tests for the transmit gate, src/txgate.c (#17): the protected
 * channel, the test channel, and the frame rules.
 *
 * The test channel is the build option RADIO_NET_CHANNEL (stage P7.1,
 * src/txgate.h). tests/host/Makefile builds this program twice:
 * test_txgate for the default channel 16, test_txgate_ch26 for channel 26
 * with its opt-in RADIO_ALLOW_CH26. tests/host/test_channel_build.sh checks
 * that channel 26 without the opt-in does not build. */
#include "txgate.h"

#include "mac.h"

#include "test.h"
#include "vectors_freedo.h"

#include <stdlib.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))

#define PROTECTED_HZ 868323668ll /* channel 26, docs/radio-runtime.md "Channel" */
#define CH16_HZ      866324157ll /* channel 16, the test channel since 2026-11-17 */
#define OLD_TEST_HZ  865124450ll /* channel 10, the test channel until 2026-11-17 */
#if RADIO_NET_CHANNEL == 16 && !TXGATE_NET_ON_PROTECTED
#define TEST_HZ CH16_HZ
/* A channel the gate refuses as protected in this build. */
#define PROT_CH 26u
#elif RADIO_NET_CHANNEL == 26 && TXGATE_NET_ON_PROTECTED
#define TEST_HZ PROTECTED_HZ
#define PROT_CH 25u /* channel 26 itself is the network's; its neighbours stay refused */
#else
#error "test_txgate covers the channel 16 and channel 26 builds"
#endif

/* The register values of src/radio.c's table, on channel ch. */
static struct txgate_radio plan(unsigned ch) {
    struct txgate_radio r = {0x21, 0x32, 0x76, (uint8_t)ch, 0x22, 0xF8, 0x00};
    return r;
}

static long long dist(const struct txgate_radio *r, long long hz) {
    long long d = txgate_carrier_hz(r) - hz;
    return d < 0 ? -d : d;
}

static void test_carrier(void) {
    struct txgate_radio r = plan(0);
    CHECK(dist(&r, 863124939ll) <= 1); /* docs/test-bench.md: channel 0 = 863.124939 MHz */
    r = plan(26);
    CHECK(dist(&r, PROTECTED_HZ) <= 1);
    r = plan(10);
    CHECK(dist(&r, OLD_TEST_HZ) <= 1);
    r = plan(16);
    CHECK(dist(&r, CH16_HZ) <= 1);
    /* 10 channels, about 2.0 MHz, from the protected channel. */
    CHECK(CH16_HZ - PROTECTED_HZ < -1999000ll && CH16_HZ - PROTECTED_HZ > -2000000ll);
    r = plan(TXGATE_TEST_CHANNEL);
    CHECK(dist(&r, TEST_HZ) <= 1);
    r.fsctrl0 = 0x01; /* fXOSC / 2^14 = 1586.9 Hz per unit */
    CHECK(dist(&r, TEST_HZ + 1587) <= 1);
    r.fsctrl0 = 0xFF;
    CHECK(dist(&r, TEST_HZ - 1587) <= 1);
}

static void test_channel(void) {
    /* On the plan: only the test channel passes. */
    for (unsigned ch = 0; ch < 256; ch++) {
        struct txgate_radio r = plan(ch);
        enum txgate_verdict v = txgate_check_channel(&r);
        if (ch == TXGATE_TEST_CHANNEL) {
            CHECK_EQ(v, TXGATE_OK);
        } else if (ch >= 22 && ch <= 30) {
            CHECK_EQ(v, TXGATE_PROTECTED);
        } else if (ch == 21 || ch == 31) {
            /* Exactly 5 channels away: allowed by distance, refused by rule 2. */
            CHECK_EQ(v, TXGATE_NOT_TEST_CHANNEL);
        } else if (ch > 49) {
            CHECK_EQ(v, TXGATE_BAD_CHANNEL);
        } else {
            CHECK_EQ(v, TXGATE_NOT_TEST_CHANNEL);
        }
    }
    CHECK_EQ(TXGATE_PROTECTED_CHANNEL, 26);
#if TXGATE_NET_ON_PROTECTED
    CHECK_EQ(TXGATE_TEST_CHANNEL, 26);
    /* The default test channel is refused like any other in this build. */
    struct txgate_radio r = plan(16);
    CHECK_EQ(txgate_check_channel(&r), TXGATE_NOT_TEST_CHANNEL);
#else
    CHECK_EQ(TXGATE_TEST_CHANNEL, 16);
    CHECK_EQ(TXGATE_NET_ON_PROTECTED, 0);
    struct txgate_radio r;
#endif
    /* The old test channel is refused like any other. */
    r = plan(10);
    CHECK_EQ(txgate_check_channel(&r), TXGATE_NOT_TEST_CHANNEL);

    /* "Channel 16" moved onto the protected frequency by other registers:
     * refused as protected, or in the channel-26 build as off the plan
     * (only CHANNR 26 on the plan's base and spacing gives channel 26). */
    enum txgate_verdict moved = TXGATE_NET_ON_PROTECTED ? TXGATE_OFF_PLAN : TXGATE_PROTECTED;
    r      = plan(16);
    long f = 0x213276 + 10 * 504; /* base raised by 10 channels */
    r.freq2               = (uint8_t)(f >> 16);
    r.freq1               = (uint8_t)(f >> 8);
    r.freq0               = (uint8_t)f;
    CHECK(dist(&r, PROTECTED_HZ) <= 1);
    CHECK_EQ(txgate_check_channel(&r), moved);

    r         = plan(13);
    r.mdmcfg1 = 0x23; /* spacing doubled: channel 13 = old channel 26 */
    CHECK(dist(&r, PROTECTED_HZ) <= 1);
    CHECK_EQ(txgate_check_channel(&r), moved);

    r         = plan(TXGATE_TEST_CHANNEL);
    r.mdmcfg0 = 0xF9; /* a slightly different spacing: off the plan */
    CHECK_EQ(txgate_check_channel(&r), TXGATE_OFF_PLAN);
    r       = plan(TXGATE_TEST_CHANNEL);
    r.freq0 = 0x77;
    CHECK_EQ(txgate_check_channel(&r), TXGATE_OFF_PLAN);

    /* Offset trim: small values pass, large ones do not. */
    for (int t = -128; t < 128; t++) {
        r         = plan(TXGATE_TEST_CHANNEL);
        r.fsctrl0 = (uint8_t)t;
        CHECK_EQ(txgate_check_channel(&r),
                 t >= -TXGATE_MAX_TRIM && t <= TXGATE_MAX_TRIM ? TXGATE_OK : TXGATE_OFF_PLAN);
    }
    /* The trim cannot pull a neighbour of the protected channel through
     * (nor, in the channel-26 build, channel 26 further than the trim
     * limit). */
    for (unsigned ch = 20; ch <= 32; ch++) {
        for (int t = -128; t < 128; t++) {
            r         = plan(ch);
            r.fsctrl0 = (uint8_t)t;
            bool own  = ch == TXGATE_TEST_CHANNEL && t >= -TXGATE_MAX_TRIM && t <= TXGATE_MAX_TRIM;
            CHECK_EQ(txgate_check_channel(&r) == TXGATE_OK, own);
        }
    }

    /* Whatever the registers hold: a pass means the test channel's
     * frequency and at least 999 kHz from the protected one (in the
     * channel-26 build: channel 26's own frequency). */
    srand(17);
    unsigned passed = 0;
    for (unsigned i = 0; i < 2000000; i++) {
        struct txgate_radio x;
        if (i & 1) { /* near the plan, so that some pass */
            x = plan(rand() % 52);
            if (rand() % 4 == 0) {
                x.freq0 = (uint8_t)rand();
            }
            if (rand() % 4 == 0) {
                x.mdmcfg1 = (uint8_t)rand();
            }
            if (rand() % 4 == 0) {
                x.mdmcfg0 = (uint8_t)rand();
            }
            if (rand() % 2 == 0) {
                x.fsctrl0 = (uint8_t)rand();
            }
        } else {
            uint8_t b[7];
            for (unsigned k = 0; k < 7; k++) {
                b[k] = (uint8_t)rand();
            }
            x = (struct txgate_radio){b[0], b[1], b[2], b[3], b[4], b[5], b[6]};
        }
        if (txgate_check_channel(&x) == TXGATE_OK) {
            passed++;
            if ((!TXGATE_NET_ON_PROTECTED && dist(&x, PROTECTED_HZ) < 999000)
                || dist(&x, TEST_HZ) > 51000
                || x.channr != TXGATE_TEST_CHANNEL) {
                CHECK(!"a register set passed the gate off the test channel");
                break;
            }
        }
    }
    CHECK(passed > 1000);
}

/* A set of our own frames: wire addresses from the freedo vectors. */
static struct ccm_key key;
static uint8_t        own[8], peer[8];

static void test_ack_rule(void) {
    struct txgate_radio r = plan(TXGATE_TEST_CHANNEL);
    for (size_t i = 0; i < N(v_ack); i++) {
        const uint8_t *me  = v_ack[i].own.p;
        const uint8_t *hdr = v_ack[i].data_hdr.p; /* uplink addressed to me */
        const uint8_t *ack = v_ack[i].ack.p;

        CHECK_EQ(txgate_check_frame(&r, me, ack, MAC_ACK_LEN, hdr, 0), TXGATE_OK);
        /* Without the frame it answers. */
        CHECK_EQ(txgate_check_frame(&r, me, ack, MAC_ACK_LEN, NULL, 0), TXGATE_NO_ANSWER);

        /* The answered frame is addressed to someone else: one bit of the
         * destination differs, in any of its 8 bytes. */
        for (unsigned b = 0; b < 8; b++) {
            uint8_t h[MAC_HDR_LEN];
            memcpy(h, hdr, sizeof h);
            h[3 + b] ^= 0x01;
            CHECK_EQ(txgate_check_frame(&r, me, ack, MAC_ACK_LEN, h, 0), TXGATE_NOT_FOR_US);
            /* Even with an ACK built for that other destination as source. */
            uint8_t other[8], fake[MAC_ACK_LEN];
            memcpy(other, h + 3, 8);
            mac_build_enh_ack(fake, h[2], ack + 3, other, 0);
            CHECK(txgate_check_frame(&r, me, fake, MAC_ACK_LEN, h, 0) != TXGATE_OK);
        }
        /* The answered frame is not a unicast Data frame. */
        static const uint8_t fcfs[][2] = {{0x49, 0xE8}, {0x59, 0xE8}, {0x42, 0xEE},
                                          {0x25, 0xCD}, {0x0B, 0xE8}, {0x18, 0xEC}};
        for (size_t k = 0; k < N(fcfs); k++) {
            uint8_t h[MAC_HDR_LEN];
            memcpy(h, hdr, sizeof h);
            h[0] = fcfs[k][0];
            h[1] = fcfs[k][1];
            CHECK_EQ(txgate_check_frame(&r, me, ack, MAC_ACK_LEN, h, 0), TXGATE_NOT_FOR_US);
        }
        /* The ACK does not match the answered frame: any byte but the phase. */
        for (unsigned b = 2; b < MAC_ACK_LEN; b++) {
            uint8_t bad[MAC_ACK_LEN];
            memcpy(bad, ack, sizeof bad);
            bad[b] ^= 0x20;
            CHECK_EQ(txgate_check_frame(&r, me, bad, MAC_ACK_LEN, hdr, 0), TXGATE_WRONG_ANSWER);
        }
        /* Off the test channel, and on the protected channel. */
        struct txgate_radio p = plan(PROT_CH), q = plan(11), o = plan(10);
        CHECK_EQ(txgate_check_frame(&p, me, ack, MAC_ACK_LEN, hdr, 0), TXGATE_PROTECTED);
        CHECK_EQ(txgate_check_frame(&q, me, ack, MAC_ACK_LEN, hdr, 0), TXGATE_NOT_TEST_CHANNEL);
        CHECK_EQ(txgate_check_frame(&o, me, ack, MAC_ACK_LEN, hdr, 0), TXGATE_NOT_TEST_CHANNEL);
    }
}

static void test_frame_rules(void) {
    struct txgate_radio r = plan(TXGATE_TEST_CHANNEL), prot = plan(PROT_CH);
    uint8_t             f[MAC_FRAME_MAX], dgram[40] = {0x7E, 0x33, 0xF0};
    static const uint8_t zero[8];

    /* Wake-up. */
    static const uint8_t short_addr[2] = {0x12, 0x34};
    mac_build_wakeup(f, 7, short_addr, 100);
    CHECK_EQ(txgate_check_frame(&r, own, f, MAC_WAKE_LEN, NULL, 0), TXGATE_OK);
    CHECK_EQ(txgate_check_frame(&prot, own, f, MAC_WAKE_LEN, NULL, 0), TXGATE_PROTECTED);
    CHECK_EQ(txgate_check_frame(&r, zero, f, MAC_WAKE_LEN, NULL, 0), TXGATE_NO_IDENTITY);
    CHECK_EQ(txgate_check_frame(&r, own, f, MAC_WAKE_LEN - 1, NULL, 0), TXGATE_BAD_LENGTH);
    f[2] = 0xFF; /* another PAN */
    CHECK_EQ(txgate_check_frame(&r, own, f, MAC_WAKE_LEN, NULL, 0), TXGATE_MALFORMED);

    /* Unicast Data from us. */
    size_t n = mac_build_data(&key, f, sizeof f, false, 1, peer, own, 5, dgram, sizeof dgram);
    CHECK(n > 0);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, 0), TXGATE_OK);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, TXGATE_PAIRING), TXGATE_OK);
    CHECK_EQ(txgate_check_frame(&prot, own, f, n, NULL, 0), TXGATE_PROTECTED);
    CHECK_EQ(txgate_check_frame(&r, own, f, MAC_FRAME_MAX + 1, NULL, 0), TXGATE_BAD_LENGTH);
    CHECK_EQ(txgate_check_frame(&r, own, f, 20, NULL, 0), TXGATE_BAD_LENGTH);
    /* Somebody else's source address (for example a replayed frame). */
    for (unsigned b = 0; b < 5; b++) {
        uint8_t bad[MAC_FRAME_MAX];
        memcpy(bad, f, n);
        bad[11 + b] ^= 0x40;
        CHECK_EQ(txgate_check_frame(&r, own, bad, n, NULL, 0), TXGATE_NOT_OUR_SRC);
    }
    n = mac_build_data(&key, f, sizeof f, true, 1, own, own, 5, dgram, sizeof dgram);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, 0), TXGATE_MALFORMED);
    /* A device's uplink, as received, cannot be sent by us. */
    n = mac_build_data(&key, f, sizeof f, false, 1, own, peer, 5, dgram, sizeof dgram);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, 0), TXGATE_NOT_OUR_SRC);

    /* Broadcast from us. */
    n = mac_build_broadcast(&key, f, sizeof f, false, 3, own, 9, dgram, 20);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, 0), TXGATE_OK);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, TXGATE_PAIRING), TXGATE_OK);
    n = mac_build_broadcast(&key, f, sizeof f, true, 3, peer, 9, dgram, 20);
    CHECK_EQ(txgate_check_frame(&r, own, f, n, NULL, 0), TXGATE_NOT_OUR_SRC);

    /* Classes a coordinator never sends. */
    uint8_t req[27] = {0x0B, 0xE8, 1, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(req + 7, peer, 8);
    CHECK_EQ(txgate_check_frame(&r, own, req, sizeof req, NULL, 0), TXGATE_BAD_CLASS);
    uint8_t junk[20] = {0x61, 0x88};
    CHECK_EQ(txgate_check_frame(&r, own, junk, sizeof junk, NULL, 0), TXGATE_BAD_CLASS);

    /* Enhanced Beacon (#19): pairing flag, the test channel, and the request
     * it answers. freedo's request and beacon (tado/beacon_test.go:62, 90),
     * sealed as on air: the beacon's source is in the clear for its first
     * three bytes only. */
    static const uint8_t bridge[8] = {0x09, 0xa1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
    static const uint8_t ebr[27]   = {
        0x0b, 0xe8, 0x7b, 0xff, 0xff, 0xff, 0xff, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b,
        0x00, 0x04, 0x58, 0x14, 0x63, 0xf9, 0x01, 0x47, 0xed, 0x74, 0xca, 0xf1, 0xbd,
    };
    uint8_t eb[32] = {
        0x18, 0xec, 0x59, 0xcd, 0xab, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5,
        0x1b, 0x00, 0x09, 0xa1, 0xce, 0x08, 0x5a, 0x08, 0x64, 0xb7, 0x71,
        0x9c, 0x19, 0xe3, 0x42, 0x85, 0xc9, 0xcd, 0xed, 0x7e, 0xc2,
    };
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, TXGATE_PAIRING), TXGATE_OK);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, 0), TXGATE_NOT_TEST_CHANNEL);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, NULL, TXGATE_PAIRING),
             TXGATE_NO_ANSWER);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, eb, TXGATE_PAIRING),
             TXGATE_WRONG_ANSWER);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, 31, ebr, TXGATE_PAIRING), TXGATE_BAD_LENGTH);
    /* A request of ours: no beacon to ourselves. */
    uint8_t mine[27];
    memcpy(mine, ebr, sizeof mine);
    memcpy(mine + 7, bridge, 8);
    memcpy(eb + 5, bridge, 8);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, mine, TXGATE_PAIRING),
             TXGATE_WRONG_ANSWER);
    memcpy(eb + 5, ebr + 7, 8);
    eb[6] ^= 1; /* beacon for a device that did not ask */
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, TXGATE_PAIRING),
             TXGATE_WRONG_ANSWER);
    eb[6] ^= 1;
    eb[14] ^= 1; /* not from us */
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, TXGATE_PAIRING),
             TXGATE_NOT_OUR_SRC);
    eb[14] ^= 1;
    eb[3] ^= 1; /* another PAN */
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, TXGATE_PAIRING), TXGATE_MALFORMED);
    eb[3] ^= 1;
    uint8_t ebr2[27];
    memcpy(ebr2, ebr, sizeof ebr2);
    ebr2[4] = 0xCD; /* a request to one PAN only is not a joiner's */
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr2, TXGATE_PAIRING),
             TXGATE_WRONG_ANSWER);
    CHECK_EQ(txgate_check_frame(&r, bridge, eb, sizeof eb, ebr, TXGATE_PAIRING), TXGATE_OK);

    /* Pairing on any other channel of the plan is refused. */
    for (unsigned ch = 0; ch <= 49; ch++) {
        struct txgate_radio x = plan(ch);
        n = mac_build_broadcast(&key, f, sizeof f, true, 3, own, 9, dgram, 20);
        CHECK_EQ(txgate_check_frame(&x, own, f, n, NULL, TXGATE_PAIRING) == TXGATE_OK,
                 ch == TXGATE_TEST_CHANNEL);
    }

    for (int v = TXGATE_OK; v <= TXGATE_MALFORMED; v++) {
        CHECK(strcmp(txgate_reason((enum txgate_verdict)v), "?") != 0);
    }
}

int main(void) {
    ccm_set_key(&key, v_key.p);
    memcpy(own, v_ack[0].own.p, 8);
    memcpy(peer, v_ack[0].peer.p, 8);
    test_carrier();
    test_channel();
    test_ack_rule();
    test_frame_rules();
    return TEST_REPORT(TEST_NAME);
}
