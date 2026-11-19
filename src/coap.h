#ifndef COAP_H
#define COAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal CoAP (RFC 7252) message codec and tado TLV helpers (#15): what
 * the bridge's own requests and replies need. Relayed device messages are
 * passed through as bytes and never parsed here. */

#define COAP_CON 0u
#define COAP_NON 1u
#define COAP_ACK 2u
#define COAP_RST 3u

#define COAP_CODE(c, dd) ((uint8_t)(((c) << 5) | (dd)))
#define COAP_GET         COAP_CODE(0, 1)
#define COAP_POST        COAP_CODE(0, 2)
#define COAP_PUT         COAP_CODE(0, 3)
#define COAP_CHANGED     COAP_CODE(2, 4)
#define COAP_CONTENT     COAP_CODE(2, 5)
#define COAP_CONTINUE    COAP_CODE(2, 31) /* RFC 7959 2.9.1 */
#define COAP_BAD_REQUEST COAP_CODE(4, 0)
#define COAP_UNAUTH      COAP_CODE(4, 1)
#define COAP_FORBIDDEN   COAP_CODE(4, 3)
#define COAP_NOT_FOUND   COAP_CODE(4, 4)
#define COAP_INCOMPLETE  COAP_CODE(4, 8)  /* RFC 7959 2.9.2 */
#define COAP_TOO_LARGE   COAP_CODE(4, 13) /* RFC 7959 2.9.3 */
#define COAP_INTERNAL    COAP_CODE(5, 0)
#define COAP_UNAVAILABLE COAP_CODE(5, 3)

#define COAP_OPT_URI_PATH 11u
#define COAP_OPT_BLOCK1   27u /* RFC 7959 2.1 */
/* tanoclo's OPT_VENDOR_2048: the session token from auth/token
 * (tanoclo/ws-server/lib/coap.js:52, docs/server-api.md "Frame"). */
#define COAP_OPT_SESSION 2048u

struct coap_writer {
    uint8_t *buf;
    size_t   cap, len;
    unsigned last_opt;
    bool     overflow;
};

void coap_begin(struct coap_writer *w,
                uint8_t            *buf,
                size_t              cap,
                uint8_t             type,
                uint8_t             code,
                uint16_t            mid,
                const uint8_t      *token,
                size_t              tkl);
/* Options in ascending number order. */
void coap_option(struct coap_writer *w, unsigned num, const void *val, size_t len);
void coap_payload(struct coap_writer *w, const void *data, size_t len);

struct coap_msg {
    uint8_t        type, code, tkl;
    uint16_t       mid;
    const uint8_t *token;
    const uint8_t *opts; /* encoded options, opts_len bytes */
    size_t         opts_len;
    const uint8_t *payload;
    size_t         payload_len;
};

bool coap_parse(const uint8_t *buf, size_t len, struct coap_msg *m);

/* Iterates the options: *pos starts at 0, *num at 0. False at the end or on
 * a malformed option. */
bool coap_next_option(
    const struct coap_msg *m, size_t *pos, unsigned *num, const uint8_t **val, size_t *vlen);

/* Uri-Path segments joined with '/', for the log. */
void coap_path(const struct coap_msg *m, char *out, size_t cap);

/* True if the Uri-Path options are exactly the segments of uri ("d/config":
 * two options, "d" and "config"), compared byte for byte. */
bool coap_path_is(const struct coap_msg *m, const char *uri);

/* Piggybacked response to a confirmable request (RFC 7252 5.2.1): an ACK
 * with the request's Message ID and token, the given code, no options and
 * no payload. Returns its length, or 0 if it does not fit in cap. */
size_t coap_reply(const struct coap_msg *req, uint8_t code, uint8_t *buf, size_t cap);

/* Block1 option (RFC 7959 2.2): NUM (up to 20 bits), M, SZX (block size
 * 2^(SZX + 4)); a uint of 0-3 bytes. coap_block1() reads the first Block1
 * option: false if there is none, it is longer than 3 bytes, or SZX is the
 * reserved 7. coap_block1_put() encodes it in as few bytes as possible
 * (0 for NUM 0, M 0, SZX 0) and returns the length; num must be below
 * 2^20. */
bool   coap_block1(const struct coap_msg *m, uint32_t *num, bool *more, unsigned *szx);
size_t coap_block1_put(uint32_t num, bool more, unsigned szx, uint8_t out[3]);

/* tado TLV: FID u16 big-endian, length u8, value (tanoclo/docs/coap_tlv.md:183-197). */
bool tlv_put(uint8_t *buf, size_t cap, size_t *len, uint16_t fid, const void *val, size_t vlen);
bool tlv_find(const uint8_t *buf, size_t len, uint16_t fid, const uint8_t **val, size_t *vlen);

#endif
