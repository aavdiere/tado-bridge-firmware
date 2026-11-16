/* Host tests for src/lowpan.c (#17) against datagrams, fragments and a
 * Router Advertisement built and parsed by freedo's Go code. */
#include "lowpan.h"
#include "mac.h"

#include "test.h"
#include "vectors_freedo.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static void test_parse(void) {
    unsigned link_local = 0, context = 0;
    for (size_t i = 0; i < N(v_dgram); i++) {
        struct lowpan_udp u;
        CHECK(lowpan_parse_udp(v_dgram[i].pkt.p, v_dgram[i].pkt.n, &u));
        CHECK_EQ(u.ip.src.ctx, v_dgram[i].src_ctx);
        CHECK_EQ(u.ip.dst.ctx, v_dgram[i].dst_ctx);
        CHECK_EQ(u.ip.src.has_iid, v_dgram[i].src_has_iid);
        CHECK_EQ(u.ip.dst.has_iid, v_dgram[i].dst_has_iid);
        if (u.ip.src.has_iid) {
            CHECK_MEM(u.ip.src.iid, v_dgram[i].src_iid, 8);
        }
        if (u.ip.dst.has_iid) {
            CHECK_MEM(u.ip.dst.iid, v_dgram[i].dst_iid, 8);
        }
        CHECK_EQ(u.sport, v_dgram[i].sport);
        CHECK_EQ(u.dport, v_dgram[i].dport);
        CHECK_EQ(u.payload_len, v_dgram[i].coap.n);
        CHECK_MEM(u.payload, v_dgram[i].coap.p, u.payload_len);
        CHECK_EQ(u.hdr_len + u.payload_len, v_dgram[i].pkt.n);
        CHECK_EQ(u.cksum, v_dgram[i].cksum_inline);

        bool ll = u.ip.src.ctx == LOWPAN_CTX_STATELESS && u.ip.dst.ctx == LOWPAN_CTX_STATELESS;
        CHECK_EQ(ll, v_dgram[i].link_local);
        link_local += ll;
        context += u.ip.src.ctx == 0;

        /* The checksum freedo recomputes from the frame's L2 addresses. */
        uint8_t src[16], dst[16];
        bool    ok = lowpan_addr_ipv6(&u.ip.src, v_dgram[i].l2src.p, src)
                  && lowpan_addr_ipv6(&u.ip.dst, v_dgram[i].l2dst.p, dst);
        CHECK_EQ(ok, v_dgram[i].cksum_ok);
        if (ok) {
            uint16_t ck = lowpan_udp_checksum(src, dst, u.sport, u.dport, u.payload, u.payload_len);
            CHECK_EQ(ck, v_dgram[i].cksum_calc);
            if (u.has_cksum) {
                CHECK_EQ(ck, u.cksum);
            }
        }

        /* A link-local datagram with an inline checksum is what we build. */
        if (ll && u.has_cksum) {
            uint8_t out[160];
            size_t  n = lowpan_build_udp_ll(out, sizeof out, v_dgram[i].l2src.p, v_dgram[i].l2dst.p,
                                            u.sport, u.dport, u.payload, u.payload_len);
            CHECK_EQ(n, v_dgram[i].pkt.n);
            CHECK_MEM(out, v_dgram[i].pkt.p, n);
            CHECK_EQ(lowpan_build_udp_ll(out, n - 1, v_dgram[i].l2src.p, v_dgram[i].l2dst.p,
                                         u.sport, u.dport, u.payload, u.payload_len),
                     0);
        }

        /* Every truncation is refused or parses within bounds (ASan checks). */
        for (size_t cut = 0; cut < v_dgram[i].pkt.n; cut++) {
            struct lowpan_udp t;
            if (lowpan_parse_udp(v_dgram[i].pkt.p, cut, &t)) {
                CHECK(t.hdr_len <= cut);
            }
        }
    }
    CHECK(link_local >= 4);
    CHECK(context >= 1);

    /* Not IPHC, and IPHC followed by something that is not UDP-NHC. */
    static const uint8_t not_iphc[] = {0x41, 0x60, 0x00, 0x00};
    static const uint8_t icmp[]     = {0x7B, 0x3B, 58, 0x01, 134, 0, 0, 0};
    struct lowpan_udp    u;
    struct lowpan_iphc   h;
    CHECK(!lowpan_parse_udp(not_iphc, sizeof not_iphc, &u));
    CHECK(!lowpan_parse_udp(icmp, sizeof icmp, &u));
    CHECK(lowpan_parse_iphc(icmp, sizeof icmp, &h));
    CHECK(h.dst_multicast && !h.nh_compressed);
    CHECK_EQ(h.next_header, LOWPAN_NH_ICMPV6);
    CHECK_EQ(h.hop_limit, 255);
    CHECK_EQ(h.len, 4);
}

static void test_iid(void) {
    static const uint8_t wire[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC5, 0x1B, 0x00};
    static const uint8_t want[8] = {0x02, 0x1B, 0xC5, 0x55, 0x44, 0x33, 0x22, 0x11};
    uint8_t              iid[8], back[8];
    lowpan_iid_from_wire(wire, iid);
    CHECK_MEM(iid, want, 8);
    lowpan_wire_from_iid(iid, back);
    CHECK_MEM(back, wire, 8);

    struct lowpan_addr a = {.ctx = 1};
    uint8_t            ip[16];
    CHECK(!lowpan_addr_ipv6(&a, wire, ip)); /* only context 0 exists */
    a.ctx = LOWPAN_CTX_UNUSABLE;
    CHECK(!lowpan_addr_ipv6(&a, wire, ip));
}

static void test_fragment(void) {
    for (size_t i = 0; i < N(v_frag); i++) {
        const struct vfrag *v = &v_frag[i];
        CHECK_EQ(lowpan_frag_count(v->pkt.p, v->pkt.n), v->count);
        for (unsigned f = 0; f < v->count; f++) {
            uint8_t body[LOWPAN_FRAME_MAX];
            size_t  n = lowpan_frag_body(v->pkt.p, v->pkt.n, v->tag, f, body);
            CHECK_EQ(n, v->body[f].n);
            CHECK_MEM(body, v->body[f].p, n);
            CHECK(lowpan_is_fragment(body, n));
        }
        uint8_t body[LOWPAN_FRAME_MAX];
        CHECK_EQ(lowpan_frag_body(v->pkt.p, v->pkt.n, v->tag, v->count, body), 0);
    }
    /* Sizes around the single-frame limit. */
    uint8_t pkt[LOWPAN_PKT_MAX + 1] = {0x7E, 0x33, 0xF0};
    uint8_t body[LOWPAN_FRAME_MAX];
    CHECK_EQ(lowpan_frag_count(pkt, 0), 0);
    CHECK_EQ(lowpan_frag_count(pkt, 97), 1);
    CHECK_EQ(lowpan_frag_body(pkt, 97, 1, 0, body), 0);
    CHECK_EQ(lowpan_frag_count(pkt, 98), 2);
    CHECK_EQ(lowpan_frag_body(pkt, 98, 1, 1, body), 5 + 9);
    CHECK_EQ(lowpan_frag_count(pkt, 89 + 92), 2); /* the last FRAGN takes up to 92 */
    CHECK_EQ(lowpan_frag_count(pkt, 89 + 93), 3);
    CHECK_EQ(lowpan_frag_body(pkt, 89 + 93, 1, 1, body), 5 + 88);
    CHECK_EQ(lowpan_frag_body(pkt, 89 + 93, 1, 2, body), 5 + 5);
    CHECK(lowpan_frag_count(pkt, LOWPAN_PKT_MAX) <= 1 + LOWPAN_REASM_PIECES);
    CHECK_EQ(lowpan_frag_count(pkt, LOWPAN_PKT_MAX + 1), 0); /* over the 1280-byte limit */
    pkt[1] = 0xD7; /* not the link-local form: sizes would be wrong */
    CHECK_EQ(lowpan_frag_count(pkt, 200), 0);
    CHECK_EQ(lowpan_frag_count(pkt, 50), 1); /* fits one frame in any form */
}

static struct lowpan_reasm ra;
static struct ccm_key      key;

/* Feeds fragment bodies in the given order; returns the last result. */
static enum lowpan_reasm_result feed(const struct vfrag *v,
                                     const unsigned     *order,
                                     unsigned            n,
                                     uint32_t            now,
                                     const uint8_t     **dg,
                                     size_t             *dl) {
    enum lowpan_reasm_result r = LOWPAN_REASM_REFUSED;
    for (unsigned k = 0; k < n; k++) {
        unsigned f = order[k];
        r          = lowpan_reasm_add(&ra, v->src.p, v->body[f].p, v->body[f].n, now, dg, dl);
        if (k + 1 < n) {
            CHECK_EQ(r, LOWPAN_REASM_HELD);
        }
    }
    return r;
}

static void check_done(const struct vfrag *v, const uint8_t *dg, size_t dl) {
    CHECK_EQ(dl, v->pkt.n);
    CHECK_MEM(dg, v->pkt.p, dl);
    struct lowpan_udp u;
    CHECK(lowpan_parse_udp(dg, dl, &u));
    CHECK_EQ(u.payload_len, v->coap.n); /* the message freedo's reassembler returned */
    CHECK_MEM(u.payload, v->coap.p, u.payload_len);
}

static void test_reassembly(void) {
    for (size_t i = 0; i < N(v_frag); i++) {
        const struct vfrag *v = &v_frag[i];
        unsigned            fwd[16], rev[16];
        for (unsigned f = 0; f < v->count; f++) {
            fwd[f] = f;
            rev[f] = v->count - 1 - f;
        }
        const uint8_t *dg = NULL;
        size_t         dl = 0;

        /* In order. */
        CHECK_EQ(feed(v, fwd, v->count, 1000, &dg, &dl), LOWPAN_REASM_DONE);
        check_done(v, dg, dl);
        CHECK_EQ(lowpan_reasm_pending(&ra, 1000), 0);

        /* Reversed: FRAG1 last. */
        CHECK_EQ(feed(v, rev, v->count, 2000, &dg, &dl), LOWPAN_REASM_DONE);
        check_done(v, dg, dl);

        /* Out of the Data frames themselves, as the radio task gets them. */
        for (unsigned f = 0; f < v->count; f++) {
            uint8_t         body[MAC_FRAME_MAX];
            struct mac_data d;
            CHECK_EQ(mac_open_data(&key, v->frame[f].p, v->frame[f].n, body, sizeof body, &d),
                     MAC_OPEN_OK);
            CHECK(lowpan_is_fragment(d.dgram, d.dgram_len));
            enum lowpan_reasm_result r =
                lowpan_reasm_add(&ra, d.src, d.dgram, d.dgram_len, 3000, &dg, &dl);
            CHECK_EQ(r, f + 1 < v->count ? LOWPAN_REASM_HELD : LOWPAN_REASM_DONE);
        }
        check_done(v, dg, dl);

        /* An exact retransmission of each fragment is accepted. */
        for (unsigned f = 0; f + 1 < v->count; f++) {
            CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[f].p, v->body[f].n, 4000, &dg, &dl),
                     LOWPAN_REASM_HELD);
            CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[f].p, v->body[f].n, 4000, &dg, &dl),
                     LOWPAN_REASM_HELD);
        }
        CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[v->count - 1].p, v->body[v->count - 1].n,
                                  4000, &dg, &dl),
                 LOWPAN_REASM_DONE);
        check_done(v, dg, dl);

        if (v->count >= 3) {
            /* A FRAGN with the same offset and other bytes drops the datagram. */
            uint8_t bad[LOWPAN_FRAME_MAX];
            memcpy(bad, v->body[1].p, v->body[1].n);
            bad[10] ^= 0xFF;
            CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[1].p, v->body[1].n, 5000, &dg, &dl),
                     LOWPAN_REASM_HELD);
            CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, bad, v->body[1].n, 5000, &dg, &dl),
                     LOWPAN_REASM_REFUSED);
            CHECK_EQ(lowpan_reasm_pending(&ra, 5000), 0);

            /* A missing fragment never completes, and the rest expires. */
            for (unsigned f = 0; f < v->count; f++) {
                if (f != 1) {
                    CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[f].p, v->body[f].n, 6000, &dg,
                                              &dl),
                             LOWPAN_REASM_HELD);
                }
            }
            CHECK_EQ(lowpan_reasm_pending(&ra, 6000 + LOWPAN_REASM_TIMEOUT_MS - 1), 1);
            CHECK_EQ(lowpan_reasm_pending(&ra, 6000 + LOWPAN_REASM_TIMEOUT_MS), 0);
            /* After the timeout the late fragment starts a new, incomplete one. */
            CHECK_EQ(lowpan_reasm_add(&ra, v->src.p, v->body[1].p, v->body[1].n, 70000, &dg, &dl),
                     LOWPAN_REASM_HELD);
            CHECK_EQ(lowpan_reasm_pending(&ra, 70000 + LOWPAN_REASM_TIMEOUT_MS), 0);
        }
    }

    /* Two datagrams from different sources interleave without mixing. */
    const struct vfrag *a = &v_frag[2], *b = &v_frag[5];
    const uint8_t      *dg = NULL;
    size_t              dl = 0;
    unsigned            done = 0;
    for (unsigned f = 0; f < 16; f++) {
        if (f < a->count
            && lowpan_reasm_add(&ra, a->src.p, a->body[f].p, a->body[f].n, 100, &dg, &dl)
                   == LOWPAN_REASM_DONE) {
            check_done(a, dg, dl);
            done++;
        }
        if (f < b->count
            && lowpan_reasm_add(&ra, b->src.p, b->body[f].p, b->body[f].n, 100, &dg, &dl)
                   == LOWPAN_REASM_DONE) {
            check_done(b, dg, dl);
            done++;
        }
    }
    CHECK_EQ(done, 2);

    /* More datagrams than slots: the oldest gives way, nothing overflows. */
    for (unsigned s = 0; s < LOWPAN_REASM_SLOTS + 2; s++) {
        uint8_t src[8] = {(uint8_t)s, 1, 2, 3, 4, 0xC5, 0x1B, 0x00};
        CHECK_EQ(lowpan_reasm_add(&ra, src, b->body[0].p, b->body[0].n, 200 + s, &dg, &dl),
                 LOWPAN_REASM_HELD);
    }
    CHECK_EQ(lowpan_reasm_pending(&ra, 300), LOWPAN_REASM_SLOTS);
    CHECK_EQ(lowpan_reasm_pending(&ra, 300 + LOWPAN_REASM_TIMEOUT_MS), 0);

    /* Malformed fragment headers. */
    static const uint8_t short_frag[] = {0xC0, 0x40, 0x00};
    static const uint8_t tiny_size[]  = {0xC0, 0x10, 0x00, 0x01, 0x7E, 0x33, 0xF0, 0, 0, 0, 0, 0, 0};
    static const uint8_t low_offset[] = {0xE0, 0x80, 0x00, 0x01, 0x05, 1, 2, 3};
    static const uint8_t past_end[]   = {0xE0, 0x80, 0x00, 0x01, 0x0F, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static const uint8_t not_udp[]    = {0xC0, 0x80, 0x00, 0x01, 0x7B, 0x3B, 58, 1, 134, 0};
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, short_frag, sizeof short_frag, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED);
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, tiny_size, sizeof tiny_size, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED);
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, low_offset, sizeof low_offset, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED);
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, past_end, sizeof past_end, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED);
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, not_udp, sizeof not_udp, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED);
    CHECK_EQ(lowpan_reasm_add(&ra, a->src.p, v_dgram[0].pkt.p, v_dgram[0].pkt.n, 0, &dg, &dl),
             LOWPAN_REASM_REFUSED); /* not a fragment at all */
}

static void test_ra(void) {
    static const uint8_t gw[4] = {192, 168, 1, 1};
    uint8_t              out[LOWPAN_RA_LEN];
    CHECK_EQ(v_ra.pkt.n, LOWPAN_RA_LEN);
    CHECK_EQ(lowpan_build_ra(out, sizeof out, v_ra.src.p, gw, gw), LOWPAN_RA_LEN);
    CHECK_MEM(out, v_ra.pkt.p, LOWPAN_RA_LEN);
    CHECK_EQ(lowpan_build_ra(out, LOWPAN_RA_LEN - 1, v_ra.src.p, gw, gw), 0);
    CHECK(LOWPAN_RA_LEN <= MAC_BCAST_MAX_PKT);
}

int main(void) {
    ccm_set_key(&key, v_key.p);
    test_parse();
    test_iid();
    test_fragment();
    test_reassembly();
    test_ra();
    return TEST_REPORT("test_lowpan");
}
