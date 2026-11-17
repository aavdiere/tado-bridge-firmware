/* Host tests for src/pair.c (#19): the Beacon Request check and the
 * Enhanced Beacon against freedo's vectors, the d/pair message byte for
 * byte, and the window: only the named device, limits, expiry, d/pair
 * timing and the answer that closes the window. */
#include "pair.h"

#include "test.h"

/* freedo tado/beacon_test.go:62 (request) and :90 (beacon); joinerEUI
 * tado/pair_window_test.go:13, BridgeEUI tado/fixture_install_test.go:12.
 * Synthetic identities. */
static const uint8_t joiner[8] = {0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
static const uint8_t bridge[8] = {0x09, 0xa1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
static const uint8_t other[8]  = {0x0b, 0xb1, 0xce, 0xfa, 0x08, 0xc5, 0x1b, 0x00};

static const uint8_t request[27] = {
    0x0b, 0xe8, 0x7b, 0xff, 0xff, 0xff, 0xff, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b,
    0x00, 0x04, 0x58, 0x14, 0x63, 0xf9, 0x01, 0x47, 0xed, 0x74, 0xca, 0xf1, 0xbd,
};
static const uint8_t beacon[32] = {
    0x18, 0xec, 0x59, 0xcd, 0xab, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00, 0x09, 0xa1, 0xce,
    0x08, 0x5a, 0x08, 0x64, 0xb7, 0x71, 0x9c, 0x19, 0xe3, 0x42, 0x85, 0xc9, 0xcd, 0xed, 0x7e, 0xc2,
};

static const uint8_t net_key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static uint8_t       rnd[PAIR_RAND_LEN];

/* A Beacon Request from src with MAC command cmd, sealed under the pairing
 * key (freedo beacon_test.go beaconRequest). */
static void make_request(uint8_t out[27], const uint8_t src[8], uint8_t cmd) {
    static struct ccm_key k;
    ccm_set_key(&k, pair_static_key);
    uint8_t hdr[16] = {0x0b, 0xe8, 0x11, 0xff, 0xff, 0xff, 0xff};
    memcpy(hdr + 7, src, 8);
    hdr[15]         = 0x04;
    uint8_t body[5] = {16, 0, 0, 0, cmd};
    CHECK_EQ(mac_seal(&k, hdr, body, sizeof body, out, 27), 27);
}

static void test_key_and_vectors(void) {
    CHECK_MEM(pair_static_key, "tado pairing key", 16);
    pair_init();
    CHECK_EQ(pair_check_request(request, sizeof request, 0), PAIR_REQ_CLOSED);
    uint8_t out[PAIR_BEACON_LEN];
    CHECK_EQ(pair_build_beacon(out, 0x59, joiner, bridge, 13537), 0); /* closed */

    CHECK(pair_open(joiner, net_key, rnd, 1000));
    CHECK(pair_active(1000));
    CHECK(pair_key()->set);
    CHECK(pair_is_target(joiner));
    CHECK(!pair_is_target(other));
    CHECK_EQ(pair_check_request(request, sizeof request, 1500), PAIR_REQ_OK);
    CHECK_EQ(pair_build_beacon(out, 0x59, joiner, bridge, 13537), PAIR_BEACON_LEN);
    CHECK_MEM(out, beacon, sizeof beacon);
    CHECK_EQ(mac_classify(out, sizeof out), MAC_BEACON);
}

static void test_requests_refused(void) {
    uint8_t r[27];
    pair_init();
    CHECK(pair_open(other, net_key, rnd, 0));
    /* The vector comes from joiner: not the named device. */
    CHECK_EQ(pair_check_request(request, sizeof request, 10), PAIR_REQ_NOT_TARGET);

    CHECK(pair_open(joiner, net_key, rnd, 0));
    CHECK_EQ(pair_check_request(request, 26, 10), PAIR_REQ_MALFORMED);
    memcpy(r, request, 27);
    r[3] = 0xCD; /* PAN */
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_MALFORMED);
    memcpy(r, request, 27);
    r[15] = 0x05;
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_MALFORMED);
    memcpy(r, request, 27);
    r[0] = 0x49;
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_MALFORMED);
    for (unsigned i = 16; i < 27; i++) { /* sealed part or MIC damaged */
        memcpy(r, request, 27);
        r[i] ^= 0x10;
        CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_MIC);
    }
    make_request(r, joiner, 0x04);
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_NOT_BEACON_REQ);
    make_request(r, joiner, 0x07);
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_OK);
    /* Sealed under another key (a network key): refused. */
    static struct ccm_key nk;
    ccm_set_key(&nk, net_key);
    uint8_t body[5] = {16, 0, 0, 0, 7};
    CHECK_EQ(mac_seal(&nk, request, body, sizeof body, r, 27), 27);
    CHECK_EQ(pair_check_request(r, 27, 10), PAIR_REQ_MIC);

    /* At most PAIR_BEACONS_MAX answers per window. */
    unsigned ok = 0;
    for (unsigned i = 0; i < 2 * PAIR_BEACONS_MAX; i++) {
        ok += pair_check_request(request, 27, 20) == PAIR_REQ_OK;
    }
    CHECK_EQ(ok, PAIR_BEACONS_MAX - 1u); /* one counted above */
    CHECK_EQ(pair_check_request(request, 27, 20), PAIR_REQ_LIMIT);
}

static void test_window(void) {
    pair_init();
    /* Not a node address. */
    uint8_t bad[8];
    memcpy(bad, joiner, 8);
    bad[7] = 0x01;
    CHECK(!pair_open(bad, net_key, rnd, 0));
    CHECK(!pair_active(0));
    static const uint8_t zero_head[8] = {0, 0, 0, 0, 0, 0xc5, 0x1b, 0x00};
    CHECK(!pair_open(zero_head, net_key, rnd, 0));

    /* Expiry wipes the pairing key; the clock may wrap. */
    uint32_t t0 = 0xFFFFF000u;
    CHECK(pair_open(joiner, net_key, rnd, t0));
    CHECK(pair_active(t0 + PAIR_WINDOW_MS - 1u));
    CHECK(pair_is_target(joiner));
    CHECK(!pair_active(t0 + PAIR_WINDOW_MS));
    CHECK(!pair_key()->set);
    CHECK(!pair_is_target(joiner));
    CHECK_EQ(pair_check_request(request, 27, t0 + PAIR_WINDOW_MS + 1u), PAIR_REQ_CLOSED);

    /* Advertisement pairs: at most PAIR_RA_PAIRS_MAX, none when closed. */
    CHECK(!pair_take_ra_pair(0));
    CHECK(pair_open(joiner, net_key, rnd, 0));
    for (unsigned i = 0; i < PAIR_RA_PAIRS_MAX; i++) {
        CHECK(pair_take_ra_pair(1));
    }
    CHECK(!pair_take_ra_pair(2));
    pair_close();
    CHECK(!pair_key()->set);
}

/* freedo tado/build_resources_test.go:196-203. */
static void test_coap(void) {
    static const uint8_t session[8] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    uint8_t              want[PAIR_COAP_LEN] = {
        0x40, 0x02, 0x22, 0x22, 0xb1, 'd', 0x04, 'p', 'a', 'i', 'r', 0x11, 42, 0xe8, 0x06, 0xe7,
    };
    memcpy(want + 16, session, 8);
    want[24] = 0xff;
    want[25] = 0x12;
    want[26] = 0x10;
    memcpy(want + 27, net_key, 16);
    uint8_t out[64];
    CHECK_EQ(pair_build_coap(out, sizeof out, 0x2222, session, net_key), PAIR_COAP_LEN);
    CHECK_MEM(out, want, PAIR_COAP_LEN);
    CHECK_EQ(pair_build_coap(out, PAIR_COAP_LEN - 1u, 0x2222, session, net_key), 0);
}

static size_t ack(uint8_t *c, uint16_t mid, uint8_t code) {
    c[0] = 0x60; /* version 1, ACK, no token */
    c[1] = code;
    c[2] = (uint8_t)(mid >> 8);
    c[3] = (uint8_t)mid;
    return 4;
}

static void test_dpair(void) {
    uint8_t c[64];
    pair_init();
    for (unsigned i = 0; i < PAIR_RAND_LEN; i++) {
        rnd[i] = (uint8_t)(0x30 + i);
    }
    CHECK(pair_open(joiner, net_key, rnd, 0));
    CHECK_EQ(pair_dpair_due(c, sizeof c, 100000), 0); /* nothing heard from the target */

    /* A request answered but its beacon not sent: nothing. */
    CHECK_EQ(pair_check_request(request, 27, 1000), PAIR_REQ_OK);
    CHECK(!pair_engaged(1000));
    CHECK_EQ(pair_dpair_due(c, sizeof c, 1000 + PAIR_DPAIR_FALLBACK_MS), 0);
    /* After the beacon only: the fallback delay. */
    pair_beacon_sent(1000);
    CHECK(pair_engaged(1000));
    CHECK_EQ(pair_dpair_due(c, sizeof c, 1000 + PAIR_DPAIR_FALLBACK_MS - 1u), 0);
    /* Its Router Solicitation brings it forward. */
    pair_note_joiner(3000);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 3000 + PAIR_DPAIR_DELAY_MS - 1u), 0);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 3000 + PAIR_DPAIR_DELAY_MS), PAIR_COAP_LEN);
    uint16_t mid0 = (uint16_t)(c[2] << 8 | c[3]);
    CHECK(mid0 >= 0x4000 && mid0 < 0x6000);
    CHECK_MEM(c + 16, rnd + 2, 8);
    CHECK_MEM(c + 27, net_key, 16);
    /* One in flight at a time. */
    pair_note_joiner(7000);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 20000), 0);

    /* Every attempt failed: the next one waits for the target's next frame. */
    pair_dpair_done(false);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 30000), 0);
    pair_note_joiner(31000);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 31000 + PAIR_DPAIR_DELAY_MS), PAIR_COAP_LEN);
    CHECK_EQ((uint16_t)(c[2] << 8 | c[3]), (uint16_t)(mid0 + 1u));
    CHECK_MEM(c + 16, rnd + 2 + 8, 8); /* a new session value */
    pair_dpair_done(true);
    pair_note_joiner(40000);
    CHECK_EQ(pair_dpair_due(c, sizeof c, 40000 + PAIR_DPAIR_DELAY_MS), PAIR_COAP_LEN);
    uint32_t failed = pair_stats()->ex_failed;
    pair_dpair_done(false);
    /* The last d/pair of the window failed: the exchange has failed. */
    CHECK(!pair_engaged(43000));
    CHECK_EQ(pair_stats()->ex_failed, failed + 1u);
    pair_note_joiner(50000); /* a new exchange, with nothing left to send */
    CHECK(pair_engaged(50000));
    CHECK_EQ(pair_dpair_due(c, sizeof c, 60000), 0); /* PAIR_DPAIR_MAX sent */
    CHECK_EQ(pair_stats()->dpair_delivered, 1);

    /* Answers. */
    uint8_t m[8];
    CHECK_EQ(pair_reply(joiner, m, ack(m, 0x7123, 0x44)), PAIR_REPLY_NONE); /* the server's */
    CHECK_EQ(pair_reply(other, m, ack(m, mid0, 0x44)), PAIR_REPLY_NONE);
    m[0] = 0x40; /* a CON request from the target */
    CHECK_EQ(pair_reply(joiner, m, 4), PAIR_REPLY_NONE);
    (void)ack(m, mid0, 0x44);
    CHECK_EQ(pair_reply(joiner, m, 3), PAIR_REPLY_NONE); /* too short */
    /* Answers to the last exchange's d/pair leave this one alone. */
    CHECK_EQ(pair_reply(joiner, m, ack(m, (uint16_t)(mid0 + 2u), 0x83)), PAIR_REPLY_OTHER);
    CHECK(pair_engaged(60000));
    uint32_t paired = pair_stats()->paired;
    CHECK_EQ(pair_reply(joiner, m, ack(m, (uint16_t)(mid0 + 1u), 0x44)), PAIR_REPLY_PAIRED);
    CHECK(pair_engaged(60000));
    CHECK(pair_key()->set);
    CHECK_EQ(pair_stats()->paired, paired + 1u);
    /* A copy for the same exchange counts once, also after the window. */
    CHECK_EQ(pair_reply(joiner, m, ack(m, mid0, 0x44)), PAIR_REPLY_PAIRED);
    CHECK_EQ(pair_stats()->paired, paired + 1u);
    pair_close();
    CHECK(!pair_key()->set);
    CHECK_EQ(pair_reply(joiner, m, ack(m, mid0, 0x44)), PAIR_REPLY_PAIRED);
    CHECK_EQ(pair_stats()->paired, paired + 1u);

    /* A new window forgets the old MIDs. */
    CHECK(pair_open(joiner, net_key, rnd, 70000));
    CHECK_EQ(pair_reply(joiner, m, ack(m, mid0, 0x44)), PAIR_REPLY_NONE);
    /* Closing the window stops d/pair. */
    pair_note_joiner(70000);
    pair_close();
    CHECK_EQ(pair_dpair_due(c, sizeof c, 80000), 0);
}

/* The 10-minute window and its exchanges (owner spec 2026-11-17): a
 * pairing ends its exchange, not the window; the target may pair again;
 * an exchange without progress ends; a d/pair in flight holds it. */
static void test_exchange(void) {
    uint8_t c[64], m[8];
    CHECK_EQ(PAIR_WINDOW_MS, 600000u);
    pair_init();
    uint32_t t0 = 0xFFFC0000u; /* the millisecond clock wraps inside the window */
    CHECK(pair_open(joiner, net_key, rnd, t0));
    CHECK(!pair_engaged(t0));

    /* The window's first advertisement goes as a pair; later ones only
     * during an exchange. */
    CHECK(pair_ra_pair_wanted(t0 + 1000u));
    CHECK(pair_take_ra_pair(t0 + 1000u));
    CHECK(pair_ra_pairs_left(t0 + 1000u));
    CHECK(!pair_ra_pair_wanted(t0 + 1000u));

    /* Exchange 1: a beacon sent, then no progress. */
    uint32_t t  = t0 + 100000u;
    uint32_t ex = pair_stats()->exchanges, idle = pair_stats()->ex_idle;
    CHECK_EQ(pair_check_request(request, 27, t), PAIR_REQ_OK);
    pair_beacon_sent(t);
    CHECK(pair_engaged(t));
    CHECK_EQ(pair_stats()->exchanges, ex + 1u);
    CHECK(pair_ra_pair_wanted(t));
    pair_beacon_sent(t + 8000u); /* another beacon: the same exchange */
    CHECK_EQ(pair_stats()->exchanges, ex + 1u);
    t += 8000u;
    CHECK(pair_engaged(t - 5u)); /* a clock read a little earlier: not idle */
    pair_note_joiner(t - 5u);    /* nor does it move the progress back */
    CHECK(pair_engaged(t + PAIR_EXCHANGE_IDLE_MS - 1u));
    CHECK(!pair_engaged(t + PAIR_EXCHANGE_IDLE_MS));
    CHECK_EQ(pair_stats()->ex_idle, idle + 1u);
    CHECK(pair_active(t + PAIR_EXCHANGE_IDLE_MS));
    /* Its fallback d/pair (due 12 s after the beacon) was not taken: gone. */
    CHECK_EQ(pair_dpair_due(c, sizeof c, t + PAIR_EXCHANGE_IDLE_MS + 1u), 0);

    /* Exchange 2: the target's frame; its d/pair in flight holds the
     * exchange however long it takes; delivered, then the 2.04. */
    t = t0 + 140000u;
    pair_note_joiner(t);
    CHECK(pair_engaged(t));
    CHECK_EQ(pair_stats()->exchanges, ex + 2u);
    t += PAIR_DPAIR_DELAY_MS;
    CHECK_EQ(pair_dpair_due(c, sizeof c, t), PAIR_COAP_LEN);
    uint16_t mid0 = (uint16_t)(c[2] << 8 | c[3]);
    t += 5u * PAIR_EXCHANGE_IDLE_MS;
    CHECK(pair_engaged(t));
    pair_dpair_done(true);
    CHECK(pair_engaged(t + PAIR_EXCHANGE_IDLE_MS - 1u));
    t += PAIR_EXCHANGE_IDLE_MS - 1u;
    uint32_t paired = pair_stats()->paired;
    CHECK_EQ(pair_reply(joiner, m, ack(m, mid0, 0x44)), PAIR_REPLY_PAIRED);
    CHECK(!pair_engaged(t));
    CHECK(pair_active(t));
    CHECK(pair_key()->set);
    CHECK(pair_is_target(joiner));
    CHECK_EQ(pair_stats()->paired, paired + 1u);
    CHECK(!pair_ra_pair_wanted(t));

    /* Exchange 3: the same device pairs again in the window. */
    t = t0 + 330000u;
    CHECK_EQ(pair_check_request(request, 27, t), PAIR_REQ_OK);
    pair_beacon_sent(t);
    pair_note_joiner(t + 2000u);
    t += 2000u + PAIR_DPAIR_DELAY_MS;
    CHECK_EQ(pair_dpair_due(c, sizeof c, t), PAIR_COAP_LEN);
    CHECK_EQ((uint16_t)(c[2] << 8 | c[3]), (uint16_t)(mid0 + 1u));
    CHECK_MEM(c + 27, net_key, 16); /* the same network key */
    pair_dpair_done(true);
    /* A late copy of the 2.04 to exchange 2's d/pair: counted already,
     * and this exchange goes on. */
    CHECK_EQ(pair_reply(joiner, m, ack(m, mid0, 0x44)), PAIR_REPLY_PAIRED);
    CHECK(pair_engaged(t));
    CHECK_EQ(pair_stats()->paired, paired + 1u);
    CHECK_EQ(pair_reply(joiner, m, ack(m, (uint16_t)(mid0 + 1u), 0x44)), PAIR_REPLY_PAIRED);
    CHECK(!pair_engaged(t));
    CHECK_EQ(pair_stats()->paired, paired + 2u);

    /* Exchange 4: answered with an error: failed. */
    uint32_t failed = pair_stats()->ex_failed;
    t               = t0 + 360000u;
    pair_note_joiner(t);
    t += PAIR_DPAIR_DELAY_MS;
    CHECK_EQ(pair_dpair_due(c, sizeof c, t), PAIR_COAP_LEN);
    pair_dpair_done(true);
    CHECK_EQ(pair_reply(joiner, m, ack(m, (uint16_t)(mid0 + 2u), 0x80)), PAIR_REPLY_OTHER);
    CHECK(!pair_engaged(t));
    CHECK_EQ(pair_stats()->ex_failed, failed + 1u);
    CHECK_EQ(pair_stats()->paired, paired + 2u);

    /* Exchange 5: no d/pair left in this window; it ends without progress. */
    t = t0 + 380000u;
    pair_note_joiner(t);
    CHECK(pair_engaged(t));
    CHECK_EQ(pair_dpair_due(c, sizeof c, t + PAIR_DPAIR_DELAY_MS), 0);
    CHECK(!pair_engaged(t + PAIR_EXCHANGE_IDLE_MS));

    /* The window ends 10 minutes after it opened, across the clock's wrap,
     * and wipes the keys; nothing is answered after it. */
    CHECK(pair_active(t0 + PAIR_WINDOW_MS - 1u));
    CHECK(pair_key()->set);
    CHECK(!pair_active(t0 + PAIR_WINDOW_MS));
    CHECK(!pair_key()->set);
    CHECK_EQ(pair_check_request(request, 27, t0 + PAIR_WINDOW_MS), PAIR_REQ_CLOSED);
    pair_note_joiner(t0 + PAIR_WINDOW_MS);
    CHECK(!pair_engaged(t0 + PAIR_WINDOW_MS));

    /* Closed during an exchange: the exchange ends with it. */
    CHECK(pair_open(joiner, net_key, rnd, 0));
    pair_note_joiner(10);
    CHECK(pair_engaged(10));
    pair_close();
    CHECK(!pair_engaged(10));
}

int main(void) {
    test_key_and_vectors();
    test_requests_refused();
    test_window();
    test_coap();
    test_dpair();
    test_exchange();
    return TEST_REPORT("test_pair");
}
