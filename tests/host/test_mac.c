/* Host tests for src/mac.c and src/ccm.c (#17) against frames built by
 * freedo's Go code (vectors_freedo.h). */
#include "mac.h"

#include "test.h"
#include "vectors_freedo.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct ccm_key key, key2;

static void test_classify(void) {
    static const struct {
        uint8_t        f[2];
        enum mac_class c;
    } t[] = {
        {{0x69, 0xEC}, MAC_DATA},      {{0x79, 0xEC}, MAC_DATA_PENDING},
        {{0x49, 0xE8}, MAC_BROADCAST}, {{0x59, 0xE8}, MAC_BROADCAST},
        {{0x42, 0xEE}, MAC_ENH_ACK},   {{0x25, 0xAB}, MAC_WAKEUP},
        {{0x0B, 0xE8}, MAC_BEACON_REQUEST},
        {{0x18, 0xEC}, MAC_BEACON},    {{0x61, 0xEC}, MAC_UNKNOWN},
        {{0x69, 0xE8}, MAC_UNKNOWN},
    };
    for (size_t i = 0; i < N(t); i++) {
        CHECK_EQ(mac_classify(t[i].f, 2), t[i].c);
    }
    CHECK_EQ(mac_classify(t[0].f, 1), MAC_UNKNOWN);
}

static void test_wakeup(void) {
    for (size_t i = 0; i < N(v_wake); i++) {
        uint8_t f[MAC_WAKE_LEN];
        mac_build_wakeup(f, MAC_PAN_TADO, v_wake[i].seq, v_wake[i].dst, v_wake[i].rendezvous);
        CHECK_EQ(v_wake[i].frame.n, MAC_WAKE_LEN);
        CHECK_MEM(f, v_wake[i].frame.p, MAC_WAKE_LEN);

        struct mac_wakeup w;
        CHECK(mac_parse_wakeup(v_wake[i].frame.p, v_wake[i].frame.n, &w));
        CHECK_EQ(w.seq, v_wake[i].seq);
        CHECK_EQ(w.pan, MAC_PAN_TADO);
        CHECK_MEM(w.dst_short, v_wake[i].dst, 2);
        CHECK_EQ(w.rendezvous, v_wake[i].rendezvous);
        CHECK_EQ(mac_classify(f, sizeof f), MAC_WAKEUP);
    }
    struct mac_wakeup w;
    CHECK(!mac_parse_wakeup(v_wake[0].frame.p, MAC_WAKE_LEN - 1, &w));

    /* Another network's PAN (#43): only bytes 2-3 change. */
    uint8_t f[MAC_WAKE_LEN];
    mac_build_wakeup(f, 0x4E43u, v_wake[0].seq, v_wake[0].dst, v_wake[0].rendezvous);
    CHECK_EQ(f[2], 0x43);
    CHECK_EQ(f[3], 0x4E);
    CHECK_MEM(f, v_wake[0].frame.p, 2);
    CHECK_MEM(f + 4, v_wake[0].frame.p + 4, MAC_WAKE_LEN - 4);
    CHECK(mac_parse_wakeup(f, sizeof f, &w));
    CHECK_EQ(w.pan, 0x4E43u);

    CHECK(mac_pan_usable(MAC_PAN_TADO));
    CHECK(mac_pan_usable(0x0000u));
    CHECK(mac_pan_usable(0x4E43u));
    CHECK(!mac_pan_usable(0xFFFFu));
    CHECK(!mac_pan_usable(0xFFFEu));
    CHECK(!mac_pan_usable(MAC_PAN_NONE));
}

static void test_enh_ack(void) {
    for (size_t i = 0; i < N(v_ack); i++) {
        const uint8_t *peer = v_ack[i].peer.p, *own = v_ack[i].own.p;
        uint8_t        f[MAC_ACK_LEN];
        mac_build_enh_ack(f, v_ack[i].seq, peer, own, v_ack[i].phase);
        CHECK_EQ(v_ack[i].ack.n, MAC_ACK_LEN);
        CHECK_MEM(f, v_ack[i].ack.p, MAC_ACK_LEN);

        struct mac_enh_ack a;
        CHECK(mac_parse_enh_ack(f, sizeof f, &a));
        CHECK_EQ(a.seq, v_ack[i].seq);
        CHECK_MEM(a.dst, peer, 8);
        CHECK_MEM(a.src, own, 8);
        CHECK_EQ(a.phase, v_ack[i].phase);
        CHECK_EQ(a.period, MAC_CSL_UNITS);

        /* Built from the header of the uplink it answers. */
        uint8_t g[MAC_ACK_LEN] = {0};
        CHECK(mac_ack_for_data(g, v_ack[i].data_hdr.p, own, v_ack[i].phase));
        CHECK_MEM(g, v_ack[i].ack.p, MAC_ACK_LEN);

        /* Not for a frame addressed to someone else, whatever differs. */
        for (unsigned b = 0; b < 8; b++) {
            uint8_t other[8];
            memcpy(other, own, 8);
            other[b] ^= 0x01;
            uint8_t untouched[MAC_ACK_LEN];
            memset(untouched, 0xAA, sizeof untouched);
            CHECK(!mac_ack_for_data(untouched, v_ack[i].data_hdr.p, other, 0));
            CHECK_EQ(untouched[0], 0xAA);
        }
        /* Only for unicast Data frames. */
        static const uint8_t fcfs[][2] = {
            {0x49, 0xE8}, {0x59, 0xE8}, {0x42, 0xEE}, {0x25, 0x00}, {0x0B, 0xE8}, {0x18, 0xEC}};
        for (size_t j = 0; j < N(fcfs); j++) {
            uint8_t h[MAC_HDR_LEN];
            memcpy(h, v_ack[i].data_hdr.p, sizeof h);
            h[0] = fcfs[j][0];
            h[1] = fcfs[j][1];
            CHECK(!mac_ack_for_data(g, h, own, 0));
        }
        /* Frame Pending still gets its ACK. */
        uint8_t h[MAC_HDR_LEN];
        memcpy(h, v_ack[i].data_hdr.p, sizeof h);
        h[0] = 0x79;
        CHECK(mac_ack_for_data(g, h, own, v_ack[i].phase));
        CHECK_MEM(g, v_ack[i].ack.p, MAC_ACK_LEN);

        /* The ACK a device sends for our downlink: build the downlink header
         * (us -> peer) and the device's answer, as freedo's cslAckValid sees
         * them. */
        uint8_t down[MAC_HDR_LEN] = {0x69, 0xEC, v_ack[i].seq};
        memcpy(down + 3, peer, 8);
        memcpy(down + 11, own, 5);
        uint8_t back[MAC_ACK_LEN];
        mac_build_enh_ack(back, v_ack[i].seq, own, peer, 1234);
        CHECK(mac_ack_answers(back, sizeof back, down));
        for (unsigned b = 0; b < MAC_ACK_LEN; b++) { /* any corrupt byte */
            uint8_t bad[MAC_ACK_LEN];
            memcpy(bad, back, sizeof bad);
            bad[b] ^= 0x10;
            CHECK(!mac_ack_answers(bad, sizeof bad, down));
        }
        CHECK(!mac_ack_answers(back, MAC_ACK_LEN - 1, down));
        /* Right checksum, wrong seq or wrong peer: an ACK meant for another frame. */
        mac_build_enh_ack(back, (uint8_t)(v_ack[i].seq + 1), own, peer, 0);
        CHECK(!mac_ack_answers(back, sizeof back, down));
        mac_build_enh_ack(back, v_ack[i].seq, peer, own, 0); /* addresses swapped */
        CHECK(!mac_ack_answers(back, sizeof back, down));

        /* A flipped bit breaks the FCS. */
        f[5] ^= 0x04;
        CHECK(!mac_parse_enh_ack(f, sizeof f, &a));
    }
}

static void test_fcs(void) {
    /* CRC-16/KERMIT check value (reflected 0x1021, init 0). */
    CHECK_EQ(mac_fcs(0, (const uint8_t *)"123456789", 9), 0x2189);
    /* Chaining equals one pass. */
    uint16_t a = mac_fcs(0, v_ack[0].ack.p, 27);
    CHECK_EQ(mac_fcs(mac_fcs(0, v_ack[0].ack.p, 10), v_ack[0].ack.p + 10, 17), a);
}

static void test_data(void) {
    for (size_t i = 0; i < N(v_data); i++) {
        uint8_t f[MAC_FRAME_MAX];
        size_t  n = mac_build_data(&key, f, sizeof f, v_data[i].pending, v_data[i].seq,
                                   v_data[i].dst.p, v_data[i].src.p, v_data[i].counter,
                                   v_data[i].dgram.p, v_data[i].dgram.n);
        CHECK_EQ(n, v_data[i].frame.n);
        CHECK_MEM(f, v_data[i].frame.p, n);

        uint8_t         body[MAC_FRAME_MAX];
        struct mac_data d;
        CHECK_EQ(mac_open_data(&key, v_data[i].frame.p, v_data[i].frame.n, body, sizeof body, &d),
                 MAC_OPEN_OK);
        CHECK_EQ(d.seq, v_data[i].seq);
        CHECK_EQ(d.pending, v_data[i].pending);
        CHECK_MEM(d.dst, v_data[i].dst.p, 8);
        CHECK_MEM(d.src, v_data[i].src.p, 8);
        CHECK_EQ(d.counter, v_data[i].counter);
        CHECK_EQ(d.dgram_len, v_data[i].dgram.n);
        CHECK_MEM(d.dgram, v_data[i].dgram.p, d.dgram_len);

        /* Wrong key, and every single corrupt byte, fail the MIC. */
        CHECK_EQ(mac_open_data(&key2, v_data[i].frame.p, v_data[i].frame.n, body, sizeof body, &d),
                 MAC_OPEN_MIC);
        for (size_t b = 2; b < n; b++) {
            uint8_t bad[MAC_FRAME_MAX];
            memcpy(bad, f, n);
            bad[b] ^= 0x80;
            CHECK_EQ(mac_open_data(&key, bad, n, body, sizeof body, &d), MAC_OPEN_MIC);
        }
        /* Truncated, and too small a scratch buffer. */
        CHECK_EQ(mac_open_data(&key, f, MAC_HDR_LEN + 5, body, sizeof body, &d),
                 MAC_OPEN_MALFORMED);
        CHECK_EQ(mac_open_data(&key, f, n, body, n - MAC_HDR_LEN - MAC_MIC_LEN - 1, &d),
                 MAC_OPEN_MALFORMED);
        /* Output buffer too small. */
        CHECK_EQ(mac_build_data(&key, f, n - 1, v_data[i].pending, v_data[i].seq, v_data[i].dst.p,
                                v_data[i].src.p, v_data[i].counter, v_data[i].dgram.p,
                                v_data[i].dgram.n),
                 0);
    }
    /* One byte more than a frame can carry. */
    uint8_t big[MAC_DATA_MAX_DGRAM + 1] = {0}, f[160];
    CHECK_EQ(MAC_DATA_MAX_DGRAM, 97);
    CHECK_EQ(mac_build_data(&key, f, sizeof f, false, 1, v_data[0].dst.p, v_data[0].src.p, 1, big,
                            sizeof big),
             0);
    /* No key set. */
    struct ccm_key none = {0};
    CHECK_EQ(mac_build_data(&none, f, sizeof f, false, 1, v_data[0].dst.p, v_data[0].src.p, 1, big,
                            10),
             0);
}

/* A frame whose MIC verifies but whose inner FCS is wrong: sealed by hand. */
static void test_fcs_inside(void) {
    uint8_t f[64], body[64];
    memcpy(f, v_data[3].frame.p, MAC_HDR_LEN);
    uint8_t *pt = f + MAC_HDR_LEN;
    memset(pt, 0x11, 12);
    pt[12] = 0xDE; /* not the FCS */
    pt[13] = 0xAD;
    CHECK(ccm_seal(&key, f, f, MAC_HDR_LEN, pt, 14, pt + 14));
    struct mac_data d;
    CHECK_EQ(mac_open_data(&key, f, MAC_HDR_LEN + 14 + 4, body, sizeof body, &d), MAC_OPEN_FCS);
}

static void test_broadcast(void) {
    for (size_t i = 0; i < N(v_bcast); i++) {
        uint8_t f[MAC_FRAME_MAX];
        size_t  n = mac_build_broadcast(&key2, f, sizeof f, v_bcast[i].pending, v_bcast[i].seq,
                                        v_bcast[i].src.p, v_bcast[i].counter, v_bcast[i].pkt.p,
                                        v_bcast[i].pkt.n);
        CHECK_EQ(n, v_bcast[i].frame.n);
        CHECK_MEM(f, v_bcast[i].frame.p, n);
        CHECK_EQ(mac_classify(f, n), MAC_BROADCAST);

        uint8_t body[MAC_FRAME_MAX];
        size_t  bl = 0;
        CHECK_EQ(mac_open(&key2, f, n, body, sizeof body, &bl), MAC_OPEN_OK);
        CHECK_EQ(bl, 2 + v_bcast[i].pkt.n);
        CHECK_MEM(body + 2, v_bcast[i].pkt.p, v_bcast[i].pkt.n);
        CHECK_EQ(mac_open(&key, f, n, body, sizeof body, &bl), MAC_OPEN_MIC);
    }
    uint8_t big[MAC_BCAST_MAX_PKT + 1] = {0}, f[160];
    CHECK_EQ(mac_build_broadcast(&key2, f, sizeof f, false, 1, v_bcast[0].src.p, 1, big,
                                 sizeof big),
             0);
}

/* The Enhanced Beacon (#19) is a sealed frame too: its first 16 bytes are
 * the header and the 10 bytes after them the body. */
static void test_beacon_shape(void) {
    for (size_t i = 0; i < N(v_beacon); i++) {
        const uint8_t *b = v_beacon[i].frame.p;
        CHECK_EQ(v_beacon[i].frame.n, 32);
        uint8_t body[32];
        size_t  bl = 0;
        CHECK_EQ(mac_open(&key2, b, 32, body, sizeof body, &bl), MAC_OPEN_OK);
        CHECK_EQ(bl, 10);
        uint8_t f[32];
        CHECK_EQ(mac_seal(&key2, b, body, bl, f, sizeof f), 32);
        CHECK_MEM(f, b, 32);
        CHECK_EQ(mac_classify(b, 32), MAC_BEACON);
    }
}

int main(void) {
    ccm_set_key(&key, v_key.p);
    ccm_set_key(&key2, v_key2.p);
    test_classify();
    test_fcs();
    test_wakeup();
    test_enh_ack();
    test_data();
    test_fcs_inside();
    test_broadcast();
    test_beacon_shape();
    return TEST_REPORT("test_mac");
}
