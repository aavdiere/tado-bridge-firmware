/* MAC frames of the tado 868 MHz link (#17): classify, build and parse the
 * wake-up frame, the Enhanced ACK, and the sealed (CCM*) Data and broadcast
 * frames. Ported from freedo (Apache-2.0, see mac.c). Hardware independent:
 * no radio, no RTOS, no allocation; tested on the host (tests/host).
 *
 * Frames here never include the radio's own CRC-16 (the chip appends and
 * strips it, docs/radio.md "Checksums"). Addresses are in wire order: the
 * EUI-64 reversed, so every node's address ends in c5 1b 00
 * (docs/radio.md "Source address tail"). */
#ifndef MAC_H
#define MAC_H

#include "ccm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAC_EUI_LEN   8u
#define MAC_PAN_ID    0xABCDu /* wake-up frames (freedo tado/mac.go:23) */
#define MAC_CSL_UNITS 3125u   /* CSL period in 320 us units: 1.000 s (mac.go:24) */

#define MAC_HDR_LEN   16u /* sealed frames: nonce = hdr[0:13], AAD = hdr[0:16] */
#define MAC_MIC_LEN   CCM_MIC_LEN
#define MAC_FCS_LEN   2u
#define MAC_WAKE_LEN  12u
#define MAC_ACK_LEN   29u
#define MAC_FRAME_MAX 127u /* longest frame freedo builds (tado/frame.go:79) */
#define MAC_SHIM_LEN  8u   /* Data plaintext starts c5 1b 00 | 04 | counter(4 LE) */
/* Largest datagram one Data frame carries: 127 - 16 - 8 - 2 - 4. */
#define MAC_DATA_MAX_DGRAM (MAC_FRAME_MAX - MAC_HDR_LEN - MAC_SHIM_LEN - MAC_FCS_LEN - MAC_MIC_LEN)
/* Largest packet one broadcast carries: 127 - 16 - 2 (counter high) - 2 - 4. */
#define MAC_BCAST_MAX_PKT (MAC_FRAME_MAX - MAC_HDR_LEN - 2u - MAC_FCS_LEN - MAC_MIC_LEN)

enum mac_class {
    MAC_UNKNOWN,
    MAC_DATA,           /* 69 ec: unicast Data, secured, ACK requested */
    MAC_DATA_PENDING,   /* 79 ec: the same with Frame Pending */
    MAC_BROADCAST,      /* 49 e8, or 59 e8 with Frame Pending */
    MAC_ENH_ACK,        /* 42 ee: Enhanced ACK, not secured */
    MAC_WAKEUP,         /* 25 ..: multipurpose wake-up */
    MAC_BEACON_REQUEST, /* 0b e8: Enhanced Beacon Request (pairing, #19) */
    MAC_BEACON,         /* 18 ec: Enhanced Beacon (pairing, #19) */
};

enum mac_class mac_classify(const uint8_t *frame, size_t len);

/* Software MAC FCS: CRC-16/CCITT reflected (poly 0x8408), init 0, no final
 * XOR, sent low byte first (freedo tado/build_mac.go:104-121). crc chains
 * calls; start with 0. */
uint16_t mac_fcs(uint16_t crc, const uint8_t *p, size_t n);

/* The fixed tail of every node's wire address (freedo tado/net.go:11). */
extern const uint8_t mac_eui_suffix[3];

/* --- Wake-up -------------------------------------------------------------- */
/* 25 seq | PAN(2 LE) | dst short(2) | 82 0e | rendezvous(2 LE) | 80 3f.
 * rendezvous counts 320 us units until the Data frame. */
void mac_build_wakeup(uint8_t       out[MAC_WAKE_LEN],
                      uint8_t       seq,
                      const uint8_t dst_short[2],
                      uint16_t      rendezvous);

struct mac_wakeup {
    uint8_t  seq;
    uint16_t pan;
    uint8_t  dst_short[2];
    uint16_t rendezvous;
};
bool mac_parse_wakeup(const uint8_t *frame, size_t len, struct mac_wakeup *w);

/* --- Enhanced ACK --------------------------------------------------------- */
/* 42 ee seq | dst(8) | src(8) | 04 0d | phase(2 LE) | 35 0c | 80 3f | FCS(2). */
void mac_build_enh_ack(uint8_t       out[MAC_ACK_LEN],
                       uint8_t       seq,
                       const uint8_t dst[MAC_EUI_LEN],
                       const uint8_t src[MAC_EUI_LEN],
                       uint16_t      phase);

struct mac_enh_ack {
    uint8_t  seq;
    uint8_t  dst[MAC_EUI_LEN], src[MAC_EUI_LEN];
    uint16_t phase, period;
};
/* False unless 29 bytes, FCF 42 ee and a matching FCS. */
bool mac_parse_enh_ack(const uint8_t *frame, size_t len, struct mac_enh_ack *a);

/* The Enhanced ACK that answers a unicast Data frame, from that frame's
 * first 16 bytes alone, so it can be built while the frame is still arriving
 * (docs/radio.md "ACK prebuild"). False, and nothing written, unless the
 * frame is a unicast Data frame (69 ec or 79 ec) addressed to own. */
bool mac_ack_for_data(uint8_t       out[MAC_ACK_LEN],
                      const uint8_t data_hdr[MAC_HDR_LEN],
                      const uint8_t own[MAC_EUI_LEN],
                      uint16_t      phase);

/* Is ack the Enhanced ACK that closes our Data frame data (at least its 16
 * header bytes)? Checks length, FCF, seq, both addresses and the FCS
 * (freedo cslAckValid, cc1101_bridge.ino:1216-1224). */
bool mac_ack_answers(const uint8_t *ack, size_t ack_len, const uint8_t data_hdr[MAC_HDR_LEN]);

/* --- Sealed frames -------------------------------------------------------- */
/* hdr(16) | CCM*( body | FCS(2) ) | MIC(4), the FCS over hdr | body. Data,
 * broadcast and beacon frames all have this shape (freedo tado/frame.go,
 * build_mac.go, beacon.go). */

/* Writes the frame to out; returns its length, or 0 if it does not fit cap
 * or MAC_FRAME_MAX, or the key is not set. */
size_t mac_seal(const struct ccm_key *k,
                const uint8_t         hdr[MAC_HDR_LEN],
                const uint8_t        *body,
                size_t                body_len,
                uint8_t              *out,
                size_t                cap);

/* Decrypts a sealed frame into body (cap bytes), checking the MIC and then
 * the FCS. Returns false on any failure; *body_len excludes the FCS. */
enum mac_open_result { MAC_OPEN_OK, MAC_OPEN_MALFORMED, MAC_OPEN_MIC, MAC_OPEN_FCS };
enum mac_open_result mac_open(const struct ccm_key *k,
                              const uint8_t        *frame,
                              size_t                len,
                              uint8_t              *body,
                              size_t                cap,
                              size_t               *body_len);

/* Unicast Data: 69 ec (79 ec with pending) seq | dst(8) | src[0:5] | sealed
 * [c5 1b 00 | 04 | counter(4 LE) | datagram]. Returns the frame length or 0. */
size_t mac_build_data(const struct ccm_key *k,
                      uint8_t              *out,
                      size_t                cap,
                      bool                  pending,
                      uint8_t               seq,
                      const uint8_t         dst[MAC_EUI_LEN],
                      const uint8_t         src[MAC_EUI_LEN],
                      uint32_t              counter,
                      const uint8_t        *dgram,
                      size_t                dgram_len);

struct mac_data {
    uint8_t        seq;
    bool           pending;
    uint8_t        dst[MAC_EUI_LEN], src[MAC_EUI_LEN];
    uint32_t       counter;
    const uint8_t *dgram; /* inside the caller's body buffer */
    size_t         dgram_len;
};
/* Opens a unicast Data frame. body is scratch of at least len - 20 bytes
 * and holds the plaintext afterwards. The source address is the 5 header
 * bytes plus the fixed tail; like freedo (tado/frame.go:90-124) this does
 * not compare the tail copy inside the plaintext. */
enum mac_open_result mac_open_data(const struct ccm_key *k,
                                   const uint8_t        *frame,
                                   size_t                len,
                                   uint8_t              *body,
                                   size_t                cap,
                                   struct mac_data      *d);

/* Broadcast Data: 49 e8 (59 e8 with pending) seq | ff ff | src(8) | 04 |
 * counter low(2 LE) | sealed [counter high(2 LE) | packet]. */
size_t mac_build_broadcast(const struct ccm_key *k,
                           uint8_t              *out,
                           size_t                cap,
                           bool                  pending,
                           uint8_t               seq,
                           const uint8_t         src[MAC_EUI_LEN],
                           uint32_t              counter,
                           const uint8_t        *pkt,
                           size_t                pkt_len);

#endif
