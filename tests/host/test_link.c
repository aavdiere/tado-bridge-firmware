/* Host tests for src/link.c (#17): nothing happens without a key, uplinks
 * reach the queue for the net task, downlinks reach the coordinator, the
 * uplink queue fills without overflowing, Router Advertisements. */
#include "link.h"

#include "csl.h"
#include "lowpan.h"
#include "relay.h"

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
    (void)r;
}
bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    (void)flags;
    now_us += 4000;
    switch (mac_classify(frame, len)) {
        case MAC_WAKEUP:
            n_wake++;
            break;
        case MAC_BROADCAST:
            n_bcast++;
            memcpy(last_bcast, frame, len);
            last_bcast_len = len;
            break;
        default:
            n_data++;
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

    return TEST_REPORT("test_link");
}
