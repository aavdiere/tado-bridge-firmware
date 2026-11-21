#ifndef PAIR_FOUND_H
#define PAIR_FOUND_H

/* The joiner report (#41; owner decision 2026-11-20, docs/open-questions.md
 * row 28a): after a device has taken the network key (the target's 2.04
 * to d/pair, LINK_RX_PAIRED, src/pair.h), the bridge tells the server with
 * CON POST found, payload 05 10 <the device's IPv6 address>, so that the
 * device gets its `devices` row without manual work. Hardware independent:
 * the queue from the radio task to the net task, the request's bytes, the
 * reading of the answer and the retransmission schedule
 * (tests/host/test_pair_found.c). src/link.c posts, src/session.c sends.
 *
 * What tanoclo does with it (docs/server-api.md "Joiner report"):
 * tanoclo/ws-server/lib/handlers/path-classifier.js:59 routes any path with
 * a segment "found" (and none of the keywords tested before it) to
 * handlePairFound() (lib/handlers.js:158-212), which takes the 16 bytes
 * after the first 05 10 in the payload as the joiner's address and answers
 * with a piggybacked ACK: 2.05 with 06 10 <16-byte factory key> if a
 * `devices` row has that address and a factory key, else 2.03 [D]. The
 * stock bridge asks this before d/pair, to wrap the network key in the
 * factory key (TLV 0x07, tanoclo/docs/pairing.md:11-14, 41-42) [I]; we
 * hand the key over in plaintext (TLV 0x12) and send the report after the
 * device has paired, so a factory key in the answer is not used (and never
 * logged). Upstream tanoclo creates no row from the report; the local
 * patch bench/found-register in ../tanoclo does (lib/joiner-registry.js):
 * the device's own auth request from that address, with its serial, then
 * creates the row.
 *
 * The address is the one the device's uplinks carry in the frame header
 * (src/relay.c, relay_uplink(): link-local, from the 2.04's own source),
 * which is the one tanoclo keys the device by.
 *
 * Not kept across a restart: the queue is RAM only, as the window and its
 * copy of the network key are. A report lost to a restart before it was
 * answered means the device is registered by hand, as before #41. */

#include "coap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAIR_FOUND_URI         "found"
#define PAIR_FOUND_TAG_IPV6    0x05u /* tanoclo lib/handlers.js:177-185 */
#define PAIR_FOUND_TAG_KEY     0x06u /* tanoclo lib/handlers.js:197-203 */
#define PAIR_FOUND_PAYLOAD_LEN 18u
#define PAIR_FOUND_TOKEN_LEN   4u
#define PAIR_FOUND_SESSION_LEN 8u
/* Header 4, token 4, Uri-Path "found" 6, option 2048 of 8 bytes 11,
 * marker 1, payload 18. */
#define PAIR_FOUND_COAP_MAX 44u

/* --- Queue, radio task (poster) to net task (sender) ------------------------ */
/* Single producer, single consumer, no lock and no critical section: the
 * radio task's ACK interrupt is never masked, and neither side waits. */
#define PAIR_FOUND_QUEUE 4u
_Static_assert((PAIR_FOUND_QUEUE & (PAIR_FOUND_QUEUE - 1u)) == 0u, "a power of two");

enum pair_found_post {
    PAIR_FOUND_QUEUED,
    PAIR_FOUND_ALREADY, /* the same address waits behind the report being sent */
    PAIR_FOUND_FULL,    /* refused, counted (pair_found_full_count()) */
};
/* Radio task. A device at ipv6 has paired. An address already waiting is
 * not queued twice; the report in flight does not count, so a device that
 * pairs again while its first report is being sent is reported again. */
enum pair_found_post pair_found_post(const uint8_t ipv6[16]);
/* Net task. The oldest report: it stays first until pair_found_pop(). */
bool pair_found_peek(uint8_t ipv6[16]);
void pair_found_pop(void);
/* Any task. Reports refused because the queue was full, since boot. */
uint32_t pair_found_full_count(void);

/* --- The request and its answer --------------------------------------------- */
/* CON POST found, token (PAIR_FOUND_TOKEN_LEN bytes), the session token as
 * option 2048 if session is not NULL (as the bridge's other requests,
 * src/session.c request()), payload 05 10 ipv6. Returns its length, or 0
 * if cap is too small. */
size_t pair_found_build(uint8_t       *out,
                        size_t         cap,
                        uint16_t       mid,
                        const uint8_t  token[PAIR_FOUND_TOKEN_LEN],
                        const uint8_t *session,
                        const uint8_t  ipv6[16]);

enum pair_found_answer {
    PAIR_FOUND_ANS_KEY,     /* 2.xx with 06 10 <16 bytes>: tanoclo has a factory key for it */
    PAIR_FOUND_ANS_NO_KEY,  /* 2.xx without it (tanoclo: 2.03; a resent copy: 2.04) */
    PAIR_FOUND_ANS_EMPTY,   /* an empty ACK (code 0.00) */
    PAIR_FOUND_ANS_REFUSED, /* 4.xx, 5.xx, or a Reset */
};
/* What an answer that pair_found_tx_match() took says. */
enum pair_found_answer pair_found_answer(const struct coap_msg *m);

/* --- Retransmission (RFC 7252 4.2, 4.8) -------------------------------------- */
/* ACK_TIMEOUT 2 s, ACK_RANDOM_FACTOR 1.5, MAX_RETRANSMIT 4: sends at 0, T,
 * 3T, 7T and 15T, given up at 31T, T 2-3 s (62-93 s, MAX_TRANSMIT_WAIT). */
#define PAIR_FOUND_ACK_TIMEOUT_MS 2000u
#define PAIR_FOUND_SENDS          5u

struct pair_found_tx {
    bool     active;
    uint16_t mid;
    uint8_t  token[PAIR_FOUND_TOKEN_LEN];
    uint8_t  sends; /* sent so far, the first included */
    uint32_t first_ms, last_ms, timeout_ms;
};

/* The first send of a request with mid and token went (or was tried) at
 * now_ms; rnd: a random number for the first timeout. */
void pair_found_tx_start(struct pair_found_tx *tx,
                         uint16_t              mid,
                         const uint8_t         token[PAIR_FOUND_TOKEN_LEN],
                         uint32_t              now_ms,
                         uint32_t              rnd);

enum pair_found_due {
    PAIR_FOUND_WAIT,    /* nothing to do yet (or no request active) */
    PAIR_FOUND_RESEND,  /* send the same bytes again now (counted) */
    PAIR_FOUND_GIVE_UP, /* PAIR_FOUND_SENDS sent, the last timed out: tx is no longer active */
};
enum pair_found_due pair_found_tx_due(struct pair_found_tx *tx, uint32_t now_ms);

/* m answers the active request: an ACK with its Message ID and token, or
 * an empty ACK or a Reset with its Message ID (they carry no token). */
bool pair_found_tx_match(const struct pair_found_tx *tx, const struct coap_msg *m);

#endif
