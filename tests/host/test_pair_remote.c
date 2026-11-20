/* Host tests for src/pair_remote.c: the server's remote pairing trigger
 * (#40). The request bytes come from tanoclo's own codec
 * (ws-server/lib/coap.js buildRequest, with the path, options and payload
 * of pushDevicePair(), lib/commands/device.js:378-387) for a made-up bridge
 * serial IB1234567890, token a1 b2 c3, MID 0x7abc. */
#include "pair_remote.h"

#include "pair.h"

#include "test.h"

#include <stdint.h>

#define SERIAL "IB1234567890"

/* Duration 300 (tanoclo's default), 0 (close) and 65535 (its maximum). */
#define REQ_HEAD                                                                           \
    0x43, 0x02, 0x7a, 0xbc, 0xa1, 0xb2, 0xc3, 0x72, 0xff, 0xff, 0x41, 0x64, 0x01, 0x49,    \
        0x0d, 0x09, 0x49, 0x42, 0x30, 0x30, 0x30, 0x30, 0x46, 0x54, 0x30, 0x31, 0x30, 0x30, \
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x04, 0x70, 0x61, 0x69, \
        0x72, 0x11, 0x2a, 0xff
static const uint8_t req_300[]   = {REQ_HEAD, 0x01, 0x04, 0x00, 0x00, 0x01, 0x2c, 0x02, 0x01, 0x00};
static const uint8_t req_0[]     = {REQ_HEAD, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00};
static const uint8_t req_65535[] = {REQ_HEAD, 0x01, 0x04, 0x00, 0x00, 0xff, 0xff, 0x02, 0x01, 0x00};
/* coap.buildAck(coap.parse(req_300)): tanoclo's own answer to a request. */
static const uint8_t ack_204[] = {0x63, 0x44, 0x7a, 0xbc, 0xa1, 0xb2, 0xc3};

static enum pair_remote_rc check_bytes(const uint8_t *b,
                                       size_t         n,
                                       const char    *serial,
                                       bool           target,
                                       bool           tx,
                                       bool          *open,
                                       uint32_t      *dur) {
    struct coap_msg m;
    if (!coap_parse(b, n, &m) || !pair_remote_path(&m)) {
        return PAIR_REMOTE_RC_COUNT;
    }
    return pair_remote_check(&m, serial, target, tx, open, dur);
}

/* A POST on path segs (joined by '/') with the given payload. */
static size_t build(uint8_t *b, size_t cap, uint8_t code, const char *path, const uint8_t *p, size_t pn) {
    static const uint8_t tok[3] = {0xa1, 0xb2, 0xc3};
    struct coap_writer   w;
    coap_begin(&w, b, cap, COAP_CON, code, 0x7abc, tok, sizeof tok);
    coap_option(&w, 7u, "\xff\xff", 2);
    for (const char *s = path; *s;) {
        const char *e = strchr(s, '/');
        size_t      n = e ? (size_t)(e - s) : strlen(s);
        coap_option(&w, COAP_OPT_URI_PATH, s, n);
        s += n + (e ? 1u : 0u);
    }
    coap_option(&w, 12u, "\x2a", 1);
    coap_payload(&w, p, pn);
    return w.overflow ? 0u : w.len;
}

static void test_tanoclo_bytes(void) {
    bool     open = true;
    uint32_t dur  = 1;
    CHECK_EQ(check_bytes(req_300, sizeof req_300, SERIAL, true, true, &open, &dur),
             PAIR_REMOTE_FORWARD);
    CHECK(open);
    CHECK_EQ(dur, 300);
    CHECK_EQ(pair_remote_window_ms(dur), 300000);

    CHECK_EQ(check_bytes(req_0, sizeof req_0, SERIAL, true, true, &open, &dur),
             PAIR_REMOTE_FORWARD);
    CHECK(!open);
    CHECK_EQ(dur, 0);

    /* 65535 s: capped at the button's 10 minutes. */
    CHECK_EQ(check_bytes(req_65535, sizeof req_65535, SERIAL, true, true, &open, &dur),
             PAIR_REMOTE_FORWARD);
    CHECK(open);
    CHECK_EQ(dur, 65535);
    CHECK_EQ(pair_remote_window_ms(dur), PAIR_WINDOW_MS);
    CHECK_EQ(pair_remote_window_ms(1), 1000);
    CHECK_EQ(pair_remote_window_ms(599), 599000);
    CHECK_EQ(pair_remote_window_ms(600), PAIR_WINDOW_MS);
    CHECK_EQ(pair_remote_window_ms(UINT32_MAX), PAIR_WINDOW_MS);

    /* The answer: piggybacked 2.04, as tanoclo answers its own requests. */
    struct coap_msg m;
    uint8_t         out[16];
    CHECK(coap_parse(req_300, sizeof req_300, &m));
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_OPENED), COAP_CHANGED);
    size_t n = coap_reply(&m, pair_remote_code(PAIR_REMOTE_OPENED), out, sizeof out);
    CHECK_EQ(n, sizeof ack_204);
    CHECK_MEM(out, ack_204, sizeof ack_204);
}

static void test_paths(void) {
    uint8_t              b[96];
    struct coap_msg      m;
    static const uint8_t pl[] = {0x01, 0x04, 0x00, 0x00, 0x01, 0x2c};
    bool                 open;
    uint32_t             dur;
    size_t               n;

    n = build(b, sizeof b, COAP_POST, "d/I/IB0000FT01001234567890/pair", pl, sizeof pl);
    CHECK(n && coap_parse(b, n, &m) && pair_remote_path(&m));
    CHECK_EQ(pair_remote_check(&m, SERIAL, true, true, &open, &dur), PAIR_REMOTE_FORWARD);

    /* Not the pair path at all. */
    static const char *const not_path[] = {
        "d/I/IB0000FT01001234567890",
        "d/I/IB0000FT01001234567890/pair/x",
        "d/I/IB0000FT01001234567890/pai",
        "d/i/IB0000FT01001234567890/pair",
        "x/I/IB0000FT01001234567890/pair",
        "d/pair",
        "d/config",
        "I/IB0000FT01001234567890/pair",
    };
    for (unsigned i = 0; i < sizeof not_path / sizeof not_path[0]; i++) {
        n = build(b, sizeof b, COAP_POST, not_path[i], pl, sizeof pl);
        CHECK(n && coap_parse(b, n, &m));
        CHECK(!pair_remote_path(&m));
    }

    /* The pair path, but another serial: 4.04, nothing opened. */
    static const char *const other[] = {
        "d/I/IB0000FT01001234567891/pair", /* last digit */
        "d/I/IB0000FT0100123456789/pair",  /* one short */
        "d/I/IB0000FT010012345678900/pair", /* one more */
        "d/I/IB0000FT0101234567890/pair",  /* prefix one short */
        "d/I/IB1234567890/pair",           /* the bare serial */
        "d/I/IB0000FT0100IB1234567890/pair",
        "d/I/ib0000ft01001234567890/pair",
        "d/I/IB0000FT0100/pair",
    };
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        n = build(b, sizeof b, COAP_POST, other[i], pl, sizeof pl);
        CHECK(n && coap_parse(b, n, &m) && pair_remote_path(&m));
        open = true;
        CHECK_EQ(pair_remote_check(&m, SERIAL, true, true, &open, &dur), PAIR_REMOTE_NOT_OURS);
        CHECK(!open);
    }
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_NOT_OURS), COAP_NOT_FOUND);

    /* Our path against another identity, and without one. */
    n = build(b, sizeof b, COAP_POST, "d/I/IB0000FT01001234567890/pair", pl, sizeof pl);
    CHECK(n && coap_parse(b, n, &m));
    CHECK_EQ(pair_remote_check(&m, "IB1234567891", true, true, &open, &dur), PAIR_REMOTE_NOT_OURS);
    CHECK_EQ(pair_remote_check(&m, "", true, true, &open, &dur), PAIR_REMOTE_NOT_OURS);
    CHECK_EQ(pair_remote_check(&m, NULL, true, true, &open, &dur), PAIR_REMOTE_NOT_OURS);
    /* A serial without "IB" goes in whole, as tanoclo's replace(/^IB/, '') does. */
    n = build(b, sizeof b, COAP_POST, "d/I/IB0000FT0100XY12/pair", pl, sizeof pl);
    CHECK(n && coap_parse(b, n, &m));
    CHECK_EQ(pair_remote_check(&m, "XY12", true, true, &open, &dur), PAIR_REMOTE_FORWARD);

    /* Wrong serial goes before the method and the payload. */
    n = build(b, sizeof b, COAP_GET, "d/I/IB0000FT01001234567891/pair", NULL, 0);
    CHECK(n && coap_parse(b, n, &m));
    CHECK_EQ(pair_remote_check(&m, SERIAL, true, true, &open, &dur), PAIR_REMOTE_NOT_OURS);

    /* Not a POST: 4.05. */
    static const uint8_t methods[] = {COAP_GET, COAP_PUT, COAP_CODE(0, 4)};
    for (unsigned i = 0; i < sizeof methods; i++) {
        n = build(b, sizeof b, methods[i], "d/I/IB0000FT01001234567890/pair", pl, sizeof pl);
        CHECK(n && coap_parse(b, n, &m));
        CHECK_EQ(pair_remote_check(&m, SERIAL, true, true, &open, &dur), PAIR_REMOTE_METHOD);
    }
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_METHOD), COAP_CODE(4, 5));
}

static enum pair_remote_rc check_payload(const uint8_t *p, size_t pn, bool *open, uint32_t *dur) {
    uint8_t         b[128];
    struct coap_msg m;
    size_t n = build(b, sizeof b, COAP_POST, "d/I/IB0000FT01001234567890/pair", p, pn);
    if (!n || !coap_parse(b, n, &m)) {
        return PAIR_REMOTE_RC_COUNT;
    }
    return pair_remote_check(&m, SERIAL, true, true, open, dur);
}

static void test_payloads(void) {
    bool     open;
    uint32_t dur;

    /* Variants that are well formed. */
    static const uint8_t only_dur[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x3c};
    CHECK_EQ(check_payload(only_dur, sizeof only_dur, &open, &dur), PAIR_REMOTE_FORWARD);
    CHECK(open);
    CHECK_EQ(dur, 60);
    static const uint8_t tag2_first[] = {0x02, 0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01};
    CHECK_EQ(check_payload(tag2_first, sizeof tag2_first, &open, &dur), PAIR_REMOTE_FORWARD);
    CHECK_EQ(dur, 1);
    static const uint8_t unknown[] = {0x7f, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x02,
                                      0x33, 0x03, 0xaa, 0xbb, 0xcc};
    CHECK_EQ(check_payload(unknown, sizeof unknown, &open, &dur), PAIR_REMOTE_FORWARD);
    CHECK_EQ(dur, 2);
    /* Upper bytes set (tanoclo never does): read as u32, the window capped. */
    static const uint8_t big[] = {0x01, 0x04, 0xff, 0xff, 0xff, 0xff};
    CHECK_EQ(check_payload(big, sizeof big, &open, &dur), PAIR_REMOTE_FORWARD);
    CHECK_EQ(dur, UINT32_MAX);
    CHECK(open);
    static const uint8_t close_only[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x00};
    CHECK_EQ(check_payload(close_only, sizeof close_only, &open, &dur), PAIR_REMOTE_FORWARD);
    CHECK(!open);

    /* Malformed: 4.00, nothing opened. */
    static const struct {
        uint8_t b[12];
        size_t  n;
    } bad[] = {
        {{0}, 0},                                                 /* no payload */
        {{0x01}, 1},                                              /* header cut */
        {{0x01, 0x04, 0x00, 0x00, 0x01}, 5},                      /* value cut */
        {{0x01, 0x02, 0x01, 0x2c}, 4},                            /* duration of 2 bytes */
        {{0x01, 0x05, 0x00, 0x00, 0x00, 0x01, 0x2c}, 7},          /* of 5 bytes */
        {{0x01, 0x00}, 2},                                        /* of 0 bytes */
        {{0x02, 0x01, 0x00}, 3},                                  /* no duration */
        {{0x01, 0x04, 0, 0, 1, 0x2c, 0x01, 0x04, 0, 0, 0, 0}, 12}, /* two durations */
        {{0x01, 0x04, 0x00, 0x00, 0x01, 0x2c, 0x02}, 7},          /* trailing header byte */
        {{0x01, 0x04, 0x00, 0x00, 0x01, 0x2c, 0x02, 0x02, 0x00}, 9}, /* trailing value cut */
        {{0x02, 0xff, 0x01, 0x04, 0x00, 0x00, 0x01, 0x2c}, 8},    /* length past the end */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        open = true;
        dur  = 7;
        CHECK_EQ(check_payload(bad[i].b, bad[i].n, &open, &dur), PAIR_REMOTE_MALFORMED);
        CHECK(!open);
        CHECK_EQ(dur, 0);
    }
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_MALFORMED), COAP_BAD_REQUEST);
}

/* The build's guards: no target, receive-only. A close needs neither. */
static void test_build_guards(void) {
    bool     open;
    uint32_t dur;
    CHECK_EQ(check_bytes(req_300, sizeof req_300, SERIAL, false, true, &open, &dur),
             PAIR_REMOTE_NO_TARGET);
    CHECK(!open);
    CHECK_EQ(dur, 300);
    CHECK_EQ(check_bytes(req_300, sizeof req_300, SERIAL, true, false, &open, &dur),
             PAIR_REMOTE_RX_ONLY);
    CHECK(!open);
    CHECK_EQ(check_bytes(req_300, sizeof req_300, SERIAL, false, false, &open, &dur),
             PAIR_REMOTE_NO_TARGET);
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_NO_TARGET), COAP_CODE(5, 1));
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_RX_ONLY), COAP_CODE(5, 1));
    /* Wrong serial and malformed still win over the build's guards. */
    CHECK_EQ(check_bytes(req_300, sizeof req_300, "IB1234567891", false, false, &open, &dur),
             PAIR_REMOTE_NOT_OURS);
    static const uint8_t cut[] = {0x01, 0x04, 0x00};
    uint8_t              b[96];
    struct coap_msg      m;
    size_t n = build(b, sizeof b, COAP_POST, "d/I/IB0000FT01001234567890/pair", cut, sizeof cut);
    CHECK(n && coap_parse(b, n, &m));
    CHECK_EQ(pair_remote_check(&m, SERIAL, false, false, &open, &dur), PAIR_REMOTE_MALFORMED);
    /* Duration 0 goes to the radio task, which closes a window if one is open. */
    CHECK_EQ(check_bytes(req_0, sizeof req_0, SERIAL, false, false, &open, &dur),
             PAIR_REMOTE_FORWARD);
    CHECK(!open);
}

static void test_codes(void) {
    static const enum pair_remote_rc ok[] = {
        PAIR_REMOTE_OPENED, PAIR_REMOTE_WAS_OPEN, PAIR_REMOTE_CLOSED, PAIR_REMOTE_WAS_CLOSED};
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        CHECK_EQ(pair_remote_code(ok[i]), COAP_CHANGED);
    }
    for (unsigned rc = PAIR_REMOTE_SELF_TARGET; rc < PAIR_REMOTE_RC_COUNT; rc++) {
        uint8_t c = pair_remote_code((enum pair_remote_rc)rc);
        CHECK(c == COAP_UNAVAILABLE || c == COAP_CODE(5, 1));
    }
    for (unsigned rc = 0; rc < PAIR_REMOTE_RC_COUNT; rc++) {
        const char *t = pair_remote_text((enum pair_remote_rc)rc);
        CHECK(t && *t && strlen(t) <= 80); /* the session's log line stays below 192 bytes */
    }
    CHECK_EQ(pair_remote_code(PAIR_REMOTE_RC_COUNT), COAP_INTERNAL);
}

static void test_seen(void) {
    struct pair_remote_seen s;
    memset(&s, 0, sizeof s);
    enum pair_remote_rc rc;
    uint32_t            secs;
    CHECK(!pair_remote_seen_find(&s, 0x7abc, 0, &rc, &secs));

    /* A resent open gets the first answer for the lifetime, on a wrapping clock. */
    uint32_t t0 = 0xFFFFFF00u;
    pair_remote_seen_add(&s, 0x7abc, PAIR_REMOTE_OPENED, 300, t0);
    CHECK(pair_remote_seen_find(&s, 0x7abc, t0, &rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_OPENED);
    CHECK_EQ(secs, 300);
    CHECK(pair_remote_seen_find(&s, 0x7abc, t0 + 185000u, &rc, &secs));
    CHECK(pair_remote_seen_find(&s, 0x7abc, t0 + PAIR_REMOTE_LIFETIME_MS - 1u, &rc, &secs));
    CHECK(!pair_remote_seen_find(&s, 0x7abc, t0 + PAIR_REMOTE_LIFETIME_MS, &rc, &secs));
    CHECK(!pair_remote_seen_find(&s, 0x7abd, t0, &rc, &secs));

    /* Open, close, then a late copy of the open: still the open's answer
     * (nothing reopened), the close's own copy gets the close's. */
    pair_remote_seen_add(&s, 0x7abd, PAIR_REMOTE_CLOSED, 0, t0 + 10u);
    CHECK(pair_remote_seen_find(&s, 0x7abc, t0 + 20u, &rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_OPENED);
    CHECK(pair_remote_seen_find(&s, 0x7abd, t0 + 20u, &rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_CLOSED);

    /* The same ID again after its lifetime: one entry, the new outcome. */
    pair_remote_seen_add(&s, 0x7abc, PAIR_REMOTE_WAS_OPEN, 12, t0 + PAIR_REMOTE_LIFETIME_MS);
    CHECK(pair_remote_seen_find(&s, 0x7abc, t0 + PAIR_REMOTE_LIFETIME_MS, &rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_WAS_OPEN);
    CHECK_EQ(secs, 12);
    unsigned used = 0;
    for (unsigned i = 0; i < PAIR_REMOTE_SEEN; i++) {
        used += s.e[i].used && s.e[i].mid == 0x7abc;
    }
    CHECK_EQ(used, 1);

    /* Full: the oldest goes first. */
    memset(&s, 0, sizeof s);
    for (unsigned i = 0; i < PAIR_REMOTE_SEEN + 2u; i++) {
        pair_remote_seen_add(&s, (uint16_t)(0x7000u + i), PAIR_REMOTE_CLOSED, 0, i);
    }
    CHECK(!pair_remote_seen_find(&s, 0x7000, 100, &rc, &secs));
    CHECK(!pair_remote_seen_find(&s, 0x7001, 100, &rc, &secs));
    for (unsigned i = 2; i < PAIR_REMOTE_SEEN + 2u; i++) {
        CHECK(pair_remote_seen_find(&s, (uint16_t)(0x7000u + i), 100, &rc, &secs));
    }
}

static void test_handover(void) {
    bool                open;
    uint32_t            wms, secs;
    enum pair_remote_rc rc;

    /* Empty: nothing to claim, withdraw or collect. */
    CHECK(!pair_remote_pending());
    CHECK(!pair_remote_claim(&open, &wms));
    CHECK(!pair_remote_withdraw());
    CHECK(!pair_remote_result(&rc, &secs));
    pair_remote_finish(PAIR_REMOTE_OPENED, 1); /* not claimed: no effect */
    CHECK(!pair_remote_result(&rc, &secs));

    /* Posted, claimed, done, collected. */
    CHECK(pair_remote_post(true, 300000u));
    CHECK(pair_remote_pending());
    CHECK(!pair_remote_post(false, 0)); /* one at a time */
    CHECK(!pair_remote_result(&rc, &secs));
    CHECK(pair_remote_claim(&open, &wms));
    CHECK(open);
    CHECK_EQ(wms, 300000);
    CHECK(!pair_remote_pending());
    CHECK(!pair_remote_claim(&open, &wms)); /* once */
    CHECK(!pair_remote_withdraw());          /* claimed: it runs to its end */
    CHECK(!pair_remote_post(false, 0));
    CHECK(!pair_remote_result(&rc, &secs));
    pair_remote_finish(PAIR_REMOTE_OPENED, 300);
    CHECK(!pair_remote_post(false, 0)); /* the outcome is not collected yet */
    CHECK(!pair_remote_claim(&open, &wms));
    CHECK(pair_remote_result(&rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_OPENED);
    CHECK_EQ(secs, 300);
    CHECK(!pair_remote_result(&rc, &secs)); /* once */

    /* Withdrawn before it was claimed: never runs. */
    CHECK(pair_remote_post(false, 0));
    CHECK(pair_remote_withdraw());
    CHECK(!pair_remote_pending());
    CHECK(!pair_remote_claim(&open, &wms));
    CHECK(!pair_remote_withdraw());
    pair_remote_finish(PAIR_REMOTE_CLOSED, 0);
    CHECK(!pair_remote_result(&rc, &secs));

    /* The slot is free again. */
    CHECK(pair_remote_post(false, 0));
    CHECK(pair_remote_claim(&open, &wms));
    CHECK(!open);
    pair_remote_finish(PAIR_REMOTE_WAS_CLOSED, 0);
    CHECK(pair_remote_result(&rc, &secs));
    CHECK_EQ(rc, PAIR_REMOTE_WAS_CLOSED);
}

int main(void) {
    test_tanoclo_bytes();
    test_paths();
    test_payloads();
    test_build_guards();
    test_codes();
    test_seen();
    test_handover();
    return TEST_REPORT("test_pair_remote");
}
