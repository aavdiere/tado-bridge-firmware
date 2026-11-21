/* CoAP relay between the radio link and the WebSocket frame (#17). See
 * relay.h. The duplicate rule is ported from freedo, tado/mac_duplicate.go
 * (Copyright 2026 The freedo contributors, Apache License 2.0,
 * https://www.apache.org/licenses/LICENSE-2.0); the rest is this project's
 * own, since freedo terminates CoAP itself and has no server to relay to. */
#include "relay.h"

#include "csl.h"
#include "txgate.h"

#include <string.h>

/* The datagram being built for a downlink. Radio task only; static because
 * the task's stack is 2 KB. */
static uint8_t down_pkt[LOWPAN_PKT_MAX];

/* --- Frame ------------------------------------------------------------------ */
bool relay_frame_parse(const uint8_t *m, size_t len, struct relay_frame *f) {
    if (len < RELAY_HDR_LEN || m[2] != 0x10) {
        return false;
    }
    size_t coap_len = (size_t)m[26] << 8 | m[27];
    if (coap_len != len - RELAY_HDR_LEN) {
        return false;
    }
    f->direction = (uint16_t)(m[0] << 8 | m[1]);
    memcpy(f->ipv6, m + 3, 16);
    f->port     = (uint16_t)(m[22] << 8 | m[23]);
    f->coap     = m + RELAY_HDR_LEN;
    f->coap_len = coap_len;
    return true;
}

void relay_frame_header(uint8_t       h[RELAY_HDR_LEN],
                        const uint8_t ipv6[16],
                        uint16_t      port,
                        size_t        coap_len) {
    memset(h, 0, RELAY_HDR_LEN);
    h[0] = (uint8_t)(RELAY_DIR_UP >> 8);
    h[1] = (uint8_t)RELAY_DIR_UP;
    h[2] = 0x10;
    memcpy(h + 3, ipv6, 16);
    h[22] = (uint8_t)(port >> 8);
    h[23] = (uint8_t)port;
    h[26] = (uint8_t)(coap_len >> 8);
    h[27] = (uint8_t)coap_len;
}

void relay_ipv6_from_wire(const uint8_t wire[MAC_EUI_LEN], uint8_t ipv6[16]) {
    memset(ipv6, 0, 16);
    ipv6[0] = 0xFE;
    ipv6[1] = 0x80;
    lowpan_iid_from_wire(wire, ipv6 + 8);
}

bool relay_wire_from_ipv6(const uint8_t ipv6[16], uint8_t wire[MAC_EUI_LEN]) {
    static const uint8_t ll[8] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0};
    if (memcmp(ipv6, ll, 8) != 0) {
        return false;
    }
    lowpan_wire_from_iid(ipv6 + 8, wire);
    return memcmp(wire + 5, mac_eui_suffix, 3) == 0;
}

/* --- Peers ------------------------------------------------------------------ */
void relay_init(struct relay *r, const uint8_t own[MAC_EUI_LEN]) {
    memset(r, 0, sizeof *r);
    memcpy(r->own, own, MAC_EUI_LEN);
}

static struct relay_peer *peer_find(struct relay *r, const uint8_t eui[MAC_EUI_LEN]) {
    for (unsigned i = 0; i < RELAY_PEERS; i++) {
        if (r->peer[i].used && memcmp(r->peer[i].eui, eui, MAC_EUI_LEN) == 0) {
            return &r->peer[i];
        }
    }
    return NULL;
}

/* The entry for eui; a new device takes a free entry or the one silent
 * for the longest time. */
static struct relay_peer *peer_get(struct relay *r, const uint8_t eui[MAC_EUI_LEN], uint32_t now) {
    struct relay_peer *p = peer_find(r, eui);
    if (!p) {
        p = &r->peer[0];
        for (unsigned i = 0; i < RELAY_PEERS; i++) {
            struct relay_peer *c = &r->peer[i];
            if (!c->used) {
                p = c;
                break;
            }
            if ((uint32_t)(now - c->seen_ms) > (uint32_t)(now - p->seen_ms)) {
                p = c;
            }
        }
        memset(p, 0, sizeof *p);
        p->used = true;
        memcpy(p->eui, eui, MAC_EUI_LEN);
    }
    p->seen_ms = now;
    return p;
}

/* --- Uplink ----------------------------------------------------------------- */
/* A device that misses our Enhanced ACK sends the frame again with the same
 * MAC seq and the same datagram under the next frame counter, 1.5 to 3.6 s
 * later. The window bounds the match in time, because a device restarts its
 * seq after a reboot; other bytes under the same seq are a new frame
 * (freedo tado/mac_duplicate.go). */
static bool duplicate(struct relay_peer *p, const struct mac_data *d, uint32_t now) {
    if (p->have_last && p->last_seq == d->seq && p->last_len == d->dgram_len
        && (uint32_t)(now - p->last_ms) < RELAY_DUP_WINDOW_MS
        && memcmp(p->last, d->dgram, d->dgram_len) == 0) {
        return true;
    }
    p->have_last = true;
    p->last_seq  = d->seq;
    p->last_len  = (uint8_t)d->dgram_len;
    p->last_ms   = now;
    memcpy(p->last, d->dgram, d->dgram_len);
    return false;
}

enum relay_up
relay_uplink(struct relay *r, const struct mac_data *d, uint32_t now, struct relay_uplink *up) {
    if (d->dgram_len == 0 || d->dgram_len > MAC_DATA_MAX_DGRAM) {
        return RELAY_UP_BAD;
    }
    struct relay_peer *p = peer_get(r, d->src, now);
    if (duplicate(p, d, now)) {
        return RELAY_UP_DUPLICATE;
    }

    const uint8_t *dg = d->dgram;
    size_t         dl = d->dgram_len;
    if (lowpan_is_fragment(dg, dl)) {
        switch (lowpan_reasm_add(&r->reasm, d->src, dg, dl, now, &dg, &dl)) {
            case LOWPAN_REASM_DONE:
                break;
            case LOWPAN_REASM_HELD:
                return RELAY_UP_HELD;
            default:
                return RELAY_UP_BAD;
        }
    }

    struct lowpan_udp u;
    if (!lowpan_parse_udp(dg, dl, &u)) {
        struct lowpan_iphc h;
        return lowpan_parse_iphc(dg, dl, &h) ? RELAY_UP_NOT_UDP : RELAY_UP_BAD;
    }
    /* Only what the device sends to the bridge itself: link-local on both
     * ends, the destination ours. Datagrams between devices (context 0,
     * the zone plane) would need forwarding on the radio, which is not
     * ported (freedo tado/emu.go relayZone). */
    uint8_t own_iid[8];
    lowpan_iid_from_wire(r->own, own_iid);
    if (u.ip.dst_multicast || u.ip.src.ctx == 0 || u.ip.dst.ctx == 0
        || (u.ip.dst.ctx == LOWPAN_CTX_STATELESS && u.ip.dst.has_iid
            && memcmp(u.ip.dst.iid, own_iid, 8) != 0)) {
        return RELAY_UP_NOT_OURS;
    }
    if (u.ip.src.ctx != LOWPAN_CTX_STATELESS || u.ip.dst.ctx != LOWPAN_CTX_STATELESS
        || !lowpan_addr_ipv6(&u.ip.src, d->src, up->ipv6) || u.payload_len < 4u) {
        return RELAY_UP_BAD;
    }
    p->have_ports = true;
    p->sport      = u.sport;
    p->dport      = u.dport;
    up->port      = u.sport;
    up->coap      = u.payload;
    up->coap_len  = u.payload_len;
    return RELAY_UP_FORWARD;
}

/* --- Downlink --------------------------------------------------------------- */
enum relay_down relay_downlink(struct relay *r, const uint8_t *msg, size_t len) {
    struct relay_frame f;
    if (!relay_frame_parse(msg, len, &f) || f.direction != RELAY_DIR_DOWN || f.coap_len < 4u) {
        return RELAY_DOWN_BAD_FRAME;
    }
    if (f.coap_len > RELAY_COAP_MAX) {
        return RELAY_DOWN_TOO_LARGE;
    }
    uint8_t dst[MAC_EUI_LEN];
    if (!relay_wire_from_ipv6(f.ipv6, dst) || memcmp(dst, r->own, MAC_EUI_LEN) == 0) {
        return RELAY_DOWN_BAD_ADDRESS;
    }
#if TXGATE_NET_ON_PROTECTED
    /* Channel-26 build (stage P7.1): downlinks to the named spare device
     * only; a stock device on that channel shares our PAN. The transmit
     * gate refuses the same frames again (src/txgate.h rule 6). */
    if (!txgate_unicast_allowed(dst)) {
        return RELAY_DOWN_NOT_PEER;
    }
#endif

    /* UDP ports [I]. A request (code 0.01-0.31) goes out from port 4005 to
     * the port the server names, as the stock bridge's downlinks do (freedo
     * tado/build_coap.go:22-24). Anything else answers the device: the
     * ports of its last datagram, swapped (freedo BuildCoAPAck's callers). */
    uint8_t  code    = f.coap[1];
    bool     request = (code >> 5) == 0 && code != 0;
    uint16_t dport   = f.port ? f.port : RELAY_PORT_COAP;
    uint16_t sport   = request ? RELAY_PORT_BRIDGE : RELAY_PORT_COAP;
    if (!request) {
        const struct relay_peer *p = peer_find(r, dst);
        if (p && p->have_ports) {
            sport = p->dport;
            dport = p->sport;
        }
    }

    size_t n = lowpan_build_udp_ll(
        down_pkt, sizeof down_pkt, r->own, dst, sport, dport, f.coap, f.coap_len);
    unsigned count = lowpan_frag_count(down_pkt, n);
    if (n == 0 || count == 0) {
        return RELAY_DOWN_TOO_LARGE;
    }
    if (count > csl_free_jobs()) {
        return RELAY_DOWN_QUEUE_FULL;
    }
    uint16_t id = r->next_id;
    r->next_id  = id < RELAY_ID_LAST ? (uint16_t)(id + 1u) : 0u;
    if (count == 1) {
        return csl_enqueue_data(dst, down_pkt, n, id, 0, CSL_KEY_NETWORK) ? RELAY_DOWN_QUEUED
                                                                          : RELAY_DOWN_QUEUE_FULL;
    }
    uint16_t tag = r->frag_tag++;
    for (unsigned i = 0; i < count; i++) {
        uint8_t body[LOWPAN_FRAME_MAX];
        size_t  bl = lowpan_frag_body(down_pkt, n, tag, i, body);
        if (!csl_enqueue_data(
                dst, body, bl, id, i + 1u < count ? CSL_JOB_MORE : 0u, CSL_KEY_NETWORK)) {
            /* Only the first can fail (no queue for a 17th device): the
             * free jobs were counted above, and the queue exists after it. */
            return RELAY_DOWN_QUEUE_FULL;
        }
    }
    return RELAY_DOWN_QUEUED;
}
