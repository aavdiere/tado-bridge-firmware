/* Host tests for src/coap.c: the bridge's answer to tanoclo's PUT d/config
 * (refs #19). The request and answer bytes come from tanoclo's own codec
 * (ws-server/lib/coap.js buildRequest and buildAck, with the options and TLV
 * entries of lib/commands/device.js:801-822 and a VA serial). */
#include "coap.h"

#include "test.h"

/* coap.buildRequest({code: PUT, path: 'd/config', token: a1b2, mid: 0x7123,
 * type: CON, payload: tlv.encode(...), extraOptions: [{7, ffff}, {12, 2a}]}) */
static const uint8_t push[] = {0x42, 0x03, 0x71, 0x23, 0xA1, 0xB2, 0x72, 0xFF, 0xFF, 0x41, 0x64,
                               0x06, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x11, 0x2A, 0xFF, 0x00,
                               0x01, 0x0C, 0x56, 0x41, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                               0x38, 0x39, 0x30, 0x00, 0x36, 0x01, 0x03, 0x00, 0x3A, 0x02, 0xD7,
                               0x02, 0x40, 0x00, 0x02, 0x00, 0x01, 0x40, 0x20, 0x01, 0x05};
/* coap.buildAck(coap.parse(push)): what tanoclo answers a PUT. */
static const uint8_t push_ack[] = {0x62, 0x44, 0x71, 0x23, 0xA1, 0xB2};

static void test_push(void) {
    struct coap_msg m;
    CHECK(coap_parse(push, sizeof push, &m));
    CHECK_EQ(m.type, COAP_CON);
    CHECK_EQ(m.code, COAP_PUT);
    CHECK_EQ(m.mid, 0x7123);
    CHECK(coap_path_is(&m, "d/config"));
    CHECK(!coap_path_is(&m, "d"));
    CHECK(!coap_path_is(&m, "d/config/x"));
    CHECK(!coap_path_is(&m, "d/confi"));
    CHECK(!coap_path_is(&m, "config"));
    CHECK(!coap_path_is(&m, ""));

    const uint8_t *v;
    size_t         vl;
    CHECK(tlv_find(m.payload, m.payload_len, 0x0001u, &v, &vl));
    CHECK_EQ(vl, 12);
    CHECK_MEM(v, "VA1234567890", 12);

    uint8_t out[16];
    size_t  n = coap_reply(&m, COAP_CHANGED, out, sizeof out);
    CHECK_EQ(n, sizeof push_ack);
    CHECK_MEM(out, push_ack, sizeof push_ack);
    /* Exactly fits, one byte short. */
    CHECK_EQ(coap_reply(&m, COAP_CHANGED, out, sizeof push_ack), sizeof push_ack);
    CHECK_EQ(coap_reply(&m, COAP_CHANGED, out, sizeof push_ack - 1u), 0);

    /* The answer parses back as an empty-bodied ACK that matches the push. */
    struct coap_msg a;
    CHECK(coap_parse(out, n, &a));
    CHECK_EQ(a.type, COAP_ACK);
    CHECK_EQ(a.code, COAP_CHANGED);
    CHECK_EQ(a.mid, m.mid);
    CHECK_EQ(a.tkl, m.tkl);
    CHECK_MEM(a.token, m.token, m.tkl);
    CHECK_EQ(a.opts_len, 0);
    CHECK_EQ(a.payload_len, 0);
}

/* Paths the bridge must not take for d/config. */
static void test_paths(void) {
    uint8_t              b[64];
    struct coap_writer   w;
    struct coap_msg      m;
    static const uint8_t tok[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    /* One segment "d/config" (a '/' inside the option) is not the path d/config. */
    coap_begin(&w, b, sizeof b, COAP_CON, COAP_PUT, 1, tok, 8);
    coap_option(&w, COAP_OPT_URI_PATH, "d/config", 8);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK(!coap_path_is(&m, "d/config"));

    /* A device's config (d/<serial>/config) is not the bridge's. */
    coap_begin(&w, b, sizeof b, COAP_CON, COAP_PUT, 2, tok, 8);
    coap_option(&w, COAP_OPT_URI_PATH, "d", 1);
    coap_option(&w, COAP_OPT_URI_PATH, "VA1234567890", 12);
    coap_option(&w, COAP_OPT_URI_PATH, "config", 6);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK(!coap_path_is(&m, "d/config"));

    /* d/rfkey, d/pair: not d/config. */
    coap_begin(&w, b, sizeof b, COAP_CON, COAP_GET, 3, tok, 8);
    coap_option(&w, COAP_OPT_URI_PATH, "d", 1);
    coap_option(&w, COAP_OPT_URI_PATH, "rfkey", 5);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK(!coap_path_is(&m, "d/config"));
    CHECK(coap_path_is(&m, "d/rfkey"));

    /* No Uri-Path at all (the time broadcast). */
    coap_begin(&w, b, sizeof b, COAP_NON, COAP_CONTENT, 4, tok, 0);
    coap_payload(&w, "\x0d\x01\x02\x03\x04", 5);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK(!coap_path_is(&m, "d/config"));
    CHECK(coap_path_is(&m, ""));

    /* 8-byte token echoed in full; no token gives a 4-byte answer. */
    coap_begin(&w, b, sizeof b, COAP_CON, COAP_PUT, 0xBEEF, tok, 8);
    coap_option(&w, COAP_OPT_URI_PATH, "d", 1);
    coap_option(&w, COAP_OPT_URI_PATH, "config", 6);
    coap_option(&w, COAP_OPT_SESSION, tok, 8);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK(coap_path_is(&m, "d/config"));
    uint8_t out[12];
    CHECK_EQ(coap_reply(&m, COAP_CHANGED, out, sizeof out), 12);
    static const uint8_t want8[] = {0x68, 0x44, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_MEM(out, want8, sizeof want8);

    coap_begin(&w, b, sizeof b, COAP_CON, COAP_PUT, 0x0102, tok, 0);
    CHECK(!w.overflow && coap_parse(b, w.len, &m));
    CHECK_EQ(coap_reply(&m, COAP_CHANGED, out, sizeof out), 4);
    static const uint8_t want0[] = {0x60, 0x44, 0x01, 0x02};
    CHECK_MEM(out, want0, sizeof want0);
}

int main(void) {
    test_push();
    test_paths();
    return TEST_REPORT("test_coap");
}
