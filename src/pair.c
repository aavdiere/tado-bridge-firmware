/* Pairing (#19). See pair.h.
 *
 * Ported from freedo: the Beacon Request check and the Enhanced Beacon of
 * tado/beacon.go and firmware/cc1101_bridge/cc1101_bridge.ino:923-974
 * (pairAnswer), the d/pair message of tado/build_resources.go:453-485
 * (BuildPairCoAP), and the window of tado/pair_window.go.
 * Copyright 2026 The freedo contributors. Licensed under the Apache License,
 * Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0.
 *
 * Changes for this project:
 * - C, no allocation; the CCM* is src/ccm.c through src/mac.c.
 * - The window names its target when it opens and answers no other device
 *   (docs/test-bench.md rule 5); freedo records every joiner and lets the
 *   operator pick one.
 * - The bridge is on its own: d/pair is sent by the window itself, a few
 *   seconds after the target has taken the beacon, not on an operator's
 *   request, and always under the pairing key.
 * - The window counts what it sends (beacons, advertisement pairs, d/pair
 *   datagrams) and stops at fixed limits (pair.h). */
#include "pair.h"

#include <string.h>

const uint8_t pair_static_key[CCM_KEY_LEN] = {
    't',
    'a',
    'd',
    'o',
    ' ',
    'p',
    'a',
    'i',
    'r',
    'i',
    'n',
    'g',
    ' ',
    'k',
    'e',
    'y',
};

#define SEC_CTRL            0x04u /* freedo tado/beacon.go:24 (bcastSecCtrl) */
#define MAC_CMD_BEACON_REQ  0x07u
#define REQ_BODY_LEN        5u /* counter(4) command(1), without the FCS */
#define COAP_ACK            2u
#define COAP_CHANGED        0x44u /* 2.04 */
#define COAP_SESSION_OPTION 2048u
#define MID_FIRST           0x4000u
#define MID_SPAN            0x2000u

static struct {
    bool           open, have_target, replied;
    uint32_t       opened_ms;
    uint8_t        target[MAC_EUI_LEN];
    uint8_t        net_key[CCM_KEY_LEN];
    struct ccm_key key; /* the pairing key's schedule */
    uint8_t        rnd[PAIR_RAND_LEN];
    uint16_t       mid_base;

    unsigned beacons, ra_pairs;
    /* d/pair: due at due_ms once armed; one datagram in flight at most. */
    bool     armed, in_flight;
    uint32_t due_ms;
    unsigned dpair_sent;
} w;

static struct pair_stats stats;

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

void pair_init(void) {
    pair_close();
    w.have_target = false;
    w.dpair_sent  = 0;
    w.in_flight   = false;
}

bool pair_open(const uint8_t target[MAC_EUI_LEN],
               const uint8_t net_key[CCM_KEY_LEN],
               const uint8_t rnd[PAIR_RAND_LEN],
               uint32_t      now_ms) {
    pair_init();
    static const uint8_t zero[MAC_EUI_LEN];
    if (memcmp(target + 5, mac_eui_suffix, 3) != 0 || memcmp(target, zero, 5) == 0) {
        return false;
    }
    ccm_set_key(&w.key, pair_static_key);
    if (!w.key.set) {
        pair_close();
        return false;
    }
    memcpy(w.target, target, MAC_EUI_LEN);
    memcpy(w.net_key, net_key, CCM_KEY_LEN);
    memcpy(w.rnd, rnd, PAIR_RAND_LEN);
    /* Below the server's 0x7000-0xFFFF (tanoclo ws-server/lib/coap-transport.js:39-44),
     * so an answer to the server's request is never taken for ours. */
    w.mid_base    = (uint16_t)(MID_FIRST + ((rnd[0] << 8 | rnd[1]) % MID_SPAN));
    w.replied     = false;
    w.have_target = true;
    w.open        = true;
    w.opened_ms   = now_ms;
    stats.windows++;
    return true;
}

void pair_close(void) {
    ccm_clear_key(&w.key);
    wipe(w.net_key, sizeof w.net_key);
    wipe(w.rnd, sizeof w.rnd);
    w.open     = false;
    w.armed    = false;
    w.beacons  = 0;
    w.ra_pairs = 0;
}

bool pair_active(uint32_t now_ms) {
    if (w.open && (uint32_t)(now_ms - w.opened_ms) >= PAIR_WINDOW_MS) {
        pair_close();
    }
    return w.open;
}

const struct ccm_key *pair_key(void) {
    return &w.key;
}

bool pair_is_target(const uint8_t wire[MAC_EUI_LEN]) {
    return w.open && memcmp(wire, w.target, MAC_EUI_LEN) == 0;
}

bool pair_target(uint8_t wire[MAC_EUI_LEN]) {
    if (w.have_target) {
        memcpy(wire, w.target, MAC_EUI_LEN);
    }
    return w.have_target;
}

/* d/pair becomes due at `at`, unless one is in flight, all were sent, or
 * one is due sooner already. */
static void arm(uint32_t at) {
    if (w.in_flight || w.dpair_sent >= PAIR_DPAIR_MAX) {
        return;
    }
    if (!w.armed || (int32_t)(at - w.due_ms) < 0) {
        w.due_ms = at;
        w.armed  = true;
    }
}

/* --- Enhanced Beacon -------------------------------------------------------- */
static enum pair_req check_request(const uint8_t *req, size_t len, uint32_t now_ms) {
    static const uint8_t bcast[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    if (!pair_active(now_ms)) {
        return PAIR_REQ_CLOSED;
    }
    if (!req || len != PAIR_REQUEST_LEN || mac_classify(req, len) != MAC_BEACON_REQUEST
        || memcmp(req + 3, bcast, sizeof bcast) != 0 || req[15] != SEC_CTRL) {
        return PAIR_REQ_MALFORMED;
    }
    /* The address first: nothing from another device is decrypted. */
    if (memcmp(req + 7, w.target, MAC_EUI_LEN) != 0) {
        return PAIR_REQ_NOT_TARGET;
    }
    if (w.beacons >= PAIR_BEACONS_MAX) {
        return PAIR_REQ_LIMIT;
    }
    uint8_t body[REQ_BODY_LEN + MAC_FCS_LEN];
    size_t  n = 0;
    bool ok = mac_open(&w.key, req, len, body, sizeof body, &n) == MAC_OPEN_OK && n == REQ_BODY_LEN;
    uint8_t cmd = body[4];
    wipe(body, sizeof body);
    if (!ok) {
        return PAIR_REQ_MIC;
    }
    if (cmd != MAC_CMD_BEACON_REQ) {
        return PAIR_REQ_NOT_BEACON_REQ;
    }
    w.beacons++;
    arm(now_ms + PAIR_DPAIR_FALLBACK_MS);
    return PAIR_REQ_OK;
}

enum pair_req pair_check_request(const uint8_t *req, size_t len, uint32_t now_ms) {
    enum pair_req r = check_request(req, len, now_ms);
    stats.req[r]++;
    return r;
}

size_t pair_build_beacon(uint8_t       out[PAIR_BEACON_LEN],
                         uint8_t       seq,
                         const uint8_t dst[MAC_EUI_LEN],
                         const uint8_t own[MAC_EUI_LEN],
                         uint32_t      counter) {
    if (!w.open) {
        return 0;
    }
    uint8_t hdr[MAC_HDR_LEN] = {0x18, 0xEC, seq, (uint8_t)MAC_PAN_ID, (uint8_t)(MAC_PAN_ID >> 8)};
    memcpy(hdr + 5, dst, MAC_EUI_LEN);
    memcpy(hdr + 13, own, 3);
    uint8_t body[10];
    memcpy(body, own + 3, 5);
    body[5]  = SEC_CTRL;
    body[6]  = (uint8_t)counter;
    body[7]  = (uint8_t)(counter >> 8);
    body[8]  = (uint8_t)(counter >> 16);
    body[9]  = (uint8_t)(counter >> 24);
    size_t n = mac_seal(&w.key, hdr, body, sizeof body, out, PAIR_BEACON_LEN);
    return n == PAIR_BEACON_LEN ? n : 0;
}

/* --- The target's other frames ---------------------------------------------- */
void pair_note_joiner(uint32_t now_ms) {
    if (!pair_active(now_ms)) {
        return;
    }
    stats.joiner_frames++;
    arm(now_ms + PAIR_DPAIR_DELAY_MS);
}

bool pair_ra_pairs_left(uint32_t now_ms) {
    return pair_active(now_ms) && w.ra_pairs < PAIR_RA_PAIRS_MAX;
}

bool pair_take_ra_pair(uint32_t now_ms) {
    if (!pair_ra_pairs_left(now_ms)) {
        return false;
    }
    w.ra_pairs++;
    stats.ra_pairs++;
    return true;
}

/* --- d/pair ------------------------------------------------------------------- */
size_t pair_build_coap(uint8_t      *out,
                       size_t        cap,
                       uint16_t      mid,
                       const uint8_t session[PAIR_SESSION_LEN],
                       const uint8_t key[CCM_KEY_LEN]) {
    if (cap < PAIR_COAP_LEN) {
        return 0;
    }
    uint8_t *p = out;
    *p++       = 0x40; /* version 1, CON, no token */
    *p++       = 0x02; /* POST */
    *p++       = (uint8_t)(mid >> 8);
    *p++       = (uint8_t)mid;
    *p++       = 0xB1; /* Uri-Path (11) "d" */
    *p++       = 'd';
    *p++       = 0x04; /* Uri-Path "pair" */
    memcpy(p, "pair", 4);
    p += 4;
    *p++ = 0x11; /* Content-Format (12): 42 */
    *p++ = 42;
    /* Option 2048: delta 2036 = 269 + 0x06e7 (nibble 14), length 8. */
    *p++ = 0xE8;
    *p++ = (uint8_t)((COAP_SESSION_OPTION - 12u - 269u) >> 8);
    *p++ = (uint8_t)(COAP_SESSION_OPTION - 12u - 269u);
    memcpy(p, session, PAIR_SESSION_LEN);
    p += PAIR_SESSION_LEN;
    *p++ = 0xFF;
    *p++ = 0x12; /* tag: network key in the clear (freedo tado/build_resources.go:453-456) */
    *p++ = CCM_KEY_LEN;
    memcpy(p, key, CCM_KEY_LEN);
    p += CCM_KEY_LEN;
    return (size_t)(p - out);
}

size_t pair_dpair_due(uint8_t *coap, size_t cap, uint32_t now_ms) {
    if (!pair_active(now_ms) || !w.armed || w.in_flight || w.dpair_sent >= PAIR_DPAIR_MAX
        || (int32_t)(now_ms - w.due_ms) < 0) {
        return 0;
    }
    uint16_t mid = (uint16_t)(w.mid_base + w.dpair_sent);
    size_t   n =
        pair_build_coap(coap, cap, mid, w.rnd + 2 + PAIR_SESSION_LEN * w.dpair_sent, w.net_key);
    if (n) {
        w.armed     = false;
        w.in_flight = true;
        w.dpair_sent++;
        stats.dpair_sent++;
    }
    return n;
}

void pair_dpair_done(bool delivered) {
    if (!w.in_flight) {
        return;
    }
    w.in_flight = false;
    if (delivered) {
        stats.dpair_delivered++;
    } else {
        stats.dpair_failed++;
    }
}

enum pair_reply pair_reply(const uint8_t src[MAC_EUI_LEN], const uint8_t *coap, size_t len) {
    if (!w.have_target || w.dpair_sent == 0 || len < 4u || memcmp(src, w.target, MAC_EUI_LEN) != 0
        || (coap[0] >> 6) != 1u || ((coap[0] >> 4) & 3u) != COAP_ACK) {
        return PAIR_REPLY_NONE;
    }
    uint16_t mid = (uint16_t)(coap[2] << 8 | coap[3]);
    if ((uint16_t)(mid - w.mid_base) >= w.dpair_sent) {
        return PAIR_REPLY_NONE; /* an answer to the server's own request */
    }
    if (coap[1] != COAP_CHANGED) {
        return PAIR_REPLY_OTHER;
    }
    if (!w.replied) {
        w.replied = true;
        stats.paired++;
    }
    if (w.open) {
        pair_close();
    }
    return PAIR_REPLY_PAIRED;
}

const struct pair_stats *pair_stats(void) {
    return &stats;
}
