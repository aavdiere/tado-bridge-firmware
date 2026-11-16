/* 6LoWPAN for the tado link (#17).
 *
 * Ported from freedo: tado/coap.go (IPHC and UDP-NHC parsing, address
 * expansion), tado/build_coap.go (link-local datagram, UDP checksum),
 * tado/sixlowpan_frag.go, tado/sixlowpan_reasm.go and tado/broadcast.go
 * (Router Advertisement).
 * Copyright 2026 The freedo contributors. Licensed under the Apache License,
 * Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0. Changes for
 * this project: C, fixed buffers instead of maps and slices, the CoAP
 * message is carried as bytes and not parsed. */
#include "lowpan.h"

#include <string.h>

/* 6LoWPAN context 0 on this link (freedo tado/coap.go:23-41). */
static const uint8_t ctx0_prefix[8] = {0xAA, 0xAA, 0, 0, 0, 0, 0, 0};

#define IPHC_LL0   0x7Eu /* TF elided, next header compressed, hop limit 64 */
#define IPHC_LL1   0x33u /* source and destination link-local, from the L2 addresses */
#define NHC_UDP_IN 0xF0u /* UDP, checksum and both ports inline */

#define FRAG1_HDR 4u
#define FRAGN_HDR 5u

/* --- IPHC ----------------------------------------------------------------- */
static unsigned src_inline(unsigned ac, unsigned am) {
    if (ac == 0) {
        static const uint8_t n[4] = {16, 8, 2, 0};
        return n[am];
    }
    return am == 1 ? 8u : am == 2 ? 2u : 0u;
}

static unsigned dst_inline(unsigned ac, unsigned am, unsigned mcast) {
    if (!mcast) {
        return src_inline(ac, am);
    }
    if (ac == 0) {
        static const uint8_t n[4] = {16, 6, 4, 1};
        return n[am];
    }
    return am == 0 ? 6u : 0u;
}

/* Describes one address field and steps over its n inline bytes; a form this
 * link does not use reads as unusable (freedo readIPHCAddr). */
static size_t read_addr(const uint8_t      *b,
                        size_t              len,
                        size_t              i,
                        unsigned            n,
                        unsigned            ac,
                        unsigned            am,
                        unsigned            ctx,
                        struct lowpan_addr *a) {
    a->ctx     = LOWPAN_CTX_UNUSABLE;
    a->has_iid = false;
    memset(a->iid, 0, sizeof a->iid);
    int8_t prefix = ac == 0 ? LOWPAN_CTX_STATELESS : (int8_t)ctx;
    if (am == 3) {
        a->ctx = prefix;
    } else if (am == 1 && n == 8 && i + n <= len) {
        a->ctx     = prefix;
        a->has_iid = true;
        memcpy(a->iid, b + i, 8);
    }
    return i + n;
}

bool lowpan_parse_iphc(const uint8_t *b, size_t n, struct lowpan_iphc *h) {
    if (n < 2 || (b[0] & 0xE0u) != 0x60u) {
        return false;
    }
    unsigned tf = (b[0] >> 3) & 3u, nh = (b[0] >> 2) & 1u, hlim = b[0] & 3u;
    unsigned cid = b[1] >> 7, sac = (b[1] >> 6) & 1u, sam = (b[1] >> 4) & 3u;
    unsigned m = (b[1] >> 3) & 1u, dac = (b[1] >> 2) & 1u, dam = b[1] & 3u;
    size_t   i   = 2;
    unsigned sci = 0, dci = 0;
    if (cid) {
        if (i >= n) {
            return false;
        }
        sci = b[i] >> 4;
        dci = b[i] & 0x0Fu;
        i++;
    }
    i += tf == 0 ? 4u : tf == 1 ? 3u : tf == 2 ? 1u : 0u;
    h->nh_compressed = nh != 0;
    h->next_header   = 0;
    if (!nh) {
        if (i >= n) {
            return false;
        }
        h->next_header = b[i++];
    }
    static const uint8_t hops[4] = {0, 1, 64, 255};
    h->hop_limit                 = hops[hlim];
    if (hlim == 0) {
        if (i >= n) {
            return false;
        }
        h->hop_limit = b[i++];
    }
    i                = read_addr(b, n, i, src_inline(sac, sam), sac, sam, sci, &h->src);
    h->dst_multicast = m != 0;
    if (m) {
        h->dst.ctx     = LOWPAN_CTX_UNUSABLE;
        h->dst.has_iid = false;
        memset(h->dst.iid, 0, sizeof h->dst.iid);
        i += dst_inline(dac, dam, 1);
    } else {
        i = read_addr(b, n, i, dst_inline(dac, dam, 0), dac, dam, dci, &h->dst);
    }
    if (i > n) {
        return false;
    }
    h->len = i;
    return true;
}

static unsigned udp_port_bytes(unsigned pp) {
    return pp == 0 ? 4u : pp == 3 ? 1u : 3u;
}

bool lowpan_parse_udp(const uint8_t *b, size_t n, struct lowpan_udp *u) {
    if (!lowpan_parse_iphc(b, n, &u->ip) || !u->ip.nh_compressed) {
        return false;
    }
    size_t i = u->ip.len;
    if (i >= n || (b[i] & 0xF8u) != 0xF0u) { /* 11110CPP */
        return false;
    }
    unsigned nhc = b[i++], pp = nhc & 3u;
    if (i + udp_port_bytes(pp) > n) {
        return false;
    }
    switch (pp) {
        case 0:
            u->sport = (uint16_t)(b[i] << 8 | b[i + 1]);
            u->dport = (uint16_t)(b[i + 2] << 8 | b[i + 3]);
            break;
        case 1:
            u->sport = (uint16_t)(b[i] << 8 | b[i + 1]);
            u->dport = (uint16_t)(0xF000u | b[i + 2]);
            break;
        case 2:
            u->sport = (uint16_t)(0xF000u | b[i]);
            u->dport = (uint16_t)(b[i + 1] << 8 | b[i + 2]);
            break;
        default:
            u->sport = (uint16_t)(0xF0B0u | (b[i] >> 4));
            u->dport = (uint16_t)(0xF0B0u | (b[i] & 0x0Fu));
            break;
    }
    i += udp_port_bytes(pp);
    u->has_cksum = ((nhc >> 2) & 1u) == 0;
    u->cksum     = 0;
    if (u->has_cksum) {
        if (i + 2 > n) {
            return false;
        }
        u->cksum = (uint16_t)(b[i] << 8 | b[i + 1]);
        i += 2;
    }
    u->hdr_len     = i;
    u->payload     = b + i;
    u->payload_len = n - i;
    return true;
}

/* --- Addresses and checksums ---------------------------------------------- */
void lowpan_iid_from_wire(const uint8_t wire[8], uint8_t iid[8]) {
    for (unsigned i = 0; i < 8; i++) {
        iid[i] = wire[7 - i];
    }
    iid[0] ^= 0x02u;
}

void lowpan_wire_from_iid(const uint8_t iid[8], uint8_t wire[8]) {
    for (unsigned i = 0; i < 8; i++) {
        wire[i] = iid[7 - i];
    }
    wire[7] ^= 0x02u;
}

bool lowpan_addr_ipv6(const struct lowpan_addr *a, const uint8_t l2_wire[8], uint8_t out[16]) {
    memset(out, 0, 16);
    if (a->ctx == LOWPAN_CTX_STATELESS) {
        out[0] = 0xFE;
        out[1] = 0x80;
    } else if (a->ctx == 0) {
        memcpy(out, ctx0_prefix, 8);
    } else {
        return false;
    }
    if (a->has_iid) {
        memcpy(out + 8, a->iid, 8);
    } else {
        lowpan_iid_from_wire(l2_wire, out + 8);
    }
    return true;
}

static uint32_t sum16(uint32_t sum, const uint8_t *p, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i += 2) {
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    }
    if (n & 1u) {
        sum += (uint32_t)p[n - 1] << 8;
    }
    return sum;
}

static uint16_t fold(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Pseudo-header sum: source, destination, upper-layer length, next header. */
static uint32_t pseudo(const uint8_t src[16], const uint8_t dst[16], uint32_t len, uint8_t nh) {
    uint32_t sum = sum16(0, src, 16);
    sum          = sum16(sum, dst, 16);
    return sum + (len >> 16) + (len & 0xFFFFu) + nh;
}

uint16_t lowpan_udp_checksum(const uint8_t  src[16],
                             const uint8_t  dst[16],
                             uint16_t       sport,
                             uint16_t       dport,
                             const uint8_t *payload,
                             size_t         len) {
    uint32_t ulen = 8u + (uint32_t)len;
    uint32_t sum  = pseudo(src, dst, ulen, LOWPAN_NH_UDP);
    sum += sport;
    sum += dport;
    sum += ulen & 0xFFFFu;
    /* The 8-byte UDP header keeps the payload on an even offset, so the
     * payload can be summed on its own. */
    sum        = sum16(sum, payload, len);
    uint16_t c = fold(sum);
    return c ? c : 0xFFFFu;
}

size_t lowpan_build_udp_ll(uint8_t       *out,
                           size_t         cap,
                           const uint8_t  src_wire[8],
                           const uint8_t  dst_wire[8],
                           uint16_t       sport,
                           uint16_t       dport,
                           const uint8_t *payload,
                           size_t         len) {
    if (len > cap || cap - len < LOWPAN_LL_HDR_LEN) {
        return 0;
    }
    static const struct lowpan_addr ll = {.ctx = LOWPAN_CTX_STATELESS};
    uint8_t                         src[16], dst[16];
    (void)lowpan_addr_ipv6(&ll, src_wire, src);
    (void)lowpan_addr_ipv6(&ll, dst_wire, dst);
    uint16_t ck = lowpan_udp_checksum(src, dst, sport, dport, payload, len);
    memmove(out + LOWPAN_LL_HDR_LEN, payload, len);
    out[0] = IPHC_LL0;
    out[1] = IPHC_LL1;
    out[2] = NHC_UDP_IN;
    out[3] = (uint8_t)(sport >> 8);
    out[4] = (uint8_t)sport;
    out[5] = (uint8_t)(dport >> 8);
    out[6] = (uint8_t)dport;
    out[7] = (uint8_t)(ck >> 8);
    out[8] = (uint8_t)ck;
    return LOWPAN_LL_HDR_LEN + len;
}

/* --- Fragmentation -------------------------------------------------------- */
/* FRAGN offsets count 8-byte units of the uncompressed datagram, where the
 * 9 compressed header bytes stand for 48. FRAG1 therefore carries 89
 * datagram bytes (48 - 9 + 89 = 128). A FRAGN carries 88, except the last
 * one, which takes whatever is left up to 92 (freedo sixlowpan_frag.go:
 * 26-50). */
#define FRAG1_PART 89u
#define FRAGN_PART 88u
#define FRAGN_LAST (LOWPAN_FRAME_MAX - FRAGN_HDR)
_Static_assert(FRAG1_PART + FRAG1_HDR <= LOWPAN_FRAME_MAX, "FRAG1 fits a Data frame");
_Static_assert((LOWPAN_UNCOMP_HDR - LOWPAN_LL_HDR_LEN + FRAG1_PART) % 8u == 0,
               "FRAG1 ends on a unit");
_Static_assert(FRAGN_PART == (FRAGN_LAST & ~7u), "FRAGN part is whole units");

static bool fragmentable(const uint8_t *pkt, size_t n) {
    return n >= LOWPAN_LL_HDR_LEN && n <= LOWPAN_PKT_MAX && pkt[0] == IPHC_LL0 && pkt[1] == IPHC_LL1
           && pkt[2] == NHC_UDP_IN;
}

/* Start and length of fragment idx >= 1 in the datagram; false past the end. */
static bool fragn_span(size_t n, unsigned idx, size_t *start, size_t *part) {
    size_t at = FRAG1_PART;
    for (unsigned i = 1; at < n; i++) {
        size_t len = n - at <= FRAGN_LAST ? n - at : FRAGN_PART;
        if (i == idx) {
            *start = at;
            *part  = len;
            return true;
        }
        at += len;
    }
    return false;
}

unsigned lowpan_frag_count(const uint8_t *pkt, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (n <= LOWPAN_FRAME_MAX) {
        return 1;
    }
    if (!fragmentable(pkt, n)) {
        return 0;
    }
    unsigned count = 1;
    size_t   start, part;
    while (fragn_span(n, count, &start, &part)) {
        count++;
    }
    return count;
}

size_t lowpan_frag_body(
    const uint8_t *pkt, size_t n, uint16_t tag, unsigned idx, uint8_t out[LOWPAN_FRAME_MAX]) {
    size_t start = 0, part = FRAG1_PART;
    if (n <= LOWPAN_FRAME_MAX || !fragmentable(pkt, n)
        || (idx > 0 && !fragn_span(n, idx, &start, &part))) {
        return 0;
    }
    size_t size = n + LOWPAN_UNCOMP_HDR - LOWPAN_LL_HDR_LEN;
    out[1]      = (uint8_t)size;
    out[2]      = (uint8_t)(tag >> 8);
    out[3]      = (uint8_t)tag;
    if (idx == 0) {
        out[0] = (uint8_t)(0xC0u | (size >> 8));
        memcpy(out + FRAG1_HDR, pkt, FRAG1_PART);
        return FRAG1_HDR + FRAG1_PART;
    }
    out[0] = (uint8_t)(0xE0u | (size >> 8));
    out[4] = (uint8_t)((start + LOWPAN_UNCOMP_HDR - LOWPAN_LL_HDR_LEN) / 8u);
    memcpy(out + FRAGN_HDR, pkt + start, part);
    return FRAGN_HDR + part;
}

bool lowpan_is_fragment(const uint8_t *body, size_t len) {
    return len >= 1 && ((body[0] & 0xF8u) == 0xC0u || (body[0] & 0xF8u) == 0xE0u);
}

/* --- Reassembly ----------------------------------------------------------- */
static void expire(struct lowpan_reasm *r, uint32_t now_ms) {
    for (unsigned i = 0; i < LOWPAN_REASM_SLOTS; i++) {
        struct lowpan_reasm_slot *s = &r->slot[i];
        if (s->used && (uint32_t)(now_ms - s->started_ms) >= LOWPAN_REASM_TIMEOUT_MS) {
            s->used = false;
        }
    }
}

unsigned lowpan_reasm_pending(struct lowpan_reasm *r, uint32_t now_ms) {
    unsigned n = 0;
    expire(r, now_ms);
    for (unsigned i = 0; i < LOWPAN_REASM_SLOTS; i++) {
        n += r->slot[i].used;
    }
    return n;
}

static void slot_start(struct lowpan_reasm_slot *s,
                       const uint8_t             src[8],
                       uint16_t                  size,
                       uint16_t                  tag,
                       uint32_t                  now_ms) {
    s->used       = true;
    s->have_first = false;
    memcpy(s->src, src, 8);
    s->size       = size;
    s->tag        = tag;
    s->started_ms = now_ms;
    s->npieces    = 0;
    s->coap1      = 0;
    s->hdr_len    = 0;
}

/* The datagram's slot; with create, a free one, else the oldest. */
static struct lowpan_reasm_slot *slot_find(
    struct lowpan_reasm *r, const uint8_t src[8], uint16_t size, uint16_t tag, uint32_t now_ms) {
    struct lowpan_reasm_slot *free_slot = NULL, *oldest = &r->slot[0];
    for (unsigned i = 0; i < LOWPAN_REASM_SLOTS; i++) {
        struct lowpan_reasm_slot *s = &r->slot[i];
        if (!s->used) {
            free_slot = free_slot ? free_slot : s;
            continue;
        }
        if (s->size == size && s->tag == tag && memcmp(s->src, src, 8) == 0) {
            return s;
        }
        if ((uint32_t)(now_ms - s->started_ms) > (uint32_t)(now_ms - oldest->started_ms)) {
            oldest = s;
        }
    }
    struct lowpan_reasm_slot *s = free_slot ? free_slot : oldest;
    slot_start(s, src, size, tag, now_ms);
    return s;
}

/* Accepts an exact retransmission, refuses any other overlap (freedo
 * addPiece). Returns false on an overlap or a full piece table. */
static bool
add_piece(struct lowpan_reasm_slot *s, uint16_t off, const uint8_t *data, uint16_t len) {
    for (unsigned i = 0; i < s->npieces; i++) {
        uint16_t poff = s->piece[i].off, plen = s->piece[i].len;
        if (off == poff && len == plen && memcmp(s->buf + off, data, len) == 0) {
            return true;
        }
        if (off < poff + plen && off + len > poff) {
            return false;
        }
    }
    if (s->npieces == LOWPAN_REASM_PIECES) {
        return false;
    }
    memcpy(s->buf + off, data, len);
    s->piece[s->npieces].off = off;
    s->piece[s->npieces].len = len;
    s->npieces++;
    return true;
}

/* Complete when FRAG1 is there and the pieces continue it without a gap up
 * to the datagram size (freedo reassemblyComplete). */
static bool complete(struct lowpan_reasm_slot *s) {
    if (!s->have_first) {
        return false;
    }
    for (unsigned i = 1; i < s->npieces; i++) { /* insertion sort by offset */
        for (unsigned j = i; j > 0 && s->piece[j - 1].off > s->piece[j].off; j--) {
            uint16_t o = s->piece[j].off, l = s->piece[j].len;
            s->piece[j]         = s->piece[j - 1];
            s->piece[j - 1].off = o;
            s->piece[j - 1].len = l;
        }
    }
    unsigned want = LOWPAN_UNCOMP_HDR + s->coap1;
    for (unsigned i = 0; i < s->npieces; i++) {
        if (s->piece[i].off != want) {
            return false;
        }
        want += s->piece[i].len;
    }
    return want == s->size;
}

enum lowpan_reasm_result lowpan_reasm_add(struct lowpan_reasm *r,
                                          const uint8_t        src[8],
                                          const uint8_t       *body,
                                          size_t               len,
                                          uint32_t             now_ms,
                                          const uint8_t      **dgram,
                                          size_t              *dgram_len) {
    expire(r, now_ms);
    if (len < FRAG1_HDR || !lowpan_is_fragment(body, len)) {
        return LOWPAN_REASM_REFUSED;
    }
    bool     first = (body[0] & 0xF8u) == 0xC0u;
    uint16_t size  = (uint16_t)((body[0] << 8 | body[1]) & 0x07FFu);
    uint16_t tag   = (uint16_t)(body[2] << 8 | body[3]);
    if ((!first && len < FRAGN_HDR) || size < LOWPAN_UNCOMP_HDR || size > LOWPAN_IPV6_MAX) {
        return LOWPAN_REASM_REFUSED;
    }

    struct lowpan_reasm_slot *s;
    if (first) {
        const uint8_t    *f1 = body + FRAG1_HDR;
        size_t            n1 = len - FRAG1_HDR;
        struct lowpan_udp u;
        if (!lowpan_parse_udp(f1, n1, &u) || u.hdr_len > LOWPAN_UNCOMP_HDR
            || u.payload_len + LOWPAN_UNCOMP_HDR > size) {
            return LOWPAN_REASM_REFUSED;
        }
        s = slot_find(r, src, size, tag, now_ms);
        if (s->have_first
            && (s->hdr_len != u.hdr_len || s->coap1 != u.payload_len
                || memcmp(s->hdr, f1, u.hdr_len) != 0
                || memcmp(s->buf + LOWPAN_UNCOMP_HDR, u.payload, u.payload_len) != 0)) {
            /* A different first fragment reuses the tag for a new datagram. */
            slot_start(s, src, size, tag, now_ms);
        }
        for (unsigned i = 0; i < s->npieces; i++) { /* freedo firstFragmentOverlaps */
            if (s->piece[i].off < LOWPAN_UNCOMP_HDR + u.payload_len) {
                s->used = false;
                return LOWPAN_REASM_REFUSED;
            }
        }
        memcpy(s->hdr, f1, u.hdr_len);
        s->hdr_len = (uint8_t)u.hdr_len;
        s->coap1   = (uint16_t)u.payload_len;
        memcpy(s->buf + LOWPAN_UNCOMP_HDR, u.payload, u.payload_len);
        s->have_first = true;
    } else {
        uint16_t off = (uint16_t)(body[4] * 8u);
        size_t   n   = len - FRAGN_HDR;
        if (off < LOWPAN_UNCOMP_HDR || off + n > size) {
            return LOWPAN_REASM_REFUSED;
        }
        s = slot_find(r, src, size, tag, now_ms);
        if ((s->have_first && off < LOWPAN_UNCOMP_HDR + s->coap1)
            || !add_piece(s, off, body + FRAGN_HDR, (uint16_t)n)) {
            s->used = false; /* an overlap drops the whole datagram */
            return LOWPAN_REASM_REFUSED;
        }
    }

    if (!complete(s)) {
        return LOWPAN_REASM_HELD;
    }
    s->used = false;
    memcpy(s->buf + LOWPAN_UNCOMP_HDR - s->hdr_len, s->hdr, s->hdr_len);
    *dgram     = s->buf + LOWPAN_UNCOMP_HDR - s->hdr_len;
    *dgram_len = (size_t)s->hdr_len + s->size - LOWPAN_UNCOMP_HDR;
    return LOWPAN_REASM_DONE;
}

/* --- Router Advertisement ------------------------------------------------- */
size_t lowpan_build_ra(uint8_t      *out,
                       size_t        cap,
                       const uint8_t src_wire[8],
                       const uint8_t gateway[4],
                       const uint8_t dns[4]) {
    if (cap < LOWPAN_RA_LEN) {
        return 0;
    }
    /* IPHC 7b 3b: TF elided, next header inline (ICMPv6), hop limit 255,
     * source from the L2 address, destination ff02::1 as one byte. */
    out[0]     = 0x7B;
    out[1]     = 0x3B;
    out[2]     = LOWPAN_NH_ICMPV6;
    out[3]     = 0x01;
    uint8_t *m = out + 4;
    memset(m, 0, LOWPAN_RA_LEN - 4u);
    m[0]  = 134;  /* Router Advertisement */
    m[4]  = 64;   /* hop limit */
    m[5]  = 0x40; /* other configuration */
    m[6]  = 1800u >> 8;
    m[7]  = 1800u & 0xFFu; /* router lifetime, s */
    m[16] = 1;             /* source link-layer address, 16 bytes */
    m[17] = 2;
    for (unsigned i = 0; i < 8; i++) {
        m[18 + i] = src_wire[7 - i];
    }
    m[32] = 5; /* MTU 1500 */
    m[33] = 1;
    m[38] = 1500u >> 8;
    m[39] = 1500u & 0xFFu;
    m[40] = 25; /* RFC 8106 layout carrying the IPv4 side: netmask, gateway, DNS */
    m[41] = 5;
    memset(m + 44, 0xFF, 4);
    m[58] = 0xFF;
    m[59] = 0xFF;
    memcpy(m + 60, gateway, 4);
    m[74] = 0xFF;
    m[75] = 0xFF;
    memcpy(m + 76, dns, 4);

    static const struct lowpan_addr ll = {.ctx = LOWPAN_CTX_STATELESS};
    uint8_t                         src[16], dst[16] = {0xFF, 0x02};
    dst[15] = 0x01;
    (void)lowpan_addr_ipv6(&ll, src_wire, src);
    uint32_t msg_len = LOWPAN_RA_LEN - 4u;
    uint16_t ck      = fold(sum16(pseudo(src, dst, msg_len, LOWPAN_NH_ICMPV6), m, msg_len));
    m[2]             = (uint8_t)(ck >> 8);
    m[3]             = (uint8_t)ck;
    return LOWPAN_RA_LEN;
}
