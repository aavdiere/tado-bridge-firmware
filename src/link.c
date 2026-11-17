/* Radio link layer (#17), and pairing (#19, src/pair.h). See link.h. */
#include "link.h"

#include "csl.h"
#include "lowpan.h"
#include "msgq.h"
#include "pair.h"
#include "relay.h"

#include <string.h>

/* Queues between the radio task and the net task, in static RAM
 * (docs/design.md "RAM measured (#16, #17)"). Each holds three of the
 * largest messages (28 + 1232 bytes) or about 40 typical ones. */
#define QUEUE_BYTES 4096u
_Static_assert(QUEUE_BYTES > 3u * (RELAY_MSG_MAX + 2u), "three full-size messages");

static uint8_t     up_store[QUEUE_BYTES], down_store[QUEUE_BYTES];
static struct msgq up_q, down_q;

static struct relay   relay;
static struct ccm_key net_key;
static uint8_t        own[MAC_EUI_LEN];
static bool           have_own;

/* A downlink that found the job pool full waits at the head of its queue,
 * but not for ever: a device that never answers must not hold up the
 * messages for the others behind it. */
#define DOWN_WAIT_MS 30000u
static bool     down_waiting;
static uint32_t down_wait_ms;

static struct link_stats stats;

/* Scratch for one frame's plaintext and for one message from the server.
 * Radio task only; static because its stack is 2 KB. */
static uint8_t plain[MAC_FRAME_MAX];
static uint8_t down_msg[RELAY_MSG_MAX];

/* Router Advertisements: every 200-600 s (freedo docs/PROTOCOL.md:
 * 1378-1383; a device stops reporting after about 20 minutes without one),
 * and soon after a Router Solicitation. Only while a device has been heard
 * in the last 30 minutes: with nobody listening the 1 s train is wasted
 * airtime [I]. */
#define RA_MIN_MS       200000u
#define RA_SPAN_MS      400000u
#define RA_SOLICITED_MS 1000u
#define RA_HOLDOFF_MS   10000u
#define RA_LISTENER_MS  1800000u
static volatile uint32_t router_gw, router_dns; /* written by the net task */
static uint32_t          ra_due_ms, ra_last_ms, heard_ms;
static bool              ra_armed, ra_sent_once, heard;

/* Datagram ids of the link's own frames, above the relay's (relay.h). */
#define ID_RA    0xFFFFu
#define ID_DPAIR 0xFFFEu
_Static_assert(ID_DPAIR > RELAY_ID_LAST, "d/pair id not used by the relay");

/* The Enhanced Beacon prepared for the last Beacon Request of the window's
 * target, and that request's header. */
static uint8_t beacon[PAIR_BEACON_LEN], beacon_req[MAC_HDR_LEN];
static size_t  beacon_len;

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

void link_init(const uint8_t own_wire[MAC_EUI_LEN]) {
    msgq_init(&up_q, up_store, sizeof up_store);
    msgq_init(&down_q, down_store, sizeof down_store);
    have_own = own_wire != NULL;
    if (have_own) {
        memcpy(own, own_wire, MAC_EUI_LEN);
    }
    relay_init(&relay, own);
    pair_init();
    beacon_len = 0;
    link_clear_key();
}

static void csl_restart(uint32_t counter, uint32_t limit) {
    /* The pairing key's schedule is set only while a window is open; jobs
     * that need it fail as "no key" otherwise. */
    struct csl_config cfg = {
        .key = {&net_key, pair_key()}, .counter = counter, .counter_limit = limit};
    memcpy(cfg.own, own, MAC_EUI_LEN);
    csl_init(&cfg);
}

void link_set_key(const uint8_t key[CCM_KEY_LEN], uint32_t first_counter, uint32_t counter_limit) {
    pair_close(); /* a window would hand out the key it was opened with */
    ccm_set_key(&net_key, key);
    csl_restart(first_counter, counter_limit);
    ra_armed = false;
}

void link_set_counter_limit(uint32_t counter_limit) {
    csl_set_counter_limit(counter_limit);
}

void link_clear_key(void) {
    pair_close(); /* a window hands out the key in use: no key, no window */
    ccm_clear_key(&net_key);
    csl_restart(0, 0);
    ra_armed = false;
}

uint32_t link_counter(void) {
    return csl_counter();
}

bool link_ready(void) {
    return have_own && net_key.set;
}

const uint8_t *link_own(void) {
    return have_own ? own : NULL;
}

bool link_uplink_room(void) {
    return msgq_room(&up_q) >= RELAY_MSG_MAX;
}

const struct link_stats *link_stats(void) {
    return &stats;
}

/* --- Receive ---------------------------------------------------------------- */
/* An ICMPv6 Router Solicitation inside a compressed packet. */
static bool is_router_solicitation(const uint8_t *pkt, size_t len) {
    struct lowpan_iphc h;
    return lowpan_parse_iphc(pkt, len, &h) && !h.nh_compressed && h.next_header == LOWPAN_NH_ICMPV6
           && h.len < len && pkt[h.len] == 133u;
}

static void solicit(uint32_t now_ms) {
    stats.rs_seen++;
    uint32_t due = now_ms + RA_SOLICITED_MS;
    if (ra_sent_once && (uint32_t)(now_ms - ra_last_ms) < RA_HOLDOFF_MS) {
        due = ra_last_ms + RA_HOLDOFF_MS;
    }
    if (!ra_armed || (int32_t)(due - ra_due_ms) < 0) {
        ra_due_ms = due;
        ra_armed  = true;
    }
}

/* A frame of the pairing window's target under the pairing key (#19): it
 * has taken our beacon. Its Router Solicitation asks for an advertisement
 * pair, while the window has one left; once they are used up it asks for
 * nothing: a lone advertisement under the network key is of no use to the
 * joiner and would only cost airtime. Nothing of it goes to the server.
 * Frames of any other device are not tried under the pairing key. */
static enum link_rx on_pairing(bool rs, uint32_t now_ms) {
    pair_note_joiner(now_ms);
    if (rs && pair_ra_pairs_left(now_ms)) {
        solicit(now_ms);
    }
    return LINK_RX_PAIRING;
}

static enum link_rx on_data(const uint8_t *frame, size_t len, uint32_t now_ms) {
    if (len < MAC_HDR_LEN || memcmp(frame + 3, own, MAC_EUI_LEN) != 0) {
        return LINK_RX_NOT_FOR_US;
    }
    if (!net_key.set) {
        return LINK_RX_NO_KEY;
    }
    struct mac_data d;
    if (mac_open_data(&net_key, frame, len, plain, sizeof plain, &d) != MAC_OPEN_OK) {
        uint8_t src[MAC_EUI_LEN];
        memcpy(src, frame + 11, 5);
        memcpy(src + 5, mac_eui_suffix, 3);
        if (pair_is_target(src)
            && mac_open_data(pair_key(), frame, len, plain, sizeof plain, &d) == MAC_OPEN_OK) {
            return on_pairing(is_router_solicitation(d.dgram, d.dgram_len), now_ms);
        }
        return LINK_RX_MIC_FAIL;
    }
    heard    = true;
    heard_ms = now_ms;

    struct relay_uplink up;
    switch (relay_uplink(&relay, &d, now_ms, &up)) {
        case RELAY_UP_FORWARD: {
            /* The target's answer to d/pair is the window's, not the server's. */
            switch (pair_reply(d.src, up.coap, up.coap_len)) {
                case PAIR_REPLY_PAIRED:
                    return LINK_RX_PAIRED;
                case PAIR_REPLY_OTHER:
                    return LINK_RX_NOT_RELAYED;
                case PAIR_REPLY_NONE:
                    break;
            }
            uint8_t hdr[RELAY_HDR_LEN];
            relay_frame_header(hdr, up.ipv6, up.port, up.coap_len);
            return msgq_put(&up_q, hdr, sizeof hdr, up.coap, up.coap_len) ? LINK_RX_FORWARDED
                                                                          : LINK_RX_DROPPED;
        }
        case RELAY_UP_HELD:
            return LINK_RX_HELD;
        case RELAY_UP_DUPLICATE:
            return LINK_RX_DUPLICATE;
        case RELAY_UP_NOT_UDP:
            if (is_router_solicitation(d.dgram, d.dgram_len)) {
                solicit(now_ms);
            }
            return LINK_RX_NOT_RELAYED;
        default:
            return LINK_RX_NOT_RELAYED;
    }
}

static enum link_rx on_broadcast(const uint8_t *frame, size_t len, uint32_t now_ms) {
    if (!net_key.set) {
        return LINK_RX_NO_KEY;
    }
    size_t n = 0;
    if (mac_open(&net_key, frame, len, plain, sizeof plain, &n) != MAC_OPEN_OK || n < 2u) {
        /* 49 e8 seq | ff ff | src(8) | ...: the source is in the clear. */
        if (len >= MAC_HDR_LEN && pair_is_target(frame + 5)
            && mac_open(pair_key(), frame, len, plain, sizeof plain, &n) == MAC_OPEN_OK
            && n >= 2u) {
            return on_pairing(is_router_solicitation(plain + 2, n - 2u), now_ms);
        }
        return LINK_RX_MIC_FAIL;
    }
    heard    = true;
    heard_ms = now_ms;
    if (is_router_solicitation(plain + 2, n - 2u)) {
        solicit(now_ms);
    }
    return LINK_RX_BROADCAST;
}

/* A Beacon Request: answered only for the pairing window's target, with
 * the bridge's next seq and frame counter (freedo docs/PROTOCOL.md:
 * 651-656). The beacon is built here, before the radio task's wait, so
 * nothing but the transmission itself is left for the deadline. */
static enum link_rx on_beacon_request(const uint8_t *frame, size_t len, uint32_t now_ms) {
    beacon_len   = 0;
    uint8_t  seq = 0;
    uint32_t ctr = 0;
    if (!net_key.set || pair_check_request(frame, len, now_ms) != PAIR_REQ_OK
        || !csl_take_seq_counter(&seq, &ctr)) {
        return LINK_RX_BEACON_REQ;
    }
    beacon_len = pair_build_beacon(beacon, seq, frame + 7, own, ctr);
    if (beacon_len == 0) {
        return LINK_RX_BEACON_REQ;
    }
    memcpy(beacon_req, frame, MAC_HDR_LEN);
    return LINK_RX_BEACON_ANSWER;
}

const uint8_t *link_beacon(size_t *len, const uint8_t **answered) {
    *len      = beacon_len;
    *answered = beacon_req;
    return beacon_len ? beacon : NULL;
}

void link_beacon_done(bool sent) {
    if (beacon_len) {
        if (sent) {
            stats.beacons_sent++;
        } else {
            stats.beacons_dropped++;
        }
    }
    beacon_len = 0;
}

enum link_rx
link_on_frame(const uint8_t *frame, size_t len, bool crc_ok, uint64_t end_us, uint32_t now_ms) {
    enum link_rx r = LINK_RX_OTHER;
    /* The coordinator sees every frame while it waits for an Enhanced ACK:
     * a bad CRC there may be the ACK, damaged. */
    bool taken = csl_on_frame(frame, len, crc_ok, end_us);
    if (!crc_ok) {
        r = LINK_RX_BAD_CRC;
    } else if (taken) {
        r = LINK_RX_ACK_TAKEN;
    } else if (have_own) {
        switch (mac_classify(frame, len)) {
            case MAC_DATA:
            case MAC_DATA_PENDING:
                r = on_data(frame, len, now_ms);
                break;
            case MAC_BROADCAST:
                r = on_broadcast(frame, len, now_ms);
                break;
            case MAC_ENH_ACK:
                r = LINK_RX_ACK_OTHER;
                break;
            case MAC_BEACON_REQUEST:
                r = on_beacon_request(frame, len, now_ms);
                break;
            default:
                break;
        }
    }
    stats.rx[r]++;
    return r;
}

/* --- Downlink and Router Advertisements --------------------------------------- */
void link_set_router(uint32_t gateway, uint32_t dns) {
    router_gw  = gateway;
    router_dns = dns;
}

static void router_advertisement(uint32_t now_ms, uint32_t rnd) {
    if (!ra_armed) {
        /* First one soon after the key is set, then every 200-600 s. */
        ra_due_ms = now_ms + RA_SOLICITED_MS;
        ra_armed  = true;
    }
    if ((int32_t)(now_ms - ra_due_ms) < 0) {
        return;
    }
    ra_due_ms = now_ms + RA_MIN_MS + rnd % RA_SPAN_MS;
    /* In a pairing window with an advertisement pair left, every
     * advertisement goes as a pair, or waits until one fits: never as a
     * lone copy that the joiner cannot read, so a window sends at most
     * PAIR_RA_PAIRS_MAX pairs and nothing else in their place. */
    bool pairing = pair_ra_pairs_left(now_ms);
    if (!pairing && (!heard || (uint32_t)(now_ms - heard_ms) > RA_LISTENER_MS)) {
        return; /* nobody to hear it; in a pairing window the joiner does */
    }
    if (pairing && (csl_broadcasts_queued() != 0 || !csl_room_for_broadcasts(2))) {
        ra_due_ms = now_ms + RA_SOLICITED_MS; /* the one before is still queued: try again */
        return;
    }
    uint8_t  pkt[LOWPAN_RA_LEN];
    uint32_t gw = router_gw, dns = router_dns;
    bool     ok = lowpan_build_ra(pkt, sizeof pkt, own, (const uint8_t *)&gw, (const uint8_t *)&dns)
                  == LOWPAN_RA_LEN;
    if (pairing) {
        /* In a pairing window: the copy under the network key with Frame
         * Pending, then the same under the pairing key, so that the joiner
         * can read one (freedo docs/PROTOCOL.md:667-672). The pool has room
         * for both, so neither enqueue fails. */
        ok = ok && pair_take_ra_pair(now_ms)
             && csl_enqueue_broadcast(
                 pkt, sizeof pkt, ID_RA, CSL_JOB_PAIRING | CSL_JOB_PENDING, CSL_KEY_NETWORK)
             && csl_enqueue_broadcast(pkt, sizeof pkt, ID_RA, CSL_JOB_PAIRING, CSL_KEY_PAIRING);
        if (ok) {
            stats.ra_pairs_queued++;
        }
    } else {
        ok = ok && csl_enqueue_broadcast(pkt, sizeof pkt, ID_RA, 0, CSL_KEY_NETWORK);
    }
    if (ok) {
        stats.ra_queued++;
        ra_last_ms   = now_ms;
        ra_sent_once = true;
    } else {
        ra_due_ms = now_ms + RA_SOLICITED_MS; /* the one before is still queued: try again */
    }
}

/* d/pair, when the window says it is due: a link-local UDP datagram from
 * port 5683 to 5683 (freedo tado/build_resources.go:476-478) under the
 * pairing key, ahead of nothing (the joiner has no other downlinks yet). */
static void pair_downlink(uint32_t now_ms) {
    uint8_t target[MAC_EUI_LEN];
    if (csl_free_jobs() == 0 || !pair_target(target)) {
        return;
    }
    uint8_t coap[PAIR_COAP_LEN];
    size_t  n = pair_dpair_due(coap, sizeof coap, now_ms);
    if (n == 0) {
        return;
    }
    uint8_t dg[LOWPAN_FRAME_MAX];
    size_t  dl =
        lowpan_build_udp_ll(dg, sizeof dg, own, target, RELAY_PORT_COAP, RELAY_PORT_COAP, coap, n);
    bool ok =
        dl != 0 && csl_enqueue_data(target, dg, dl, ID_DPAIR, CSL_JOB_PAIRING, CSL_KEY_PAIRING);
    wipe(coap, sizeof coap); /* both hold the network key */
    wipe(dg, sizeof dg);
    if (ok) {
        stats.dpair_queued++;
    } else {
        pair_dpair_done(false);
    }
}

void link_poll(uint32_t now_ms, uint32_t rnd) {
    size_t n;
    while ((n = msgq_peek(&down_q, down_msg, sizeof down_msg)) != 0) {
        if (!link_ready()) {
            stats.down_refused++;
        } else {
            switch (relay_downlink(&relay, down_msg, n)) {
                case RELAY_DOWN_QUEUED:
                    stats.down_queued++;
                    break;
                case RELAY_DOWN_QUEUE_FULL:
                    /* Keep it for a later pass, when jobs have finished. Its
                     * turn comes before any later message, so the order of
                     * the server's messages holds. */
                    if (!down_waiting) {
                        down_waiting = true;
                        down_wait_ms = now_ms;
                    }
                    if ((uint32_t)(now_ms - down_wait_ms) < DOWN_WAIT_MS) {
                        goto advertise;
                    }
                    stats.down_full++;
                    break;
                default:
                    stats.down_refused++;
                    break;
            }
        }
        down_waiting = false;
        msgq_pop(&down_q);
    }
advertise:
    if (link_ready()) {
        router_advertisement(now_ms, rnd);
        pair_downlink(now_ms);
    }
}

void link_on_report(const struct csl_report *r) {
    uint8_t target[MAC_EUI_LEN];
    if (r->id == ID_DPAIR && r->final && pair_target(target)
        && memcmp(r->dst, target, MAC_EUI_LEN) == 0) {
        pair_dpair_done(r->outcome == CSL_DELIVERED);
    }
}

bool link_pair_open(const uint8_t target[MAC_EUI_LEN],
                    const uint8_t key[CCM_KEY_LEN],
                    const uint8_t rnd[PAIR_RAND_LEN],
                    uint32_t      now_ms) {
    if (!link_ready() || memcmp(target, own, MAC_EUI_LEN) == 0
        || !pair_open(target, key, rnd, now_ms)) {
        return false;
    }
    /* The window's first advertisement pair goes a second from now, before
     * any frame of the joiner (freedo docs/PROTOCOL.md:673-674). */
    ra_due_ms = now_ms + RA_SOLICITED_MS;
    ra_armed  = true;
    return true;
}

void link_pair_close(void) {
    pair_close();
}

bool link_pair_active(uint32_t now_ms) {
    return pair_active(now_ms);
}

bool link_downlink_put(const uint8_t *msg, size_t len) {
    return msgq_put(&down_q, msg, len, NULL, 0);
}

size_t link_uplink_peek(uint8_t *out, size_t cap) {
    return msgq_peek(&up_q, out, cap);
}

void link_uplink_pop(void) {
    msgq_pop(&up_q);
}
