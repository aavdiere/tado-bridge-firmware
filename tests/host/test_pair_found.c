/* Host tests for src/pair_found.c: the joiner report (#41). The answer
 * bytes come from tanoclo's own codec (ws-server/lib/coap.js buildAck and
 * buildAckWithPayload on coap.parse(req_session) below, as handlePairFound()
 * calls them, lib/handlers.js:197-211), for MID 0x1234, token a1 b2 c3 d4
 * and a made-up device address fe80::21b:c500:102:304. tanoclo's
 * coap.parse() reads req_session as CON POST, options 11 "found" and 2048,
 * payload 05 10 <address>, and its path classifier routes "found" to
 * pair_found (lib/handlers/path-classifier.js:59). */
#include "pair_found.h"

#include "test.h"

#include <stdint.h>

static const uint8_t ipv6[16] = {
    0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x1b, 0xc5, 0x00, 0x01, 0x02, 0x03, 0x04};
static const uint8_t ipv6_b[16] = {
    0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x1b, 0xc5, 0x00, 0x09, 0x09, 0x09, 0x09};
static const uint8_t tok[4]     = {0xa1, 0xb2, 0xc3, 0xd4};
static const uint8_t session[8] = {1, 2, 3, 4, 5, 6, 7, 8};

#define PAYLOAD                                                                               \
    0xff, 0x05, 0x10, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1b, 0xc5, 0x00, \
        0x01, 0x02, 0x03, 0x04
static const uint8_t req_plain[] = {
    0x44, 0x02, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4, 0xb5, 'f', 'o', 'u', 'n', 'd', PAYLOAD};
static const uint8_t req_session[] = {0x44, 0x02, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4,   0xb5,
                                      'f',  'o',  'u',  'n',  'd',  0xe8, 0x06, 0xe8,   1,
                                      2,    3,    4,    5,    6,    7,    8,    PAYLOAD};
/* buildAck(m, CODE_VALID): no device row with a factory key. */
static const uint8_t ack_203[] = {0x64, 0x43, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
/* buildAck(m): the router's answer to a resent copy (coap-helpers.js sendCoAPAck). */
static const uint8_t ack_204[] = {0x64, 0x44, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
/* buildAckWithPayload(m, CODE_CONTENT, 06 10 <16 x 5a>): a factory key. */
static const uint8_t ack_205[] = {0x64, 0x45, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4, 0xff,
                                  0x06, 0x10, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
                                  0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a};

static void test_build(void) {
    uint8_t b[PAIR_FOUND_COAP_MAX + 8];
    size_t  n = pair_found_build(b, sizeof b, 0x1234, tok, NULL, ipv6);
    CHECK_EQ(n, sizeof req_plain);
    CHECK_MEM(b, req_plain, sizeof req_plain);
    n = pair_found_build(b, sizeof b, 0x1234, tok, session, ipv6);
    CHECK_EQ(n, sizeof req_session);
    CHECK_MEM(b, req_session, sizeof req_session);
    CHECK_EQ(n, PAIR_FOUND_COAP_MAX); /* the largest it gets */
    CHECK_EQ(pair_found_build(b, PAIR_FOUND_COAP_MAX - 1u, 0x1234, tok, session, ipv6), 0);

    /* Our own parser reads it back as tanoclo does. */
    struct coap_msg m;
    CHECK(coap_parse(req_session, sizeof req_session, &m));
    CHECK_EQ(m.type, COAP_CON);
    CHECK_EQ(m.code, COAP_POST);
    CHECK(coap_path_is(&m, "found"));
    CHECK_EQ(m.payload_len, PAIR_FOUND_PAYLOAD_LEN);
    CHECK_EQ(m.payload[0], PAIR_FOUND_TAG_IPV6);
    CHECK_EQ(m.payload[1], 16);
    CHECK_MEM(m.payload + 2, ipv6, 16);
}

static enum pair_found_answer answer_of(const uint8_t *b, size_t n, struct pair_found_tx *tx) {
    struct coap_msg m;
    if (!coap_parse(b, n, &m) || !pair_found_tx_match(tx, &m)) {
        return (enum pair_found_answer)99;
    }
    return pair_found_answer(&m);
}

static void test_answer(void) {
    struct pair_found_tx tx;
    pair_found_tx_start(&tx, 0x1234, tok, 0, 0);
    CHECK_EQ(answer_of(ack_203, sizeof ack_203, &tx), PAIR_FOUND_ANS_NO_KEY);
    CHECK_EQ(answer_of(ack_204, sizeof ack_204, &tx), PAIR_FOUND_ANS_NO_KEY);
    CHECK_EQ(answer_of(ack_205, sizeof ack_205, &tx), PAIR_FOUND_ANS_KEY);

    /* A key of another length, or under another tag, is not one. */
    uint8_t b[sizeof ack_205];
    memcpy(b, ack_205, sizeof b);
    b[10] = 0x0f;
    CHECK_EQ(answer_of(b, sizeof b, &tx), PAIR_FOUND_ANS_NO_KEY);
    memcpy(b, ack_205, sizeof b);
    b[9] = 0x05;
    CHECK_EQ(answer_of(b, sizeof b, &tx), PAIR_FOUND_ANS_NO_KEY);
    /* A value running past the end: not read. */
    CHECK_EQ(answer_of(ack_205, sizeof ack_205 - 1u, &tx), PAIR_FOUND_ANS_NO_KEY);

    /* Errors, an empty ACK and a Reset end it too. */
    static const uint8_t ack_404[] = {0x64, 0x84, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
    static const uint8_t ack_503[] = {0x64, 0xa3, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
    static const uint8_t empty[]   = {0x60, 0x00, 0x12, 0x34};
    static const uint8_t rst[]     = {0x70, 0x00, 0x12, 0x34};
    CHECK_EQ(answer_of(ack_404, sizeof ack_404, &tx), PAIR_FOUND_ANS_REFUSED);
    CHECK_EQ(answer_of(ack_503, sizeof ack_503, &tx), PAIR_FOUND_ANS_REFUSED);
    CHECK_EQ(answer_of(empty, sizeof empty, &tx), PAIR_FOUND_ANS_EMPTY);
    CHECK_EQ(answer_of(rst, sizeof rst, &tx), PAIR_FOUND_ANS_REFUSED);

    /* Not ours: another Message ID, another token, a request, or nothing
     * active. */
    static const uint8_t other_mid[] = {0x64, 0x43, 0x12, 0x35, 0xa1, 0xb2, 0xc3, 0xd4};
    static const uint8_t other_tok[] = {0x64, 0x43, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd5};
    static const uint8_t short_tok[] = {0x63, 0x43, 0x12, 0x34, 0xa1, 0xb2, 0xc3};
    static const uint8_t con[]       = {0x44, 0x02, 0x12, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
    struct coap_msg      m;
    CHECK(coap_parse(other_mid, sizeof other_mid, &m) && !pair_found_tx_match(&tx, &m));
    CHECK(coap_parse(other_tok, sizeof other_tok, &m) && !pair_found_tx_match(&tx, &m));
    CHECK(coap_parse(short_tok, sizeof short_tok, &m) && !pair_found_tx_match(&tx, &m));
    CHECK(coap_parse(con, sizeof con, &m) && !pair_found_tx_match(&tx, &m));
    tx.active = false;
    CHECK(coap_parse(ack_203, sizeof ack_203, &m) && !pair_found_tx_match(&tx, &m));
}

/* RFC 7252 4.2: sends at 0, T, 3T, 7T, 15T; given up at 31T. */
static void test_schedule(uint32_t t0, uint32_t rnd, uint32_t want_t) {
    struct pair_found_tx tx;
    pair_found_tx_start(&tx, 7, tok, t0, rnd);
    CHECK(tx.active);
    CHECK_EQ(tx.sends, 1);
    CHECK_EQ(tx.timeout_ms, want_t);
    static const uint32_t at[] = {1, 3, 7, 15};
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t when = t0 + at[i] * want_t;
        CHECK_EQ(pair_found_tx_due(&tx, when - 1u), PAIR_FOUND_WAIT);
        CHECK_EQ(pair_found_tx_due(&tx, when), PAIR_FOUND_RESEND);
        CHECK_EQ(tx.sends, i + 2u);
        CHECK_EQ(pair_found_tx_due(&tx, when), PAIR_FOUND_WAIT);
    }
    CHECK_EQ(pair_found_tx_due(&tx, t0 + 31u * want_t - 1u), PAIR_FOUND_WAIT);
    CHECK_EQ(pair_found_tx_due(&tx, t0 + 31u * want_t), PAIR_FOUND_GIVE_UP);
    CHECK(!tx.active);
    CHECK_EQ(tx.sends, PAIR_FOUND_SENDS);
    CHECK_EQ(pair_found_tx_due(&tx, t0 + 100u * want_t), PAIR_FOUND_WAIT);
}

static void test_retransmit(void) {
    test_schedule(1000, 0, 2000);
    test_schedule(1000, 1000, 3000);
    test_schedule(1000, 1001, 2000);              /* rnd % 1001 */
    test_schedule(UINT32_MAX - 5000u, 500, 2500); /* across the clock's wrap */
    /* The longest: given up 93 s after the first send (MAX_TRANSMIT_WAIT). */
    CHECK_EQ(31u * 3000u, 93000u);
}

static void test_queue(void) {
    uint8_t got[16];
    CHECK(!pair_found_peek(got));
    pair_found_pop(); /* nothing to pop: no effect */
    CHECK(!pair_found_peek(got));

    CHECK_EQ(pair_found_post(ipv6), PAIR_FOUND_QUEUED);
    CHECK(pair_found_peek(got));
    CHECK_MEM(got, ipv6, 16);
    /* The same device again while its report is the one being sent: a
     * second report (it paired again); once more: already waiting. */
    CHECK_EQ(pair_found_post(ipv6), PAIR_FOUND_QUEUED);
    CHECK_EQ(pair_found_post(ipv6), PAIR_FOUND_ALREADY);
    CHECK_EQ(pair_found_post(ipv6_b), PAIR_FOUND_QUEUED);
    uint8_t c[16];
    memcpy(c, ipv6_b, 16);
    c[15] = 0x0a;
    CHECK_EQ(pair_found_post(c), PAIR_FOUND_QUEUED);
    /* Full at PAIR_FOUND_QUEUE: counted, nothing replaced. */
    c[15] = 0x0b;
    CHECK_EQ(pair_found_full_count(), 0);
    CHECK_EQ(pair_found_post(c), PAIR_FOUND_FULL);
    CHECK_EQ(pair_found_full_count(), 1);
    CHECK(pair_found_peek(got));
    CHECK_MEM(got, ipv6, 16); /* still the first */

    /* In order, each until popped. */
    pair_found_pop();
    CHECK(pair_found_peek(got) && memcmp(got, ipv6, 16) == 0);
    pair_found_pop();
    CHECK(pair_found_peek(got) && memcmp(got, ipv6_b, 16) == 0);
    pair_found_pop();
    CHECK(pair_found_peek(got) && got[15] == 0x0a);
    pair_found_pop();
    CHECK(!pair_found_peek(got));

    /* Many rounds: the counters run on, the ring wraps. */
    for (unsigned i = 0; i < 1000u; i++) {
        c[14] = (uint8_t)i;
        CHECK_EQ(pair_found_post(c), PAIR_FOUND_QUEUED);
        CHECK(pair_found_peek(got) && got[14] == (uint8_t)i);
        pair_found_pop();
    }
    CHECK(!pair_found_peek(got));
    CHECK_EQ(pair_found_full_count(), 1);
}

int main(void) {
    test_build();
    test_answer();
    test_retransmit();
    test_queue();
    return TEST_REPORT("test_pair_found");
}
