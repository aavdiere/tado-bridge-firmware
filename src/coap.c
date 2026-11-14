/* Minimal CoAP codec (RFC 7252 section 3) and tado TLV (#15). */
#include "coap.h"

#include <string.h>

static void put(struct coap_writer *w, const void *d, size_t n) {
    if (w->overflow || w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, d, n);
    w->len += n;
}

static void put8(struct coap_writer *w, uint8_t v) {
    put(w, &v, 1);
}

void coap_begin(struct coap_writer *w,
                uint8_t            *buf,
                size_t              cap,
                uint8_t             type,
                uint8_t             code,
                uint16_t            mid,
                const uint8_t      *token,
                size_t              tkl) {
    *w = (struct coap_writer){.buf = buf, .cap = cap};
    /* Ver 1, T, TKL; Code; Message ID (RFC 7252 3). */
    put8(w, (uint8_t)(0x40u | (type << 4) | (tkl & 0x0Fu)));
    put8(w, code);
    put8(w, (uint8_t)(mid >> 8));
    put8(w, (uint8_t)mid);
    put(w, token, tkl);
}

/* Delta or length nibble with 1- or 2-byte extension (RFC 7252 3.1). */
static uint8_t nibble(unsigned v, uint8_t ext[2], size_t *ext_len) {
    if (v < 13u) {
        *ext_len = 0;
        return (uint8_t)v;
    }
    if (v < 269u) {
        ext[0]   = (uint8_t)(v - 13u);
        *ext_len = 1;
        return 13u;
    }
    v -= 269u;
    ext[0]   = (uint8_t)(v >> 8);
    ext[1]   = (uint8_t)v;
    *ext_len = 2;
    return 14u;
}

void coap_option(struct coap_writer *w, unsigned num, const void *val, size_t len) {
    uint8_t de[2], le[2];
    size_t  dn, ln;
    uint8_t d = nibble(num - w->last_opt, de, &dn);
    uint8_t l = nibble((unsigned)len, le, &ln);
    put8(w, (uint8_t)(d << 4 | l));
    put(w, de, dn);
    put(w, le, ln);
    put(w, val, len);
    w->last_opt = num;
}

void coap_payload(struct coap_writer *w, const void *data, size_t len) {
    if (len > 0u) {
        put8(w, 0xFFu);
        put(w, data, len);
    }
}

/* Reads a nibble's value; false if it runs past the end or is 15. */
static bool ext_value(unsigned nib, const uint8_t *p, size_t end, size_t *pos, unsigned *v) {
    if (nib < 13u) {
        *v = nib;
        return true;
    }
    if (nib == 13u && *pos + 1u <= end) {
        *v = 13u + p[*pos];
        *pos += 1u;
        return true;
    }
    if (nib == 14u && *pos + 2u <= end) {
        *v = 269u + ((unsigned)p[*pos] << 8 | p[*pos + 1u]);
        *pos += 2u;
        return true;
    }
    return false;
}

bool coap_parse(const uint8_t *buf, size_t len, struct coap_msg *m) {
    if (len < 4u || (buf[0] >> 6) != 1u) {
        return false;
    }
    *m      = (struct coap_msg){0};
    m->type = (buf[0] >> 4) & 3u;
    m->tkl  = buf[0] & 0x0Fu;
    m->code = buf[1];
    m->mid  = (uint16_t)(buf[2] << 8 | buf[3]);
    if (m->tkl > 8u || 4u + m->tkl > len) {
        return false;
    }
    m->token   = buf + 4;
    size_t pos = 4u + m->tkl;
    m->opts    = buf + pos;
    /* Walk the options to find the payload marker. */
    while (pos < len && buf[pos] != 0xFFu) {
        unsigned d, l;
        uint8_t  b = buf[pos++];
        if (!ext_value(b >> 4, buf, len, &pos, &d) || !ext_value(b & 0x0Fu, buf, len, &pos, &l)
            || pos + l > len) {
            return false;
        }
        pos += l;
    }
    m->opts_len = (size_t)(buf + pos - m->opts);
    if (pos < len) {
        pos++; /* 0xFF */
        if (pos == len) {
            return false; /* marker without payload is a format error (RFC 7252 3) */
        }
        m->payload     = buf + pos;
        m->payload_len = len - pos;
    }
    return true;
}

bool coap_next_option(
    const struct coap_msg *m, size_t *pos, unsigned *num, const uint8_t **val, size_t *vlen) {
    if (*pos >= m->opts_len) {
        return false;
    }
    unsigned d, l;
    uint8_t  b = m->opts[(*pos)++];
    if (!ext_value(b >> 4, m->opts, m->opts_len, pos, &d)
        || !ext_value(b & 0x0Fu, m->opts, m->opts_len, pos, &l) || *pos + l > m->opts_len) {
        return false;
    }
    *num += d;
    *val  = m->opts + *pos;
    *vlen = l;
    *pos += l;
    return true;
}

void coap_path(const struct coap_msg *m, char *out, size_t cap) {
    size_t         pos = 0, n = 0, vlen;
    unsigned       num = 0;
    const uint8_t *val;
    out[0] = '\0';
    while (coap_next_option(m, &pos, &num, &val, &vlen)) {
        if (num != COAP_OPT_URI_PATH) {
            continue;
        }
        if (n > 0u && n + 1u < cap) {
            out[n++] = '/';
        }
        for (size_t i = 0; i < vlen && n + 1u < cap; i++) {
            char c   = (char)val[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        out[n] = '\0';
    }
}

bool tlv_put(uint8_t *buf, size_t cap, size_t *len, uint16_t fid, const void *val, size_t vlen) {
    if (vlen > 255u || *len + 3u + vlen > cap) {
        return false;
    }
    uint8_t *p = buf + *len;
    p[0]       = (uint8_t)(fid >> 8);
    p[1]       = (uint8_t)fid;
    p[2]       = (uint8_t)vlen;
    memcpy(p + 3, val, vlen);
    *len += 3u + vlen;
    return true;
}

bool tlv_find(const uint8_t *buf, size_t len, uint16_t fid, const uint8_t **val, size_t *vlen) {
    size_t pos = 0;
    while (pos + 3u <= len) {
        uint16_t f = (uint16_t)(buf[pos] << 8 | buf[pos + 1u]);
        size_t   l = buf[pos + 2u];
        if (pos + 3u + l > len) {
            return false;
        }
        if (f == fid) {
            *val  = buf + pos + 3u;
            *vlen = l;
            return true;
        }
        pos += 3u + l;
    }
    return false;
}
