/* The server's remote pairing trigger (#40). See pair_remote.h. */
#include "pair_remote.h"

#include "pair.h"

#include <string.h>

bool pair_remote_path(const struct coap_msg *m) {
    static const char *const want[4] = {"d", "I", NULL, "pair"};
    size_t                   pos     = 0, vlen;
    unsigned                 num = 0, seg = 0;
    const uint8_t           *val;
    while (coap_next_option(m, &pos, &num, &val, &vlen)) {
        if (num != COAP_OPT_URI_PATH) {
            continue;
        }
        if (seg >= 4u) {
            return false;
        }
        const char *w = want[seg++];
        if (w && (vlen != strlen(w) || memcmp(val, w, vlen) != 0)) {
            return false;
        }
    }
    return seg == 4u;
}

/* The third segment, which pair_remote_path() has let through. */
static bool serial_segment(const struct coap_msg *m, const uint8_t **val, size_t *vlen) {
    size_t   pos = 0;
    unsigned num = 0, seg = 0;
    while (coap_next_option(m, &pos, &num, val, vlen)) {
        if (num == COAP_OPT_URI_PATH && seg++ == 2u) {
            return true;
        }
    }
    return false;
}

/* tanoclo: `IB0000FT0100${deviceId.replace(/^IB/, '')}` (device.js:383). */
static bool ours(const struct coap_msg *m, const char *serial) {
    const uint8_t *v;
    size_t         vl;
    if (!serial || !*serial || !serial_segment(m, &v, &vl)) {
        return false;
    }
    const char *digits = strncmp(serial, "IB", 2) == 0 ? serial + 2 : serial;
    size_t      pl = strlen(PAIR_REMOTE_PREFIX), dl = strlen(digits);
    return vl == pl + dl && memcmp(v, PAIR_REMOTE_PREFIX, pl) == 0
           && memcmp(v + pl, digits, dl) == 0;
}

/* One-byte tag, one-byte length, value; exactly one duration of 4 bytes. */
static bool duration(const uint8_t *p, size_t n, uint32_t *dur_s) {
    bool   have = false;
    size_t i    = 0;
    while (i < n) {
        if (n - i < 2u || n - i - 2u < p[i + 1u]) {
            return false; /* header or value runs past the end */
        }
        uint8_t        tag = p[i], len = p[i + 1u];
        const uint8_t *v = p + i + 2u;
        if (tag == PAIR_REMOTE_TAG_DUR) {
            if (have || len != 4u) {
                return false;
            }
            *dur_s = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
            have   = true;
        }
        i += 2u + len;
    }
    return have;
}

enum pair_remote_rc pair_remote_check(const struct coap_msg *m,
                                      const char            *serial,
                                      bool                   have_target,
                                      bool                   tx_allowed,
                                      bool                  *open,
                                      uint32_t              *dur_s) {
    *open  = false;
    *dur_s = 0;
    if (!ours(m, serial)) {
        return PAIR_REMOTE_NOT_OURS;
    }
    if (m->code != COAP_POST) {
        return PAIR_REMOTE_METHOD;
    }
    uint32_t d = 0;
    if (!duration(m->payload, m->payload_len, &d)) {
        return PAIR_REMOTE_MALFORMED;
    }
    *dur_s = d;
    if (d > 0u && !have_target) {
        return PAIR_REMOTE_NO_TARGET;
    }
    if (d > 0u && !tx_allowed) {
        return PAIR_REMOTE_RX_ONLY;
    }
    *open = d > 0u;
    return PAIR_REMOTE_FORWARD;
}

uint32_t pair_remote_window_ms(uint32_t dur_s) {
    return dur_s < PAIR_WINDOW_MS / 1000u ? dur_s * 1000u : PAIR_WINDOW_MS;
}

static const struct {
    uint8_t     code;
    const char *text;
} rcs[PAIR_REMOTE_RC_COUNT] = {
    [PAIR_REMOTE_FORWARD]    = {COAP_INTERNAL, "for the radio task"},
    [PAIR_REMOTE_OPENED]     = {COAP_CHANGED, "window opened"},
    [PAIR_REMOTE_WAS_OPEN]   = {COAP_CHANGED, "window already open, left as it is"},
    [PAIR_REMOTE_CLOSED]     = {COAP_CHANGED, "window closed"},
    [PAIR_REMOTE_WAS_CLOSED] = {COAP_CHANGED, "no window was open"},
    [PAIR_REMOTE_NOT_OURS]   = {COAP_NOT_FOUND, "the path names another bridge's serial"},
    [PAIR_REMOTE_METHOD]     = {COAP_CODE(4, 5), "not a POST"},
    [PAIR_REMOTE_MALFORMED] = {COAP_BAD_REQUEST,
                               "malformed payload (one TLV 0x01 of 4 bytes, the duration, needed)"},
    [PAIR_REMOTE_NO_TARGET] =
        {COAP_CODE(5, 1), "no device named for pairing (PAIR_TARGET_EUI64 in config/local.cmake)"},
    [PAIR_REMOTE_RX_ONLY]     = {COAP_CODE(5, 1), "receive-only image (RADIO_TX_ALLOWED 0)"},
    [PAIR_REMOTE_SELF_TARGET] = {COAP_CODE(5, 1), "PAIR_TARGET_EUI64 is the bridge's own EUI-64"},
    [PAIR_REMOTE_NO_FLASH]    = {COAP_UNAVAILABLE, "flash not ready at boot"},
    [PAIR_REMOTE_NO_IDENTITY] = {COAP_UNAVAILABLE, "no identity"},
    [PAIR_REMOTE_RADIO]       = {COAP_UNAVAILABLE,
                                 "radio not ready to transmit (no PLL lock or interrupts off)"},
    [PAIR_REMOTE_QUEUED]  = {COAP_UNAVAILABLE,
                             "frames of the last window still queued; try again in a few seconds"},
    [PAIR_REMOTE_NETWORK] = {COAP_UNAVAILABLE,
                             "a network record exists but is not in use, no new network over it"},
    [PAIR_REMOTE_OPEN_FAILED] = {COAP_UNAVAILABLE,
                                 "window not opened (key not read back, no random numbers, or "
                                 "target not a device)"},
    [PAIR_REMOTE_NO_RADIO]    = {COAP_UNAVAILABLE, "the radio task did not take it in time"},
};

uint8_t pair_remote_code(enum pair_remote_rc rc) {
    return (unsigned)rc < PAIR_REMOTE_RC_COUNT ? rcs[rc].code : COAP_INTERNAL;
}

const char *pair_remote_text(enum pair_remote_rc rc) {
    return (unsigned)rc < PAIR_REMOTE_RC_COUNT ? rcs[rc].text : "?";
}

/* --- Duplicates ---------------------------------------------------------------- */
void pair_remote_key_of(const struct coap_msg *m, uint32_t dur_s, struct pair_remote_key *k) {
    memset(k, 0, sizeof *k);
    k->mid   = m->mid;
    k->tkl   = m->tkl <= sizeof k->token ? m->tkl : (uint8_t)sizeof k->token;
    k->dur_s = dur_s;
    memcpy(k->token, m->token, k->tkl);
}

bool pair_remote_same(const struct pair_remote_key *a, const struct pair_remote_key *b) {
    return a->mid == b->mid && a->tkl == b->tkl && a->dur_s == b->dur_s
           && memcmp(a->token, b->token, a->tkl) == 0;
}

bool pair_remote_seen_find(const struct pair_remote_seen *s,
                           const struct pair_remote_key  *k,
                           uint32_t                       now_ms,
                           enum pair_remote_rc           *rc,
                           uint32_t                      *secs) {
    for (unsigned i = 0; i < PAIR_REMOTE_SEEN; i++) {
        if (s->e[i].used && pair_remote_same(&s->e[i].k, k)
            && (uint32_t)(now_ms - s->e[i].at_ms) < PAIR_REMOTE_LIFETIME_MS) {
            *rc   = (enum pair_remote_rc)s->e[i].rc;
            *secs = s->e[i].secs;
            return true;
        }
    }
    return false;
}

void pair_remote_seen_add(struct pair_remote_seen      *s,
                          const struct pair_remote_key *k,
                          enum pair_remote_rc           rc,
                          uint32_t                      secs,
                          uint32_t                      now_ms) {
    unsigned i = 0;
    while (i < PAIR_REMOTE_SEEN && !(s->e[i].used && s->e[i].k.mid == k->mid)) {
        i++; /* the same ID again: one entry, the newest request with it */
    }
    if (i == PAIR_REMOTE_SEEN) {
        i       = s->next % PAIR_REMOTE_SEEN;
        s->next = (i + 1u) % PAIR_REMOTE_SEEN;
    }
    s->e[i].used  = true;
    s->e[i].k     = *k;
    s->e[i].rc    = (uint8_t)rc;
    s->e[i].secs  = secs;
    s->e[i].at_ms = now_ms;
}

enum pair_remote_sort pair_remote_sort(const struct pair_remote_seen *s,
                                       const struct pair_remote_key  *in_work,
                                       const struct pair_remote_key  *k,
                                       uint32_t                       now_ms,
                                       enum pair_remote_rc           *rc,
                                       uint32_t                      *secs) {
    if (in_work && pair_remote_same(k, in_work)) {
        return PAIR_REMOTE_COPY_IN_WORK;
    }
    if (pair_remote_seen_find(s, k, now_ms, rc, secs)) {
        return PAIR_REMOTE_COPY_DONE;
    }
    return in_work ? PAIR_REMOTE_WAIT : PAIR_REMOTE_NEW;
}

/* --- Hand-over ------------------------------------------------------------------ */
/* LDREX/STREX on the Cortex-M4 (see src/handoff.c); the same builtins on
 * the host. The request and the outcome are plain fields: each is written
 * only by the side that owns the slot in that state, before the release
 * that passes the slot on. */
enum { EMPTY, POSTED, CLAIMED, DONE };

static uint32_t slot;
static bool     req_open;
static uint32_t req_window_ms;
static uint8_t  res_rc;
static uint32_t res_secs;

static bool cas(uint32_t want, uint32_t to) {
    return __atomic_compare_exchange_n(&slot, &want, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint32_t state(void) {
    return __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
}

bool pair_remote_post(bool open, uint32_t window_ms) {
    if (state() != EMPTY) {
        return false;
    }
    req_open      = open;
    req_window_ms = window_ms;
    __atomic_store_n(&slot, (uint32_t)POSTED, __ATOMIC_RELEASE);
    return true;
}

bool pair_remote_withdraw(void) {
    return cas(POSTED, EMPTY);
}

bool pair_remote_result(enum pair_remote_rc *rc, uint32_t *secs) {
    if (state() != DONE) {
        return false;
    }
    *rc   = (enum pair_remote_rc)res_rc;
    *secs = res_secs;
    __atomic_store_n(&slot, (uint32_t)EMPTY, __ATOMIC_RELEASE);
    return true;
}

bool pair_remote_pending(void) {
    return state() == POSTED;
}

bool pair_remote_claim(bool *open, uint32_t *window_ms) {
    if (!cas(POSTED, CLAIMED)) {
        return false;
    }
    *open      = req_open;
    *window_ms = req_window_ms;
    return true;
}

void pair_remote_finish(enum pair_remote_rc rc, uint32_t secs) {
    if (state() != CLAIMED) {
        return;
    }
    res_rc   = (uint8_t)rc;
    res_secs = secs;
    __atomic_store_n(&slot, (uint32_t)DONE, __ATOMIC_RELEASE);
}
