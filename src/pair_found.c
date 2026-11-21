/* The joiner report (#41). See pair_found.h. */
#include "pair_found.h"

#include <string.h>

/* --- Queue ---------------------------------------------------------------------- */
/* head is written by the poster only, tail by the sender only; a slot is
 * written by the poster only, before the release of head that hands it
 * over, and not again until the sender's release of tail gives it back.
 * The counters run freely: PAIR_FOUND_QUEUE divides 2^32. */
static uint8_t  q[PAIR_FOUND_QUEUE][16];
static uint32_t head, tail, full;

enum pair_found_post pair_found_post(const uint8_t ipv6[16]) {
    uint32_t h = head;
    uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
    /* Slot t is the report being sent (or about to be): not compared. A
     * tail that has moved on meanwhile only means one slot less compared. */
    for (uint32_t i = t + 1u; i - t < h - t; i++) {
        if (memcmp(q[i % PAIR_FOUND_QUEUE], ipv6, 16) == 0) {
            return PAIR_FOUND_ALREADY;
        }
    }
    if (h - t >= PAIR_FOUND_QUEUE) {
        __atomic_store_n(&full, full + 1u, __ATOMIC_RELAXED);
        return PAIR_FOUND_FULL;
    }
    memcpy(q[h % PAIR_FOUND_QUEUE], ipv6, 16);
    __atomic_store_n(&head, h + 1u, __ATOMIC_RELEASE);
    return PAIR_FOUND_QUEUED;
}

bool pair_found_peek(uint8_t ipv6[16]) {
    uint32_t t = tail;
    if (__atomic_load_n(&head, __ATOMIC_ACQUIRE) == t) {
        return false;
    }
    memcpy(ipv6, q[t % PAIR_FOUND_QUEUE], 16);
    return true;
}

void pair_found_pop(void) {
    uint32_t t = tail;
    if (__atomic_load_n(&head, __ATOMIC_ACQUIRE) != t) {
        __atomic_store_n(&tail, t + 1u, __ATOMIC_RELEASE);
    }
}

uint32_t pair_found_full_count(void) {
    return __atomic_load_n(&full, __ATOMIC_RELAXED);
}

/* --- The request and its answer ---------------------------------------------- */
size_t pair_found_build(uint8_t       *out,
                        size_t         cap,
                        uint16_t       mid,
                        const uint8_t  token[PAIR_FOUND_TOKEN_LEN],
                        const uint8_t *session,
                        const uint8_t  ipv6[16]) {
    uint8_t p[PAIR_FOUND_PAYLOAD_LEN];
    p[0] = PAIR_FOUND_TAG_IPV6;
    p[1] = 16u;
    memcpy(p + 2, ipv6, 16);
    struct coap_writer w;
    coap_begin(&w, out, cap, COAP_CON, COAP_POST, mid, token, PAIR_FOUND_TOKEN_LEN);
    coap_option(&w, COAP_OPT_URI_PATH, PAIR_FOUND_URI, strlen(PAIR_FOUND_URI));
    if (session) {
        coap_option(&w, COAP_OPT_SESSION, session, PAIR_FOUND_SESSION_LEN);
    }
    coap_payload(&w, p, sizeof p);
    return w.overflow ? 0u : w.len;
}

/* One-byte tag, one-byte length, value, as tanoclo writes it
 * (handlers.js:200-203): a value of 16 bytes under tag 0x06. */
static bool has_key(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (n - i >= 2u && n - i - 2u >= p[i + 1u]) {
        if (p[i] == PAIR_FOUND_TAG_KEY && p[i + 1u] == 16u) {
            return true;
        }
        i += 2u + p[i + 1u];
    }
    return false;
}

enum pair_found_answer pair_found_answer(const struct coap_msg *m) {
    if (m->type == COAP_RST || (m->code >> 5) >= 4u) {
        return PAIR_FOUND_ANS_REFUSED;
    }
    if (m->code == 0u) {
        return PAIR_FOUND_ANS_EMPTY;
    }
    if ((m->code >> 5) == 2u && has_key(m->payload, m->payload_len)) {
        return PAIR_FOUND_ANS_KEY;
    }
    return (m->code >> 5) == 2u ? PAIR_FOUND_ANS_NO_KEY : PAIR_FOUND_ANS_REFUSED;
}

/* --- Retransmission --------------------------------------------------------------- */
void pair_found_tx_start(struct pair_found_tx *tx,
                         uint16_t              mid,
                         const uint8_t         token[PAIR_FOUND_TOKEN_LEN],
                         uint32_t              now_ms,
                         uint32_t              rnd) {
    tx->active = true;
    tx->mid    = mid;
    memcpy(tx->token, token, PAIR_FOUND_TOKEN_LEN);
    tx->sends    = 1u;
    tx->first_ms = now_ms;
    tx->last_ms  = now_ms;
    /* ACK_TIMEOUT to ACK_TIMEOUT * ACK_RANDOM_FACTOR (RFC 7252 4.2). */
    tx->timeout_ms = PAIR_FOUND_ACK_TIMEOUT_MS + rnd % (PAIR_FOUND_ACK_TIMEOUT_MS / 2u + 1u);
}

enum pair_found_due pair_found_tx_due(struct pair_found_tx *tx, uint32_t now_ms) {
    if (!tx->active || now_ms - tx->last_ms < tx->timeout_ms) {
        return PAIR_FOUND_WAIT;
    }
    if (tx->sends >= PAIR_FOUND_SENDS) {
        tx->active = false;
        return PAIR_FOUND_GIVE_UP;
    }
    tx->sends++;
    tx->last_ms = now_ms;
    tx->timeout_ms *= 2u;
    return PAIR_FOUND_RESEND;
}

bool pair_found_tx_match(const struct pair_found_tx *tx, const struct coap_msg *m) {
    if (!tx->active || m->mid != tx->mid) {
        return false;
    }
    if (m->type == COAP_RST || (m->type == COAP_ACK && m->code == 0u && m->tkl == 0u)) {
        return true; /* a Reset, or an empty ACK: neither carries a token */
    }
    return m->type == COAP_ACK && m->tkl == PAIR_FOUND_TOKEN_LEN
           && memcmp(m->token, tx->token, PAIR_FOUND_TOKEN_LEN) == 0;
}
