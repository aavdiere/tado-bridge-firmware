/* 6LoWPAN as the tado 868 MHz link uses it (#17): IPHC and UDP-NHC header
 * parsing, the link-local datagram the bridge sends, the UDP checksum, and
 * RFC 4944 fragmentation and reassembly. Ported from freedo (Apache-2.0, see
 * lowpan.c). Hardware independent, no allocation; tested on the host.
 *
 * A "datagram" here is the compressed packet inside a Data frame's
 * plaintext, between the 8-byte shim and the 2-byte FCS (src/mac.h).
 * Addresses are wire EUI-64s (reversed, ending c5 1b 00). */
#ifndef LOWPAN_H
#define LOWPAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOWPAN_CTX_STATELESS (-1) /* fe80::/64 */
#define LOWPAN_CTX_UNUSABLE  (-2) /* a form this link never uses (multicast, 16-bit, ...) */

/* One endpoint as the IPHC header left it. */
struct lowpan_addr {
    int8_t  ctx;     /* LOWPAN_CTX_*, or a 6LoWPAN context index (0 = aaaa::/64) */
    bool    has_iid; /* false: elided, take it from the frame's L2 address */
    uint8_t iid[8];
};

#define LOWPAN_NH_UDP    17u
#define LOWPAN_NH_ICMPV6 58u

struct lowpan_iphc {
    struct lowpan_addr src, dst;
    bool               dst_multicast;
    bool               nh_compressed; /* next header is LOWPAN_NHC */
    uint8_t            next_header;   /* when not compressed */
    uint8_t            hop_limit;
    size_t             len; /* bytes of IPHC header */
};

/* False if b is not a LOWPAN_IPHC header or is truncated. */
bool lowpan_parse_iphc(const uint8_t *b, size_t n, struct lowpan_iphc *h);

struct lowpan_udp {
    struct lowpan_iphc ip;
    uint16_t           sport, dport;
    bool               has_cksum;
    uint16_t           cksum;
    size_t             hdr_len; /* IPHC and UDP-NHC together */
    const uint8_t     *payload; /* inside b */
    size_t             payload_len;
};

/* IPHC followed by LOWPAN_NHC UDP. False for anything else. */
bool lowpan_parse_udp(const uint8_t *b, size_t n, struct lowpan_udp *u);

/* Interface identifier of a node: its wire EUI-64 reversed with the
 * universal/local bit flipped (freedo tado/coap.go:83-97), and back. */
void lowpan_iid_from_wire(const uint8_t wire[8], uint8_t iid[8]);
void lowpan_wire_from_iid(const uint8_t iid[8], uint8_t wire[8]);

/* The 16-byte address of an endpoint; l2_wire supplies an elided IID.
 * False for a form that cannot be expanded (only fe80::/64 and context 0,
 * aaaa::/64, exist on this link, freedo tado/coap.go:23-41). */
bool lowpan_addr_ipv6(const struct lowpan_addr *a, const uint8_t l2_wire[8], uint8_t out[16]);

/* RFC 768 checksum over the IPv6 pseudo-header, the UDP header and the
 * payload; a result of 0 is sent as 0xffff. */
uint16_t lowpan_udp_checksum(const uint8_t  src[16],
                             const uint8_t  dst[16],
                             uint16_t       sport,
                             uint16_t       dport,
                             const uint8_t *payload,
                             size_t         len);

/* The datagram the bridge sends: IPHC 7e 33 (both addresses link-local and
 * elided, hop limit 64), UDP-NHC f0, both ports and the checksum inline
 * (freedo tado/build_coap.go:15-40). Returns its length, 9 + len, or 0 if
 * it does not fit cap. */
#define LOWPAN_LL_HDR_LEN 9u
size_t lowpan_build_udp_ll(uint8_t       *out,
                           size_t         cap,
                           const uint8_t  src_wire[8],
                           const uint8_t  dst_wire[8],
                           uint16_t       sport,
                           uint16_t       dport,
                           const uint8_t *payload,
                           size_t         len);

/* --- Fragmentation (RFC 4944) --------------------------------------------- */
#define LOWPAN_FRAME_MAX  97u   /* datagram bytes one Data frame carries (MAC_DATA_MAX_DGRAM) */
#define LOWPAN_IPV6_MAX   1280u /* devices refuse larger datagrams (freedo sixlowpan_frag.go:10) */
#define LOWPAN_UNCOMP_HDR 48u   /* IPv6 + UDP headers the compressed ones stand for */
/* Largest link-local datagram that can be fragmented: 1280 - 48 + 9. */
#define LOWPAN_PKT_MAX (LOWPAN_IPV6_MAX - LOWPAN_UNCOMP_HDR + LOWPAN_LL_HDR_LEN)

/* Number of Data frames a link-local datagram of pkt_len bytes needs: 1 if
 * it fits one frame, 0 if it cannot be sent (too large, or not the 7e 33 f0
 * form whose header sizes the fragment offsets assume). */
unsigned lowpan_frag_count(const uint8_t *pkt, size_t pkt_len);

/* The body of fragment idx (0 = FRAG1) of a datagram that needs more than
 * one frame. Returns its length, at most LOWPAN_FRAME_MAX, or 0. */
size_t lowpan_frag_body(
    const uint8_t *pkt, size_t pkt_len, uint16_t tag, unsigned idx, uint8_t out[LOWPAN_FRAME_MAX]);

bool lowpan_is_fragment(const uint8_t *body, size_t len);

/* --- Reassembly ----------------------------------------------------------- */
#define LOWPAN_REASM_SLOTS      3u
#define LOWPAN_REASM_PIECES     16u    /* FRAGNs of one datagram: at most 14 */
#define LOWPAN_REASM_TIMEOUT_MS 60000u /* RFC 4944 (freedo sixlowpan_reasm.go:17) */

struct lowpan_reasm_slot {
    bool     used, have_first;
    uint8_t  src[8];
    uint16_t size, tag;
    uint32_t started_ms;
    uint8_t  hdr[LOWPAN_UNCOMP_HDR]; /* compressed headers of FRAG1 */
    uint8_t  hdr_len;
    uint16_t coap1; /* payload bytes FRAG1 carried */
    uint8_t  npieces;
    struct {
        uint16_t off, len; /* uncompressed offset, bytes */
    } piece[LOWPAN_REASM_PIECES];
    /* Payload at buf + 48, by uncompressed offset; the compressed headers
     * are copied in front of it when the datagram completes. */
    uint8_t buf[LOWPAN_IPV6_MAX];
};

struct lowpan_reasm {
    struct lowpan_reasm_slot slot[LOWPAN_REASM_SLOTS];
};

enum lowpan_reasm_result {
    LOWPAN_REASM_REFUSED, /* not a usable fragment, or an overlap dropped the datagram */
    LOWPAN_REASM_HELD,
    LOWPAN_REASM_DONE, /* *dgram, *dgram_len: the whole datagram, valid until the next call */
};

/* Files one fragment body from src. now_ms is any monotonic millisecond
 * clock (wrap-safe). */
enum lowpan_reasm_result lowpan_reasm_add(struct lowpan_reasm *r,
                                          const uint8_t        src[8],
                                          const uint8_t       *body,
                                          size_t               len,
                                          uint32_t             now_ms,
                                          const uint8_t      **dgram,
                                          size_t              *dgram_len);

/* Datagrams still incomplete (after dropping expired ones). */
unsigned lowpan_reasm_pending(struct lowpan_reasm *r, uint32_t now_ms);

/* --- Router Advertisement ------------------------------------------------- */
/* The Router Advertisement the bridge broadcasts to ff02::1, as a
 * compressed packet for mac_build_broadcast() (freedo tado/broadcast.go:
 * 189-240). gateway and dns are IPv4 addresses in network order. Returns
 * the length (84) or 0 if cap is too small. */
#define LOWPAN_RA_LEN 84u
size_t lowpan_build_ra(uint8_t      *out,
                       size_t        cap,
                       const uint8_t src_wire[8],
                       const uint8_t gateway[4],
                       const uint8_t dns[4]);

#endif
