/* Host tests for src/relay.c and src/msgq.c (#17): the WebSocket frame, the
 * uplink path (duplicates, reassembly, what is and is not relayed) and the
 * downlink path through the CSL coordinator, read back as a device would.
 * tests/host/Makefile builds it twice: test_relay for channel 16 and
 * test_relay_ch26 as the channel-26 build (stage P7.1), where downlinks go
 * to the build's pairing target only (src/txgate.h rule 6; the stand-in
 * provision/peer/provision.h names dev below). */
#include "relay.h"

#include "csl.h"
#include "msgq.h"
#include "txgate.h"

#include "test.h"
#include "vectors_freedo.h"

#include <stdlib.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct ccm_key key;
static struct relay   relay;
static const uint8_t  own[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0xC5, 0x1B, 0x00};
#if TXGATE_NET_ON_PROTECTED
#include "provision.h"
/* The pairing target of provision/peer/provision.h, wire order. */
static const uint8_t dev[8] = {0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
#else
static const uint8_t dev[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
#endif

/* --- CSL port: every Data frame is acknowledged at once -------------------- */
static uint64_t now_us = 1000000;
static struct {
    uint8_t len;
    uint8_t frame[MAC_FRAME_MAX];
} data_sent[32];
static unsigned n_data, n_reports, n_delivered;
static bool     want_ack;
static uint8_t  ack[MAC_ACK_LEN];

uint64_t csl_port_now(void) {
    return now_us;
}
uint32_t csl_port_random(void) {
    return 0;
}
void csl_port_listen_begin(void) {
}
bool csl_port_listen_busy(void) {
    return false;
}
void csl_port_report(const struct csl_report *r) {
    n_reports++;
    n_delivered += r->outcome == CSL_DELIVERED;
}
bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    (void)flags;
    now_us += 4000;
    enum mac_class c = mac_classify(frame, len);
    if ((c == MAC_DATA || c == MAC_DATA_PENDING) && n_data < 32) {
        data_sent[n_data].len = (uint8_t)len;
        memcpy(data_sent[n_data].frame, frame, len);
        n_data++;
        uint8_t peer[8];
        memcpy(peer, frame + 3, 8);
        mac_build_enh_ack(ack, frame[2], own, peer, 0);
        want_ack = true;
    }
    return true;
}

/* Run the coordinator until its queue is empty. */
static void drain(void) {
    for (unsigned guard = 0; guard < 2000000 && csl_free_jobs() != CSL_JOB_POOL; guard++) {
        if (want_ack) {
            want_ack = false;
            now_us += 3000;
            (void)csl_on_frame(ack, sizeof ack, true, now_us);
        }
        uint32_t wait = csl_step();
        if (wait != 0 && wait != UINT32_MAX) {
            now_us += wait;
        }
    }
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
}

static void reset(const uint8_t *me) {
    struct csl_config cfg = {.pan = MAC_PAN_TADO, .key = {&key, NULL}, .counter = 1, .counter_limit = UINT32_MAX, .seq = 1};
    memcpy(cfg.own, me, 8);
    csl_init(&cfg);
    relay_init(&relay, me);
    n_data = n_reports = n_delivered = 0;
    want_ack                         = false;
}

/* --- Frame and addresses ------------------------------------------------------ */
static void test_frame(void) {
    static const uint8_t a[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
    uint8_t              ip[16], wire[8];
    relay_ipv6_from_wire(a, ip);
    static const uint8_t want[16] = {0xFE, 0x80, 0, 0, 0,    0,    0,    0,
                                     0x02, 0x1B, 0xC5, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1};
    CHECK_MEM(ip, want, 16);
    CHECK(relay_wire_from_ipv6(ip, wire));
    CHECK_MEM(wire, a, 8);
    ip[0] = 0xAA; /* not link-local */
    CHECK(!relay_wire_from_ipv6(ip, wire));
    ip[0] = 0xFE;
    ip[9] = 0x1C; /* not a tado address */
    CHECK(!relay_wire_from_ipv6(ip, wire));

    uint8_t msg[RELAY_HDR_LEN + 5] = {0};
    relay_ipv6_from_wire(dev, ip);
    relay_frame_header(msg, ip, 5683, 5);
    memcpy(msg + RELAY_HDR_LEN, "\x40\x01\x12\x34\xFF", 5);
    /* As tanoclo's parser reads it (ws-bridge.js:62-130): big-endian fields. */
    static const uint8_t head[3] = {0x00, 0x01, 0x10};
    CHECK_MEM(msg, head, 3);
    CHECK_MEM(msg + 3, ip, 16);
    CHECK_EQ(msg[22] << 8 | msg[23], 5683);
    CHECK_EQ(msg[26] << 8 | msg[27], 5);
    struct relay_frame f;
    CHECK(relay_frame_parse(msg, sizeof msg, &f));
    CHECK_EQ(f.direction, RELAY_DIR_UP);
    CHECK_EQ(f.port, 5683);
    CHECK_EQ(f.coap_len, 5);
    CHECK(f.coap == msg + RELAY_HDR_LEN);
    CHECK(!relay_frame_parse(msg, sizeof msg - 1, &f)); /* length field does not match */
    CHECK(!relay_frame_parse(msg, RELAY_HDR_LEN - 1, &f));
    msg[2] = 0x04;
    CHECK(!relay_frame_parse(msg, sizeof msg, &f));
}

/* --- Uplink ------------------------------------------------------------------- */
/* A Data frame from `from` to `to` carrying datagram dg, opened as the
 * radio task would. */
static uint8_t         up_frame[MAC_FRAME_MAX], up_body[MAC_FRAME_MAX];
static struct mac_data up_d;
static void make_uplink(const uint8_t *from, const uint8_t *to, uint8_t seq, const uint8_t *dg,
                        size_t dl) {
    size_t n = mac_build_data(&key, up_frame, sizeof up_frame, false, seq, to, from, 77, dg, dl);
    CHECK(n > 0);
    CHECK_EQ(mac_open_data(&key, up_frame, n, up_body, sizeof up_body, &up_d), MAC_OPEN_OK);
}

static void test_uplink(void) {
    reset(own);
    static const uint8_t coap[] = {0x44, 0x02, 0xBE, 0xEF, 1, 2, 3, 4, 0xB1, 'd', 0xFF, 9, 9};
    uint8_t              dg[LOWPAN_FRAME_MAX], ip[16];
    size_t dl = lowpan_build_udp_ll(dg, sizeof dg, dev, own, 40001, 5683, coap, sizeof coap);
    relay_ipv6_from_wire(dev, ip);

    struct relay_uplink up;
    make_uplink(dev, own, 5, dg, dl);
    CHECK_EQ(relay_uplink(&relay, &up_d, 1000, &up), RELAY_UP_FORWARD);
    CHECK_MEM(up.ipv6, ip, 16);
    CHECK_EQ(up.port, 40001);
    CHECK_EQ(up.coap_len, sizeof coap);
    CHECK_MEM(up.coap, coap, sizeof coap);

    /* MAC retransmission: same seq and datagram (the frame counter differs). */
    make_uplink(dev, own, 5, dg, dl);
    CHECK_EQ(relay_uplink(&relay, &up_d, 3000, &up), RELAY_UP_DUPLICATE);
    CHECK_EQ(relay_uplink(&relay, &up_d, 1000 + RELAY_DUP_WINDOW_MS - 1, &up), RELAY_UP_DUPLICATE);
    /* After the window the same bytes are a new frame (a device that
     * restarted its seq). */
    CHECK_EQ(relay_uplink(&relay, &up_d, 1000 + RELAY_DUP_WINDOW_MS, &up), RELAY_UP_FORWARD);
    /* Same seq, other bytes: a new frame. */
    dg[dl - 1] ^= 1;
    make_uplink(dev, own, 5, dg, dl);
    CHECK_EQ(relay_uplink(&relay, &up_d, 12000, &up), RELAY_UP_FORWARD);
    /* Another device with the same seq and bytes is no duplicate either. */
    uint8_t dev2[8];
    memcpy(dev2, dev, 8);
    dev2[0] ^= 0xFF;
    size_t d2 = lowpan_build_udp_ll(dg, sizeof dg, dev2, own, 40001, 5683, coap, sizeof coap);
    make_uplink(dev2, own, 5, dg, d2);
    CHECK_EQ(relay_uplink(&relay, &up_d, 12001, &up), RELAY_UP_FORWARD);

    /* freedo's datagrams: the link-local ones are relayed with the CoAP
     * message freedo's parser found; the context-0 ones (zone plane, between
     * devices) are not. */
    unsigned fwd = 0, zone = 0;
    for (size_t i = 0; i < N(v_dgram); i++) {
        reset(v_dgram[i].l2dst.p);
        make_uplink(v_dgram[i].l2src.p, v_dgram[i].l2dst.p, (uint8_t)i, v_dgram[i].pkt.p,
                    v_dgram[i].pkt.n);
        enum relay_up r = relay_uplink(&relay, &up_d, 0, &up);
        if (v_dgram[i].link_local) {
            CHECK_EQ(r, RELAY_UP_FORWARD);
            CHECK_EQ(up.coap_len, v_dgram[i].coap.n);
            CHECK_MEM(up.coap, v_dgram[i].coap.p, up.coap_len);
            CHECK_EQ(up.port, v_dgram[i].sport);
            relay_ipv6_from_wire(v_dgram[i].l2src.p, ip);
            CHECK_MEM(up.ipv6, ip, 16);
            fwd++;
        } else {
            CHECK_EQ(r, RELAY_UP_NOT_OURS);
            zone++;
        }
    }
    CHECK(fwd >= 4 && zone >= 2);

    /* Not UDP (a Router Solicitation), and not 6LoWPAN at all. */
    reset(own);
    static const uint8_t rs[]   = {0x7B, 0x3B, 58, 0x02, 133, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t junk[] = {0x00, 0x11, 0x22, 0x33};
    make_uplink(dev, own, 1, rs, sizeof rs);
    CHECK_EQ(relay_uplink(&relay, &up_d, 0, &up), RELAY_UP_NOT_UDP);
    make_uplink(dev, own, 2, junk, sizeof junk);
    CHECK_EQ(relay_uplink(&relay, &up_d, 0, &up), RELAY_UP_BAD);
    /* A link-local datagram for another node's address. */
    uint8_t other[] = {0x7E, 0x31, 9, 9, 9, 9, 9, 9, 9, 9, 0xF0, 0x16, 0x33, 0x16, 0x33, 0, 0,
                       0x40, 0x01, 0, 1};
    make_uplink(dev, own, 3, other, sizeof other);
    CHECK_EQ(relay_uplink(&relay, &up_d, 0, &up), RELAY_UP_NOT_OURS);

    /* More devices than entries: the table recycles, nothing overflows. */
    for (unsigned i = 0; i < 3 * RELAY_PEERS; i++) {
        uint8_t e[8];
        memcpy(e, dev, 8);
        e[1]   = (uint8_t)i;
        size_t l = lowpan_build_udp_ll(dg, sizeof dg, e, own, 1, 5683, coap, sizeof coap);
        make_uplink(e, own, 1, dg, l);
        CHECK_EQ(relay_uplink(&relay, &up_d, 100 + i, &up), RELAY_UP_FORWARD);
    }
}

/* Fragmented uplinks: the Data frames freedo built, and the message its
 * reassembler returned. */
static void test_uplink_fragments(void) {
    for (size_t i = 0; i < N(v_frag); i++) {
        const struct vfrag *v = &v_frag[i];
        reset(v->dst.p);
        struct relay_uplink up;
        uint8_t             ip[16];
        relay_ipv6_from_wire(v->src.p, ip);
        for (unsigned f = 0; f < v->count; f++) {
            uint8_t         b[MAC_FRAME_MAX];
            struct mac_data d;
            CHECK_EQ(mac_open_data(&key, v->frame[f].p, v->frame[f].n, b, sizeof b, &d),
                     MAC_OPEN_OK);
            enum relay_up r = relay_uplink(&relay, &d, 50 + f, &up);
            if (f + 1 < v->count) {
                CHECK_EQ(r, RELAY_UP_HELD);
                continue;
            }
            CHECK_EQ(r, RELAY_UP_FORWARD);
            CHECK_EQ(up.coap_len, v->coap.n);
            CHECK_MEM(up.coap, v->coap.p, up.coap_len);
            CHECK_MEM(up.ipv6, ip, 16);
            CHECK(up.coap_len <= RELAY_COAP_MAX);
            /* The last fragment sent again (its ACK was lost): not relayed twice. */
            CHECK_EQ(relay_uplink(&relay, &d, 60 + f, &up), RELAY_UP_DUPLICATE);
        }
    }
}

/* --- Downlink ----------------------------------------------------------------- */
static size_t make_downlink(uint8_t *msg, const uint8_t *to, uint16_t port, const uint8_t *coap,
                            size_t n) {
    uint8_t ip[16];
    relay_ipv6_from_wire(to, ip);
    relay_frame_header(msg, ip, port, n);
    msg[1]  = (uint8_t)RELAY_DIR_DOWN;
    msg[20] = 4; /* field_a, field_b, field_c as tanoclo sends them */
    msg[21] = 2;
    msg[25] = 5;
    memcpy(msg + RELAY_HDR_LEN, coap, n);
    return RELAY_HDR_LEN + n;
}

/* Read the Data frames the coordinator sent as the device would: decrypt,
 * reassemble, parse, check the UDP checksum. */
static void check_received(const uint8_t *coap, size_t n, uint16_t sport, uint16_t dport) {
    static struct lowpan_reasm ra;
    const uint8_t             *dg = NULL;
    size_t                     dl = 0;
    static uint8_t             b[MAC_FRAME_MAX];
    CHECK(n_data >= 1);
    for (unsigned i = 0; i < n_data; i++) {
        struct mac_data d;
        CHECK_EQ(mac_open_data(&key, data_sent[i].frame, data_sent[i].len, b, sizeof b, &d),
                 MAC_OPEN_OK);
        CHECK_MEM(d.dst, dev, 8);
        CHECK_MEM(d.src, own, 8);
        CHECK_EQ(d.pending, i + 1 < n_data); /* Frame Pending on all but the last */
        if (n_data == 1) {
            CHECK(!lowpan_is_fragment(d.dgram, d.dgram_len));
            dg = d.dgram;
            dl = d.dgram_len;
        } else {
            enum lowpan_reasm_result r =
                lowpan_reasm_add(&ra, d.src, d.dgram, d.dgram_len, 0, &dg, &dl);
            CHECK_EQ(r, i + 1 < n_data ? LOWPAN_REASM_HELD : LOWPAN_REASM_DONE);
        }
    }
    struct lowpan_udp u;
    CHECK(lowpan_parse_udp(dg, dl, &u));
    CHECK_EQ(u.sport, sport);
    CHECK_EQ(u.dport, dport);
    CHECK_EQ(u.payload_len, n);
    CHECK_MEM(u.payload, coap, n);
    uint8_t s[16], t[16];
    CHECK(lowpan_addr_ipv6(&u.ip.src, own, s) && lowpan_addr_ipv6(&u.ip.dst, dev, t));
    CHECK(u.has_cksum);
    CHECK_EQ(lowpan_udp_checksum(s, t, u.sport, u.dport, u.payload, u.payload_len), u.cksum);
    CHECK_EQ(n_delivered, n_data);
}

static void test_downlink(void) {
    static uint8_t msg[RELAY_MSG_MAX + 8], coap[RELAY_COAP_MAX + 1];
    srand(3);
    for (size_t i = 0; i < sizeof coap; i++) {
        coap[i] = (uint8_t)rand();
    }

    /* A request from the server: one frame, from port 4005. */
    reset(own);
    coap[0]  = 0x44;
    coap[1]  = 0x03; /* PUT */
    size_t n = make_downlink(msg, dev, 5683, coap, 40);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL - 1);
    drain();
    CHECK_EQ(n_data, 1);
    check_received(coap, 40, RELAY_PORT_BRIDGE, 5683);

    /* The largest that fits one frame, and one byte more (two fragments). */
    reset(own);
    n = make_downlink(msg, dev, 5683, coap, LOWPAN_FRAME_MAX - LOWPAN_LL_HDR_LEN);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    drain();
    CHECK_EQ(n_data, 1);
    check_received(coap, LOWPAN_FRAME_MAX - LOWPAN_LL_HDR_LEN, RELAY_PORT_BRIDGE, 5683);
    reset(own);
    n = make_downlink(msg, dev, 5683, coap, LOWPAN_FRAME_MAX - LOWPAN_LL_HDR_LEN + 1);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    drain();
    CHECK_EQ(n_data, 2);
    check_received(coap, LOWPAN_FRAME_MAX - LOWPAN_LL_HDR_LEN + 1, RELAY_PORT_BRIDGE, 5683);

    /* The largest message there is: 15 frames. */
    reset(own);
    n = make_downlink(msg, dev, 5683, coap, RELAY_COAP_MAX);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL - 15);
    /* A second one does not fit the pool: nothing of it is queued. */
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUE_FULL);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL - 15);
    drain();
    CHECK_EQ(n_data, 15);
    check_received(coap, RELAY_COAP_MAX, RELAY_PORT_BRIDGE, 5683);
    n = make_downlink(msg, dev, 5683, coap, RELAY_COAP_MAX + 1);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_TOO_LARGE);

    /* An answer to the device's request: its ports, swapped. */
    reset(own);
    static const uint8_t req[] = {0x44, 0x01, 0x00, 0x07, 1, 2, 3, 4};
    uint8_t              dg[LOWPAN_FRAME_MAX];
    size_t dl = lowpan_build_udp_ll(dg, sizeof dg, dev, own, 40123, 5683, req, sizeof req);
    make_uplink(dev, own, 9, dg, dl);
    struct relay_uplink up;
    CHECK_EQ(relay_uplink(&relay, &up_d, 0, &up), RELAY_UP_FORWARD);
    coap[0] = 0x64;
    coap[1] = 0x45; /* ACK 2.05 */
    n       = make_downlink(msg, dev, 5683, coap, 30);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    drain();
    check_received(coap, 30, 5683, 40123);
    /* An answer for a device we have not heard: CoAP's port both ways. */
    reset(own);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    drain();
    check_received(coap, 30, 5683, 5683);

    /* Refused: wrong direction, no CoAP, our own address, not link-local,
     * a broken header. */
    reset(own);
    coap[1] = 0x03;
    n       = make_downlink(msg, dev, 5683, coap, 40);
    msg[1]  = (uint8_t)RELAY_DIR_UP;
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_BAD_FRAME);
    n = make_downlink(msg, dev, 5683, coap, 3);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_BAD_FRAME);
    n = make_downlink(msg, own, 5683, coap, 40);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_BAD_ADDRESS);
    n      = make_downlink(msg, dev, 5683, coap, 40);
    msg[3] = 0xAA; /* aaaa::... */
    msg[4] = 0xAA;
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_BAD_ADDRESS);
    n = make_downlink(msg, dev, 5683, coap, 40);
    CHECK_EQ(relay_downlink(&relay, msg, n - 1), RELAY_DOWN_BAD_FRAME);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    CHECK_EQ(n_data, 0);
}

/* Downlinks to any device but the pairing target: refused in the channel-26
 * build, nothing queued and nothing sent; queued as before in the
 * channel-16 build. */
static void test_downlink_peer(void) {
    static uint8_t msg[RELAY_MSG_MAX + 8], coap[64];
    memset(coap, 0x5A, sizeof coap);
    coap[0] = 0x44;
    coap[1] = 0x03; /* PUT */
    enum relay_down other = TXGATE_NET_ON_PROTECTED ? RELAY_DOWN_NOT_PEER : RELAY_DOWN_QUEUED;

#if TXGATE_NET_ON_PROTECTED
    static const uint8_t stored[8] = PROVISION_PAIR_EUI64;
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(stored[i], dev[7 - i]);
    }
#endif
    reset(own);
    size_t n = make_downlink(msg, dev, 5683, coap, 40);
    CHECK_EQ(relay_downlink(&relay, msg, n), RELAY_DOWN_QUEUED);
    drain();
    CHECK_EQ(n_data, 1);

    /* Any one bit of the target's address changed (a neighbour's), and an
     * unrelated device; a request and an answer, one frame and fragments. */
    static const size_t   lens[]   = {40, LOWPAN_FRAME_MAX - LOWPAN_LL_HDR_LEN + 1, RELAY_COAP_MAX};
    static const unsigned frames[] = {1, 2, 15}; /* as test_downlink */
    for (unsigned b = 0; b < 8; b++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t to[8], ip[16], back[8];
            memcpy(to, dev, 8);
            to[b] ^= (uint8_t)(1u << bit);
            relay_ipv6_from_wire(to, ip);
            if (!relay_wire_from_ipv6(ip, back) || memcmp(to, own, 8) == 0) {
                continue; /* refused as an address already (suffix not tado's) */
            }
            for (size_t k = 0; k < N(lens); k++) {
                reset(own);
                coap[1] = (k & 1u) ? 0x45 : 0x03; /* 2.05 or PUT */
                static uint8_t big[RELAY_COAP_MAX];
                memset(big, 0x5A, sizeof big);
                memcpy(big, coap, 4);
                n = make_downlink(msg, to, 5683, big, lens[k]);
                CHECK_EQ(relay_downlink(&relay, msg, n), other);
                if (TXGATE_NET_ON_PROTECTED) {
                    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
                }
                drain();
                CHECK_EQ(n_data, TXGATE_NET_ON_PROTECTED ? 0u : frames[k]);
            }
        }
    }
    static const uint8_t stock[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
    reset(own);
    coap[1] = 0x03;
    n       = make_downlink(msg, stock, 5683, coap, 40);
    CHECK_EQ(relay_downlink(&relay, msg, n), other);
    drain();
    CHECK_EQ(n_data, TXGATE_NET_ON_PROTECTED ? 0u : 1u);
}

/* --- Message queue ------------------------------------------------------------ */
static void test_msgq(void) {
    uint8_t     store[64], out[64];
    struct msgq q;
    msgq_init(&q, store, sizeof store);
    CHECK_EQ(msgq_peek(&q, out, sizeof out), 0);
    CHECK_EQ(msgq_room(&q), 61);
    CHECK(msgq_put(&q, "abc", 3, "defg", 4));
    CHECK(msgq_put(&q, "xy", 2, NULL, 0));
    CHECK_EQ(msgq_peek(&q, out, sizeof out), 7);
    CHECK_MEM(out, "abcdefg", 7);
    CHECK_EQ(msgq_peek(&q, out, sizeof out), 7); /* stays until popped */
    msgq_pop(&q);
    CHECK_EQ(msgq_peek(&q, out, sizeof out), 2);
    CHECK_MEM(out, "xy", 2);
    msgq_pop(&q);
    CHECK_EQ(msgq_peek(&q, out, sizeof out), 0);
    msgq_pop(&q); /* popping an empty queue does nothing */
    CHECK_EQ(msgq_room(&q), 61);
    /* Too large for the queue, and too large for the reader's buffer. */
    uint8_t big[70] = {0};
    CHECK(!msgq_put(&q, big, 62, NULL, 0));
    CHECK(msgq_put(&q, big, 61, NULL, 0));
    CHECK_EQ(msgq_room(&q), 0);
    CHECK(!msgq_put(&q, big, 0, NULL, 0));
    CHECK_EQ(msgq_peek(&q, out, 10), 0); /* dropped */
    CHECK_EQ(msgq_room(&q), 61);

    /* Random traffic against a model, with wrap-around. */
    static uint8_t model[4096][40];
    static uint8_t mlen[4096];
    unsigned       put = 0, got = 0;
    srand(5);
    for (unsigned i = 0; i < 200000; i++) {
        if (rand() % 2 && put < 4096) {
            uint8_t m[40];
            size_t  l = (size_t)(1 + rand() % 39), split = (size_t)rand() % (l + 1);
            for (size_t k = 0; k < l; k++) {
                m[k] = (uint8_t)rand();
            }
            bool fits = msgq_room(&q) >= l;
            CHECK_EQ(msgq_put(&q, m, split, m + split, l - split), fits);
            if (fits) {
                memcpy(model[put], m, l);
                mlen[put++] = (uint8_t)l;
            }
        } else {
            size_t l = msgq_peek(&q, out, sizeof out);
            if (got == put) {
                CHECK_EQ(l, 0);
            } else {
                CHECK_EQ(l, mlen[got]);
                CHECK_MEM(out, model[got], l);
                msgq_pop(&q);
                got++;
            }
        }
        if (put == 4096 && got == put) {
            break;
        }
    }
    CHECK(got > 1000);
}

int main(void) {
    ccm_set_key(&key, v_key.p);
    test_frame();
    test_uplink();
    test_uplink_fragments();
    test_downlink();
    test_downlink_peer();
    test_msgq();
    return TEST_REPORT(TEST_NAME);
}
