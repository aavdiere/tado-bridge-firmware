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
#define COAP_CONTENT     COAP_CODE(2, 5)
#define COAP_UNAUTH      COAP_CODE(4, 1)

#define COAP_OPT_URI_PATH 11u
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

/* tado TLV: FID u16 big-endian, length u8, value (tanoclo/docs/coap_tlv.md:183-197). */
bool tlv_put(uint8_t *buf, size_t cap, size_t *len, uint16_t fid, const void *val, size_t vlen);
bool tlv_find(const uint8_t *buf, size_t len, uint16_t fid, const uint8_t **val, size_t *vlen);

#endif
