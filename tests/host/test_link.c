/* Host tests for src/link.c (#17): nothing happens without a key, uplinks
 * reach the queue for the net task, downlinks reach the coordinator, the
 * uplink queue fills without overflowing, Router Advertisements. */
#include "link.h"

#include "csl.h"
#include "lowpan.h"
#include "pair.h"
#include "relay.h"
#include "txgate.h"

#include "test.h"
#include "vectors_freedo.h"

static struct ccm_key key;
static const uint8_t  own[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0xC5, 0x1B, 0x00};
static const uint8_t  dev[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};

/* --- CSL port: counts what would go on the air ------------------------------- */
static uint64_t now_us = 1000000;
static unsigned n_wake, n_data, n_bcast;
static uint8_t  last_bcast[MAC_FRAME_MAX];
static size_t   last_bcast_len;
static uint8_t  last_data[MAC_FRAME_MAX];
static size_t   last_data_len;
/* Every broadcast, for the pairing window's advertisement pairs. */
static uint8_t  bcast_log[64][MAC_FRAME_MAX];
static size_t   bcast_log_len[64];
static unsigned n_bcast_log;
/* The test channel, as the driver's registers would give it. */
static const struct txgate_radio radio = {0x21, 0x32, 0x76, TXGATE_TEST_CHANNEL, 0x22, 0xF8, 0};

uint64_t csl_port_now(void) {
    return now_us;
}
uint32_t csl_port_random(void) {
    return 1;
}
void csl_port_listen_begin(void) {
}
bool csl_port_listen_busy(void) {
    return false;
}
void csl_port_report(const struct csl_report *r) {
    link_on_report(r);
}
bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    /* The same rules the driver applies. */
    CHECK_EQ(txgate_check_frame(&radio, own, frame, len, NULL, flags), TXGATE_OK);
    now_us += 4000;
    switch (mac_classify(frame, len)) {
        case MAC_WAKEUP:
            n_wake++;
            break;
        case MAC_BROADCAST:
            n_bcast++;
            memcpy(last_bcast, frame, len);
            last_bcast_len = len;
            if (n_bcast_log < 64) {
                memcpy(bcast_log[n_bcast_log], frame, len);
                bcast_log_len[n_bcast_log++] = len;
            }
            break;
        default:
            n_data++;
            memcpy(last_data, frame, len);
            last_data_len = len;
            break;
    }
    return true;
}

static void run_ms(uint32_t *now_ms, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t++) {
        (*now_ms)++;
        now_us += 1000;
        link_poll(*now_ms, 12345);
        while (csl_step() == 0) {
        }
    }
}

static size_t uplink_frame(uint8_t *f, uint8_t seq, const uint8_t *to, const uint8_t *coap,
                           size_t n) {
    uint8_t dg[LOWPAN_FRAME_MAX];
    size_t  dl = lowpan_build_udp_ll(dg, sizeof dg, dev, to, 40001, 5683, coap, n);
    return mac_build_data(&key, f, MAC_FRAME_MAX, false, seq, to, dev, seq, dg, dl);
}

static size_t downlink_msg(uint8_t *msg, const uint8_t *coap, size_t n) {
    uint8_t ip[16];
    relay_ipv6_from_wire(dev, ip);
    relay_frame_header(msg, ip, 5683, n);
    msg[1] = (uint8_t)RELAY_DIR_DOWN;
    memcpy(msg + RELAY_HDR_LEN, coap, n);
    return RELAY_HDR_LEN + n;
}

/* --- Pairing (#19) ------------------------------------------------------------- */
static const uint8_t joiner[8]   = {0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
static const uint8_t stranger[8] = {0x0b, 0xb1, 0xce, 0xfa, 0x08, 0xc5, 0x1b, 0x00};
static struct ccm_key pk; /* the pairing key */

static size_t beacon_request(uint8_t *f, const uint8_t src[8], uint8_t seq) {
    uint8_t hdr[16] = {0x0b, 0xe8, seq, 0xff, 0xff, 0xff, 0xff};
    memcpy(hdr + 7, src, 8);
    hdr[15]         = 0x04;
    uint8_t body[5] = {seq, 0, 0, 0, 0x07};
    return mac_seal(&pk, hdr, body, sizeof body, f, MAC_FRAME_MAX);
}

static size_t frame_from(uint8_t *f, const uint8_t *src, const struct ccm_key *k, uint8_t seq,
                         const uint8_t *coap, size_t n) {
    uint8_t dg[LOWPAN_FRAME_MAX];
    size_t  dl = lowpan_build_udp_ll(dg, sizeof dg, src, own, 5683, 5683, coap, n);
    return mac_build_data(k, f, MAC_FRAME_MAX, false, seq, own, src, seq, dg, dl);
}

static unsigned count_pending_bcasts(unsigned from) {
    unsigned n = 0;
    for (unsigned i = from; i < n_bcast_log; i++) {
        n += bcast_log[i][0] == 0x59;
    }
    return n;
}

static void test_pairing(uint32_t *now_ms) {
    static uint8_t       f[MAC_FRAME_MAX], out[RELAY_MSG_MAX], body[MAC_FRAME_MAX];
    static const uint8_t rs[] = {0x7B, 0x3B, 58, 0x02, 133, 0, 0, 0, 0, 0, 0, 0};
    uint8_t              rnd[PAIR_RAND_LEN];
    for (unsigned i = 0; i < sizeof rnd; i++) {
        rnd[i] = (uint8_t)(i * 7u);
    }
    ccm_set_key(&pk, pair_static_key);

    /* No key, no window; never for ourselves. */
    link_init(own);
    CHECK(!link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    link_set_key(v_key.p, 2000, UINT32_MAX);
    CHECK(!link_pair_open(own, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    CHECK(!link_pair_active(*now_ms));

    /* No window: a Beacon Request gets nothing. */
    size_t         n = beacon_request(f, joiner, 0x7b), bl = 0;
    const uint8_t *ans;
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_REQ);
    CHECK(link_beacon(&bl, &ans) == NULL);

    /* The window opens; its first advertisement pair goes a second later:
     * 59 e8 under the network key, then 49 e8 under the pairing key. */
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    CHECK(link_pair_active(*now_ms));
    n_bcast_log = 0;
    run_ms(now_ms, 3000);
    CHECK_EQ(n_bcast_log, 2);
    size_t b0 = 0, b1 = 0;
    uint8_t body1[MAC_FRAME_MAX];
    CHECK_EQ(bcast_log[0][0], 0x59);
    CHECK_EQ(bcast_log[1][0], 0x49);
    CHECK_EQ(mac_open(&key, bcast_log[0], bcast_log_len[0], body, sizeof body, &b0), MAC_OPEN_OK);
    CHECK_EQ(mac_open(&pk, bcast_log[1], bcast_log_len[1], body1, sizeof body1, &b1), MAC_OPEN_OK);
    CHECK_EQ(b0, 2 + LOWPAN_RA_LEN);
    CHECK_EQ(b1, b0);
    CHECK_MEM(body + 2, body1 + 2, LOWPAN_RA_LEN);
    CHECK_EQ(link_stats()->ra_pairs_queued, 1);

    /* Another device's request: ignored. The target's: a beacon, with the
     * bridge's next seq and counter, that the transmit gate passes only as
     * a pairing frame answering that request. */
    n = beacon_request(f, stranger, 1);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_REQ);
    CHECK(link_beacon(&bl, &ans) == NULL);
    CHECK_EQ(link_on_frame(f, n, false, now_us, *now_ms), LINK_RX_BAD_CRC);
    uint32_t ctr = link_counter();
    n            = beacon_request(f, joiner, 0x7b);
    CHECK_EQ(link_on_frame(f, n, false, now_us, *now_ms), LINK_RX_BAD_CRC);
    CHECK(link_beacon(&bl, &ans) == NULL);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_ANSWER);
    const uint8_t *b = link_beacon(&bl, &ans);
    CHECK(b != NULL);
    CHECK_EQ(bl, PAIR_BEACON_LEN);
    if (b) {
        CHECK_EQ(txgate_check_frame(&radio, own, b, bl, ans, TXGATE_PAIRING), TXGATE_OK);
        CHECK(txgate_check_frame(&radio, own, b, bl, ans, 0) != TXGATE_OK);
        CHECK(txgate_check_frame(&radio, own, b, bl, NULL, TXGATE_PAIRING) != TXGATE_OK);
        CHECK_MEM(b + 5, joiner, 8);
        CHECK_MEM(b + 13, own, 3);
        size_t k = 0;
        CHECK_EQ(mac_open(&pk, b, bl, body, sizeof body, &k), MAC_OPEN_OK);
        CHECK_EQ(k, 10);
        CHECK_MEM(body, own + 3, 5);
        CHECK_EQ(body[5], 0x04);
        CHECK_EQ(body[6] | body[7] << 8 | body[8] << 16 | (uint32_t)body[9] << 24, ctr);
    }
    CHECK_EQ(link_counter(), ctr + 1u);
    CHECK(!link_pair_engaged(*now_ms)); /* not before the beacon is on the air */
    link_beacon_done(true, *now_ms);
    CHECK(link_beacon(&bl, &ans) == NULL);
    CHECK_EQ(link_stats()->beacons_sent, 1);
    CHECK(link_pair_engaged(*now_ms)); /* the exchange with the target */

    /* The target's Router Solicitation under the pairing key; a stranger's
     * is not even tried under it. Neither goes to the server. */
    n = mac_build_broadcast(&pk, f, sizeof f, false, 8, stranger, 17, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_MIC_FAIL);
    n = mac_build_broadcast(&pk, f, sizeof f, false, 8, joiner, 17, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRING);
    CHECK_EQ(link_uplink_peek(out, sizeof out), 0);

    /* d/pair, a few seconds later: a Data frame to the target under the
     * pairing key carrying the network key as tag 0x12. */
    n_data = 0;
    run_ms(now_ms, PAIR_DPAIR_DELAY_MS + 2000);
    CHECK(n_data >= 1);
    CHECK_EQ(link_stats()->dpair_queued, 1);
    struct mac_data d;
    CHECK_EQ(mac_open_data(&key, last_data, last_data_len, body, sizeof body, &d), MAC_OPEN_MIC);
    CHECK_EQ(mac_open_data(&pk, last_data, last_data_len, body, sizeof body, &d), MAC_OPEN_OK);
    CHECK_MEM(d.dst, joiner, 8);
    struct lowpan_udp u;
    uint16_t          mid = 0;
    CHECK(lowpan_parse_udp(d.dgram, d.dgram_len, &u));
    CHECK_EQ(u.sport, 5683);
    CHECK_EQ(u.dport, 5683);
    CHECK_EQ(u.payload_len, PAIR_COAP_LEN);
    if (u.payload_len == PAIR_COAP_LEN) {
        CHECK_EQ(u.payload[0], 0x40);
        CHECK_EQ(u.payload[1], 0x02);
        CHECK_EQ(u.payload[25], 0x12);
        CHECK_EQ(u.payload[26], 0x10);
        CHECK_MEM(u.payload + 27, v_key.p, 16);
        mid = (uint16_t)(u.payload[2] << 8 | u.payload[3]);
    }
    /* Nobody answers in this port: its attempts end, and the target's next
     * frame under the pairing key brings another one. */
    run_ms(now_ms, 12000);
    CHECK_EQ(pair_stats()->dpair_failed, 1);
    n = mac_build_broadcast(&pk, f, sizeof f, false, 9, joiner, 18, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRING);
    run_ms(now_ms, PAIR_DPAIR_DELAY_MS + 2000);
    CHECK_EQ(link_stats()->dpair_queued, 2);

    /* The target's 2.04 under the network key: paired, the exchange is
     * over, the answer is not relayed, and the window stays open (owner
     * spec 2026-11-17). */
    uint8_t reply[4] = {0x60, 0x44, (uint8_t)(mid >> 8), (uint8_t)mid};
    n                = frame_from(f, joiner, &key, 0x31, reply, sizeof reply);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRED);
    CHECK(link_pair_active(*now_ms));
    CHECK(!link_pair_engaged(*now_ms));
    CHECK(pair_key()->set);
    CHECK_EQ(link_uplink_peek(out, sizeof out), 0);
    /* From then on the device is one of the network's: to the server. */
    static const uint8_t token_req[] = {0x40, 0x02, 0x12, 0x99, 0xB4, 'a', 'u', 't', 'h'};
    n = frame_from(f, joiner, &key, 0x32, token_req, sizeof token_req);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_FORWARDED);
    link_uplink_pop();
    /* It may pair again while the window is open: its Beacon Request is
     * answered; a beacon that is not sent starts no exchange. */
    n = beacon_request(f, joiner, 0x7c);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_ANSWER);
    link_beacon_done(false, *now_ms);
    CHECK(!link_pair_engaged(*now_ms));
    /* Once the window is closed, nothing. */
    link_pair_close();
    n = beacon_request(f, joiner, 0x7d);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_REQ);
    CHECK(link_beacon(&bl, &ans) == NULL);
    run_ms(now_ms, 20000);

    /* A new window: at most PAIR_RA_PAIRS_MAX advertisement pairs however
     * often the target asks. Once they are used up, its Router
     * Solicitations under the pairing key send nothing at all: no lone
     * advertisement under the network key, which it cannot read (#19
     * review M2). The window ends on its own. */
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    unsigned from      = n_bcast_log;
    uint32_t rs_before = link_stats()->rs_seen;
    for (unsigned i = 0; i < 9; i++) {
        n = mac_build_broadcast(&pk, f, sizeof f, false, (uint8_t)(20 + i), joiner, 30 + i, rs,
                                sizeof rs);
        CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRING);
        run_ms(now_ms, 12000);
    }
    CHECK(link_pair_active(*now_ms));
    CHECK(!pair_ra_pairs_left(*now_ms));
    CHECK_EQ(count_pending_bcasts(from), PAIR_RA_PAIRS_MAX);
    CHECK_EQ(n_bcast_log - from, 2 * PAIR_RA_PAIRS_MAX); /* the pairs, nothing else */
    CHECK_EQ(link_stats()->rs_seen - rs_before, PAIR_RA_PAIRS_MAX);
    CHECK_EQ(pair_stats()->dpair_sent, 2 + PAIR_DPAIR_MAX);
    /* A network device's solicitation is still answered as outside a
     * window: one advertisement under the network key. */
    from = n_bcast_log;
    n    = mac_build_broadcast(&key, f, sizeof f, false, 0x50, dev, 0x5000, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BROADCAST);
    run_ms(now_ms, 3000);
    CHECK_EQ(n_bcast_log - from, 1);
    CHECK_EQ(bcast_log[from][0], 0x49);
    CHECK_EQ(mac_open(&key, bcast_log[from], bcast_log_len[from], body, sizeof body, &b0),
             MAC_OPEN_OK);
    run_ms(now_ms, PAIR_WINDOW_MS);
    CHECK(!link_pair_active(*now_ms));
    n = beacon_request(f, joiner, 0x7d);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BEACON_REQ);

    /* While the window has a pair left, an advertisement that finds another
     * broadcast queued waits for it and then goes as a pair, never alone.
     * The counter reservation holds the other broadcast in the queue. */
    static const uint8_t other[] = {0x7B, 0x3B, 58, 0x02, 134, 0, 0, 0, 0, 0, 0, 0};
    unsigned             pairs   = link_stats()->ra_pairs_queued;
    uint32_t             ras     = link_stats()->ra_queued;
    link_set_key(v_key.p, link_counter(), link_counter());
    CHECK(csl_enqueue_broadcast(other, sizeof other, 7, 0, CSL_KEY_NETWORK));
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    from = n_bcast_log;
    run_ms(now_ms, 3000);
    CHECK_EQ(n_bcast_log - from, 0);
    CHECK_EQ(csl_broadcasts_queued(), 1);
    CHECK_EQ(link_stats()->ra_queued, ras);
    link_set_counter_limit(UINT32_MAX);
    run_ms(now_ms, 4000);
    CHECK_EQ(n_bcast_log - from, 3);
    CHECK_EQ(bcast_log[from][0], 0x49);
    CHECK_EQ(bcast_log[from + 1][0], 0x59);
    CHECK_EQ(bcast_log[from + 2][0], 0x49);
    CHECK_EQ(mac_open(&pk, bcast_log[from + 2], bcast_log_len[from + 2], body, sizeof body, &b0),
             MAC_OPEN_OK);
    CHECK_EQ(link_stats()->ra_pairs_queued, pairs + 1);
    link_pair_close();
    run_ms(now_ms, 4000);

    /* A window closes while its d/pair waits for a retry: no new window
     * opens until that job is gone, so the old d/pair never goes out under
     * a new window's pairing key (#19 review L3). Without the pairing key
     * its next attempt fails and drops it, sending nothing. */
    CHECK(!link_pair_frames_queued());
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    run_ms(now_ms, 1500);
    n = mac_build_broadcast(&pk, f, sizeof f, false, 40, joiner, 60, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRING);
    uint32_t dq = link_stats()->dpair_queued, failed = pair_stats()->dpair_failed;
    run_ms(now_ms, PAIR_DPAIR_DELAY_MS + 100);
    CHECK_EQ(link_stats()->dpair_queued, dq + 1);
    CHECK(link_pair_frames_queued()); /* in its retry gap */
    link_pair_close();
    unsigned nd = n_data;
    CHECK(!link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    CHECK(!link_pair_active(*now_ms));
    run_ms(now_ms, 3000);
    CHECK(!link_pair_frames_queued());
    CHECK_EQ(n_data, nd);
    CHECK_EQ(pair_stats()->dpair_failed, failed + 1);
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    link_pair_close();

    /* Between exchanges the 10-minute window keeps its pairs for the
     * target: the periodic advertisement goes as outside a window, once,
     * under the network key, to the network's devices. During an exchange
     * the target's solicitation gets a pair again. */
    n = mac_build_broadcast(&key, f, sizeof f, false, 0x51, dev, 0x5001, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_BROADCAST); /* a device heard */
    run_ms(now_ms, 12000);
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    run_ms(now_ms, 3000); /* the window's first pair */
    pairs           = link_stats()->ra_pairs_queued;
    ras             = link_stats()->ra_queued;
    unsigned before = n_bcast;
    run_ms(now_ms, 250000); /* one periodic advertisement, 212 s later */
    CHECK(link_pair_active(*now_ms));
    CHECK(!link_pair_engaged(*now_ms));
    CHECK_EQ(link_stats()->ra_queued, ras + 1u);
    CHECK_EQ(link_stats()->ra_pairs_queued, pairs);
    CHECK_EQ(n_bcast - before, 1);
    CHECK_EQ(last_bcast[0], 0x49);
    CHECK_EQ(mac_open(&key, last_bcast, last_bcast_len, body, sizeof body, &b0), MAC_OPEN_OK);
    CHECK(pair_ra_pairs_left(*now_ms));
    n = mac_build_broadcast(&pk, f, sizeof f, false, 41, joiner, 61, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, *now_ms), LINK_RX_PAIRING);
    CHECK(link_pair_engaged(*now_ms));
    run_ms(now_ms, 3000);
    CHECK_EQ(link_stats()->ra_pairs_queued, pairs + 1u);
    CHECK_EQ(n_bcast - before, 3);
    link_pair_close();
    run_ms(now_ms, 12000);

    /* A new key (a new network) closes a window. */
    CHECK(link_pair_open(joiner, v_key.p, rnd, *now_ms, PAIR_WINDOW_MS));
    link_set_key(v_key2.p, 1, UINT32_MAX);
    CHECK(!link_pair_active(*now_ms));
    CHECK(!pair_key()->set);
    CHECK(link_pair_open(joiner, v_key2.p, rnd, *now_ms, PAIR_WINDOW_MS));
    link_clear_key();
    CHECK(!link_pair_active(*now_ms));
}

/* Which outcomes count as frames of our own network for the frequency
 * offset warning (#11, src/freqoff.h): every value, so a new one has to be
 * placed here. */
static void test_from_network(void) {
    static const bool want[LINK_RX_COUNT] = {
        [LINK_RX_BAD_CRC]       = false,
        [LINK_RX_ACK_TAKEN]     = true,
        [LINK_RX_ACK_OTHER]     = false,
        [LINK_RX_FORWARDED]     = true,
        [LINK_RX_HELD]          = true,
        [LINK_RX_DUPLICATE]     = true,
        [LINK_RX_NOT_RELAYED]   = true,
        [LINK_RX_DROPPED]       = true,
        [LINK_RX_NOT_FOR_US]    = false,
        [LINK_RX_NO_KEY]        = false,
        [LINK_RX_MIC_FAIL]      = false,
        [LINK_RX_BROADCAST]     = true,
        [LINK_RX_BEACON_REQ]    = false,
        [LINK_RX_BEACON_ANSWER] = false,
        [LINK_RX_PAIRING]       = false,
        [LINK_RX_PAIRED]        = true,
        [LINK_RX_OTHER]         = false,
    };
    _Static_assert(LINK_RX_OTHER + 1 == LINK_RX_COUNT, "a row for every link_rx");
    for (int r = 0; r < LINK_RX_COUNT; r++) {
        CHECK_EQ(link_rx_from_network((enum link_rx)r), want[r]);
    }
    CHECK(!link_rx_from_network(LINK_RX_COUNT));
}

int main(void) {
    static const uint8_t coap[] = {0x44, 0x02, 0x12, 0x34, 1, 2, 3, 4, 0xB1, 's', 0xFF, 7};
    static uint8_t       f[MAC_FRAME_MAX], msg[RELAY_MSG_MAX], out[RELAY_MSG_MAX];
    uint32_t             now_ms = 5000;
    ccm_set_key(&key, v_key.p);

    /* No identity: nothing is taken. */
    link_init(NULL);
    CHECK(!link_ready());
    CHECK(link_own() == NULL);
    size_t n = uplink_frame(f, 1, own, coap, sizeof coap);
    CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms), LINK_RX_OTHER);

    /* Identity but no key: frames for us are seen and left alone; a server
     * message is refused; nothing is queued for the air. */
    link_init(own);
    CHECK(!link_ready());
    CHECK_MEM(link_own(), own, 8);
    CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms), LINK_RX_NO_KEY);
    CHECK_EQ(link_uplink_peek(out, sizeof out), 0);
    size_t m = downlink_msg(msg, coap, sizeof coap);
    CHECK(link_downlink_put(msg, m));
    run_ms(&now_ms, 3000);
    CHECK_EQ(link_stats()->down_refused, 1);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    CHECK_EQ(n_wake + n_data + n_bcast, 0);

    /* With the key: the uplink is queued for the server with its header. */
    link_set_key(v_key.p, 500, UINT32_MAX);
    CHECK(link_ready());
    CHECK_EQ(link_counter(), 500);
    CHECK(link_uplink_room());
    CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms), LINK_RX_FORWARDED);
    size_t l = link_uplink_peek(out, sizeof out);
    CHECK_EQ(l, RELAY_HDR_LEN + sizeof coap);
    struct relay_frame rf;
    CHECK(relay_frame_parse(out, l, &rf));
    CHECK_EQ(rf.direction, RELAY_DIR_UP);
    CHECK_EQ(rf.port, 40001);
    CHECK_MEM(rf.coap, coap, sizeof coap);
    uint8_t ip[16];
    relay_ipv6_from_wire(dev, ip);
    CHECK_MEM(rf.ipv6, ip, 16);
    link_uplink_pop();
    CHECK_EQ(link_uplink_peek(out, sizeof out), 0);

    /* The same frame again, a bad CRC, another destination, a wrong key. */
    CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms + 100), LINK_RX_DUPLICATE);
    CHECK_EQ(link_on_frame(f, n, false, now_us, now_ms), LINK_RX_BAD_CRC);
    static const uint8_t other[8] = {9, 9, 9, 9, 9, 0xC5, 0x1B, 0x00};
    size_t               n2       = uplink_frame(f, 2, other, coap, sizeof coap);
    CHECK_EQ(link_on_frame(f, n2, true, now_us, now_ms), LINK_RX_NOT_FOR_US);
    struct ccm_key wrong;
    ccm_set_key(&wrong, v_key2.p);
    uint8_t dg[LOWPAN_FRAME_MAX];
    size_t  dl = lowpan_build_udp_ll(dg, sizeof dg, dev, own, 1, 5683, coap, sizeof coap);
    n2         = mac_build_data(&wrong, f, sizeof f, false, 3, own, dev, 1, dg, dl);
    CHECK_EQ(link_on_frame(f, n2, true, now_us, now_ms), LINK_RX_MIC_FAIL);
    CHECK_EQ(link_uplink_peek(out, sizeof out), 0);
    /* Frames that are nobody's business here. */
    uint8_t a[MAC_ACK_LEN];
    mac_build_enh_ack(a, 1, own, dev, 0);
    CHECK_EQ(link_on_frame(a, sizeof a, true, now_us, now_ms), LINK_RX_ACK_OTHER);
    static const uint8_t req[27] = {0x0B, 0xE8};
    CHECK_EQ(link_on_frame(req, sizeof req, true, now_us, now_ms), LINK_RX_BEACON_REQ);

    /* The uplink queue fills: room is refused before a frame would be lost. */
    unsigned stored = 0;
    for (unsigned i = 0; i < 200 && link_uplink_room(); i++) {
        n = uplink_frame(f, (uint8_t)(10 + i), own, coap, sizeof coap);
        CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms), LINK_RX_FORWARDED);
        stored++;
    }
    CHECK(stored > 20 && stored < 200);
    CHECK(!link_uplink_room());
    unsigned drained = 0;
    while (link_uplink_peek(out, sizeof out) != 0) {
        link_uplink_pop();
        drained++;
    }
    CHECK_EQ(drained, stored);
    CHECK(link_uplink_room());

    /* A server message becomes a job and goes out: a train, then the frame. */
    m = downlink_msg(msg, coap, sizeof coap);
    CHECK(link_downlink_put(msg, m));
    link_poll(now_ms, 0);
    CHECK_EQ(link_stats()->down_queued, 1);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL - 1);
    run_ms(&now_ms, 200);
    CHECK(n_wake > 20);
    /* More than the job pool holds: the rest waits in the queue, in order. */
    for (unsigned i = 0; i < 2 * CSL_JOB_POOL; i++) {
        CHECK(link_downlink_put(msg, m));
    }
    link_poll(now_ms, 0);
    CHECK_EQ(csl_free_jobs(), 0);
    CHECK_EQ(link_stats()->down_queued, 1 + CSL_JOB_POOL - 1);
    CHECK_EQ(link_stats()->down_full, 0);

    /* Router Advertisement: after a Router Solicitation, with the gateway
     * and DNS addresses the net task gave. A new key drops what was queued. */
    link_set_key(v_key.p, 900, UINT32_MAX);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    n_bcast = 0;
    link_set_router(0x0101A8C0u, 0x0101A8C0u); /* 192.168.1.1 as stored in memory */
    static const uint8_t rs[] = {0x7B, 0x3B, 58, 0x02, 133, 0, 0, 0, 0, 0, 0, 0};
    n = mac_build_broadcast(&key, f, sizeof f, false, 7, dev, 3, rs, sizeof rs);
    CHECK_EQ(link_on_frame(f, n, true, now_us, now_ms), LINK_RX_BROADCAST);
    CHECK_EQ(link_stats()->rs_seen, 1);
    run_ms(&now_ms, 3000);
    CHECK_EQ(n_bcast, 1);
    uint8_t body[MAC_FRAME_MAX];
    size_t  bl = 0;
    CHECK_EQ(mac_open(&key, last_bcast, last_bcast_len, body, sizeof body, &bl), MAC_OPEN_OK);
    CHECK_EQ(bl, 2 + LOWPAN_RA_LEN);
    uint8_t              want[LOWPAN_RA_LEN];
    static const uint8_t gw[4] = {192, 168, 1, 1};
    CHECK_EQ(lowpan_build_ra(want, sizeof want, own, gw, gw), LOWPAN_RA_LEN);
    CHECK_MEM(body + 2, want, LOWPAN_RA_LEN);
    /* The periodic ones follow 200-600 s apart while a device is around. */
    run_ms(&now_ms, 700000);
    CHECK(n_bcast >= 2 && n_bcast <= 4);
    /* With no device heard for 30 minutes they stop. */
    run_ms(&now_ms, 1800000);
    unsigned before = n_bcast;
    run_ms(&now_ms, 1300000);
    CHECK_EQ(n_bcast, before);

    test_pairing(&now_ms);
    test_from_network();
    return TEST_REPORT("test_link");
}
