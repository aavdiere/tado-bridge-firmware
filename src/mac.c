/* MAC frames of the tado 868 MHz link (#17).
 *
 * Ported from freedo: tado/mac.go (classes, wake-up and Enhanced ACK
 * parsing), tado/build_mac.go (builders, MAC FCS, broadcast), tado/frame.go
 * (Data frame) and the Enhanced ACK of firmware/cc1101_bridge/
 * cc1101_bridge.ino:699-725, 1216-1224.
 * Copyright 2026 The freedo contributors. Licensed under the Apache License,
 * Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0. Changes for
 * this project: C, caller-provided buffers, one sealed-frame routine shared
 * by Data and broadcast frames. */
#include "mac.h"

#include <string.h>

const uint8_t mac_eui_suffix[3] = {0xC5, 0x1B, 0x00};

static const uint8_t ie_rendezvous[2] = {0x82, 0x0E}; /* Header IE 0x1D, length 2 */
static const uint8_t ie_csl[2]        = {0x04, 0x0D}; /* Header IE 0x1A, length 4 */
static const uint8_t ie_term[2]       = {0x80, 0x3F}; /* Header Termination IE 0x7F */

#define SEC_CTRL 0x04u /* aux security control of broadcasts (freedo tado/emu.go:57) */

enum mac_class mac_classify(const uint8_t *f, size_t len) {
    if (len < 2) {
        return MAC_UNKNOWN;
    }
    if (f[0] == 0x69 && f[1] == 0xEC) {
        return MAC_DATA;
    }
    if (f[0] == 0x79 && f[1] == 0xEC) {
        return MAC_DATA_PENDING;
    }
    if ((f[0] == 0x49 || f[0] == 0x59) && f[1] == 0xE8) {
        return MAC_BROADCAST;
    }
    if (f[0] == 0x0B && f[1] == 0xE8) {
        return MAC_BEACON_REQUEST;
    }
    if (f[0] == 0x18 && f[1] == 0xEC) {
        return MAC_BEACON;
    }
    if (f[0] == 0x42 && f[1] == 0xEE) {
        return MAC_ENH_ACK;
    }
    if (f[0] == 0x25) {
        return MAC_WAKEUP;
    }
    return MAC_UNKNOWN;
}

uint16_t mac_fcs(uint16_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* --- Wake-up -------------------------------------------------------------- */
void mac_build_wakeup(uint8_t       out[MAC_WAKE_LEN],
                      uint8_t       seq,
                      const uint8_t dst_short[2],
                      uint16_t      rendezvous) {
    out[0] = 0x25;
    out[1] = seq;
    out[2] = (uint8_t)MAC_PAN_ID;
    out[3] = (uint8_t)(MAC_PAN_ID >> 8);
    out[4] = dst_short[0];
    out[5] = dst_short[1];
    memcpy(out + 6, ie_rendezvous, 2);
    out[8] = (uint8_t)rendezvous;
    out[9] = (uint8_t)(rendezvous >> 8);
    memcpy(out + 10, ie_term, 2);
}

bool mac_parse_wakeup(const uint8_t *f, size_t len, struct mac_wakeup *w) {
    if (len < MAC_WAKE_LEN || f[0] != 0x25) {
        return false;
    }
    w->seq          = f[1];
    w->pan          = (uint16_t)(f[2] | f[3] << 8);
    w->dst_short[0] = f[4];
    w->dst_short[1] = f[5];
    w->rendezvous   = (uint16_t)(f[8] | f[9] << 8);
    return true;
}

/* --- Enhanced ACK --------------------------------------------------------- */
void mac_build_enh_ack(uint8_t       out[MAC_ACK_LEN],
                       uint8_t       seq,
                       const uint8_t dst[MAC_EUI_LEN],
                       const uint8_t src[MAC_EUI_LEN],
                       uint16_t      phase) {
    out[0] = 0x42;
    out[1] = 0xEE;
    out[2] = seq;
    memcpy(out + 3, dst, MAC_EUI_LEN);
    memcpy(out + 11, src, MAC_EUI_LEN);
    memcpy(out + 19, ie_csl, 2);
    out[21] = (uint8_t)phase;
    out[22] = (uint8_t)(phase >> 8);
    out[23] = (uint8_t)MAC_CSL_UNITS;
    out[24] = (uint8_t)(MAC_CSL_UNITS >> 8);
    memcpy(out + 25, ie_term, 2);
    uint16_t fcs = mac_fcs(0, out, MAC_ACK_LEN - MAC_FCS_LEN);
    out[27]      = (uint8_t)fcs;
    out[28]      = (uint8_t)(fcs >> 8);
}

static bool ack_fcs_ok(const uint8_t *f) {
    uint16_t fcs = mac_fcs(0, f, MAC_ACK_LEN - MAC_FCS_LEN);
    return f[27] == (uint8_t)fcs && f[28] == (uint8_t)(fcs >> 8);
}

bool mac_parse_enh_ack(const uint8_t *f, size_t len, struct mac_enh_ack *a) {
    if (len != MAC_ACK_LEN || f[0] != 0x42 || f[1] != 0xEE || !ack_fcs_ok(f)) {
        return false;
    }
    a->seq = f[2];
    memcpy(a->dst, f + 3, MAC_EUI_LEN);
    memcpy(a->src, f + 11, MAC_EUI_LEN);
    a->phase  = (uint16_t)(f[21] | f[22] << 8);
    a->period = (uint16_t)(f[23] | f[24] << 8);
    return true;
}

static bool is_unicast_data(const uint8_t *f) {
    return (f[0] == 0x69 || f[0] == 0x79) && f[1] == 0xEC;
}

bool mac_ack_for_data(uint8_t       out[MAC_ACK_LEN],
                      const uint8_t h[MAC_HDR_LEN],
                      const uint8_t own[MAC_EUI_LEN],
                      uint16_t      phase) {
    if (!is_unicast_data(h) || memcmp(h + 3, own, MAC_EUI_LEN) != 0) {
        return false;
    }
    uint8_t peer[MAC_EUI_LEN];
    memcpy(peer, h + 11, 5);
    memcpy(peer + 5, mac_eui_suffix, 3);
    mac_build_enh_ack(out, h[2], peer, own, phase);
    return true;
}

bool mac_ack_answers(const uint8_t *ack, size_t ack_len, const uint8_t d[MAC_HDR_LEN]) {
    if (ack_len != MAC_ACK_LEN || ack[0] != 0x42 || ack[1] != 0xEE || ack[2] != d[2]) {
        return false;
    }
    /* The ACK's destination is our source address (5 header bytes plus the
     * fixed tail); its source is the destination of our Data frame. */
    if (memcmp(ack + 3, d + 11, 5) != 0 || memcmp(ack + 8, mac_eui_suffix, 3) != 0
        || memcmp(ack + 11, d + 3, MAC_EUI_LEN) != 0) {
        return false;
    }
    return ack_fcs_ok(ack);
}

/* --- Sealed frames -------------------------------------------------------- */
size_t mac_seal(const struct ccm_key *k,
                const uint8_t         hdr[MAC_HDR_LEN],
                const uint8_t        *body,
                size_t                body_len,
                uint8_t              *out,
                size_t                cap) {
    size_t total = MAC_HDR_LEN + body_len + MAC_FCS_LEN + MAC_MIC_LEN;
    if (total > cap || total > MAC_FRAME_MAX) {
        return 0;
    }
    uint8_t *pt = out + MAC_HDR_LEN;
    memmove(pt, body, body_len); /* body may already sit in out */
    memcpy(out, hdr, MAC_HDR_LEN);
    uint16_t fcs      = mac_fcs(mac_fcs(0, out, MAC_HDR_LEN), pt, body_len);
    pt[body_len]      = (uint8_t)fcs;
    pt[body_len + 1u] = (uint8_t)(fcs >> 8);
    if (!ccm_seal(
            k, out, out, MAC_HDR_LEN, pt, body_len + MAC_FCS_LEN, pt + body_len + MAC_FCS_LEN)) {
        return 0;
    }
    return total;
}

enum mac_open_result mac_open(const struct ccm_key *k,
                              const uint8_t        *frame,
                              size_t                len,
                              uint8_t              *body,
                              size_t                cap,
                              size_t               *body_len) {
    if (len < MAC_HDR_LEN + MAC_FCS_LEN + MAC_MIC_LEN) {
        return MAC_OPEN_MALFORMED;
    }
    size_t n = len - MAC_HDR_LEN - MAC_MIC_LEN; /* body and FCS */
    if (n > cap) {
        return MAC_OPEN_MALFORMED;
    }
    memcpy(body, frame + MAC_HDR_LEN, n);
    if (!ccm_open(k, frame, frame, MAC_HDR_LEN, body, n, frame + len - MAC_MIC_LEN)) {
        return MAC_OPEN_MIC;
    }
    uint16_t fcs = mac_fcs(mac_fcs(0, frame, MAC_HDR_LEN), body, n - MAC_FCS_LEN);
    if (body[n - 2u] != (uint8_t)fcs || body[n - 1u] != (uint8_t)(fcs >> 8)) {
        return MAC_OPEN_FCS;
    }
    *body_len = n - MAC_FCS_LEN;
    return MAC_OPEN_OK;
}

size_t mac_build_data(const struct ccm_key *k,
                      uint8_t              *out,
                      size_t                cap,
                      bool                  pending,
                      uint8_t               seq,
                      const uint8_t         dst[MAC_EUI_LEN],
                      const uint8_t         src[MAC_EUI_LEN],
                      uint32_t              counter,
                      const uint8_t        *dgram,
                      size_t                dgram_len) {
    if (dgram_len > MAC_DATA_MAX_DGRAM) {
        return 0;
    }
    uint8_t hdr[MAC_HDR_LEN];
    hdr[0] = pending ? 0x79 : 0x69;
    hdr[1] = 0xEC;
    hdr[2] = seq;
    memcpy(hdr + 3, dst, MAC_EUI_LEN);
    memcpy(hdr + 11, src, 5);

    uint8_t body[MAC_SHIM_LEN + MAC_DATA_MAX_DGRAM];
    memcpy(body, mac_eui_suffix, 3);
    body[3] = 0x04;
    body[4] = (uint8_t)counter;
    body[5] = (uint8_t)(counter >> 8);
    body[6] = (uint8_t)(counter >> 16);
    body[7] = (uint8_t)(counter >> 24);
    memcpy(body + MAC_SHIM_LEN, dgram, dgram_len);
    return mac_seal(k, hdr, body, MAC_SHIM_LEN + dgram_len, out, cap);
}

enum mac_open_result mac_open_data(const struct ccm_key *k,
                                   const uint8_t        *frame,
                                   size_t                len,
                                   uint8_t              *body,
                                   size_t                cap,
                                   struct mac_data      *d) {
    if (len < MAC_HDR_LEN + MAC_SHIM_LEN + MAC_FCS_LEN + MAC_MIC_LEN || !is_unicast_data(frame)) {
        return MAC_OPEN_MALFORMED;
    }
    size_t               n = 0;
    enum mac_open_result r = mac_open(k, frame, len, body, cap, &n);
    if (r != MAC_OPEN_OK) {
        return r;
    }
    d->seq     = frame[2];
    d->pending = frame[0] == 0x79;
    memcpy(d->dst, frame + 3, MAC_EUI_LEN);
    memcpy(d->src, frame + 11, 5);
    memcpy(d->src + 5, mac_eui_suffix, 3);
    d->counter   = (uint32_t)body[4] | (uint32_t)body[5] << 8 | (uint32_t)body[6] << 16
                   | (uint32_t)body[7] << 24;
    d->dgram     = body + MAC_SHIM_LEN;
    d->dgram_len = n - MAC_SHIM_LEN;
    return MAC_OPEN_OK;
}

size_t mac_build_broadcast(const struct ccm_key *k,
                           uint8_t              *out,
                           size_t                cap,
                           bool                  pending,
                           uint8_t               seq,
                           const uint8_t         src[MAC_EUI_LEN],
                           uint32_t              counter,
                           const uint8_t        *pkt,
                           size_t                pkt_len) {
    if (pkt_len > MAC_BCAST_MAX_PKT) {
        return 0;
    }
    uint8_t hdr[MAC_HDR_LEN];
    hdr[0] = pending ? 0x59 : 0x49;
    hdr[1] = 0xE8;
    hdr[2] = seq;
    hdr[3] = 0xFF;
    hdr[4] = 0xFF;
    memcpy(hdr + 5, src, MAC_EUI_LEN);
    hdr[13] = SEC_CTRL;
    hdr[14] = (uint8_t)counter;
    hdr[15] = (uint8_t)(counter >> 8);

    uint8_t body[2u + MAC_BCAST_MAX_PKT];
    body[0] = (uint8_t)(counter >> 16); /* freedo WrapBroadcastPayload, tado/broadcast.go:258 */
    body[1] = (uint8_t)(counter >> 24);
    memcpy(body + 2, pkt, pkt_len);
    return mac_seal(k, hdr, body, 2u + pkt_len, out, cap);
}
