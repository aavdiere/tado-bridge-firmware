/* WebSocket client (#15), RFC 6455, over src/tls.c. Design: docs/design.md
 * "TLS and WebSocket (#15)". */
#include "ws.h"

#include "log.h"
#include "rng.h"
#include "tls.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Liveness [I]: tanoclo's uWebSockets server has idleTimeout 0, so it never
 * pings (tanoclo/ws-server/server.js:404-408). The client pings after
 * PING_IDLE_MS without any frame from the server, and gives up if nothing
 * arrives within PONG_TIMEOUT_MS after that. Stock timing is unknown
 * (docs/open-questions.md row 14). */
#define PING_IDLE_MS    30000u
#define PONG_TIMEOUT_MS 10000u
#define UPGRADE_WAIT_MS 10000u
#define CLOSE_WAIT_MS   2000u
#define HTTP_HEAD_MAX   512u

/* RFC 6455 5.2 opcodes */
#define OP_CONT   0x0u
#define OP_TEXT   0x1u
#define OP_BINARY 0x2u
#define OP_CLOSE  0x8u
#define OP_PING   0x9u
#define OP_PONG   0xAu

/* RFC 6455 7.4.1 status codes */
#define CLOSE_PROTOCOL_ERROR 1002u
#define CLOSE_TOO_BIG        1009u

static enum ws_state state;
static ws_message_fn handler;
static const char   *req_host, *req_path;
static char          accept_expected[29]; /* base64 of a SHA-1: 28 characters */
static bool          request_sent;

static uint8_t rx[WS_RX_MAX + 14]; /* largest frame header is 14 bytes */
static size_t  rx_len;             /* bytes in rx */
static size_t  asm_len;            /* payload of an unfinished fragmented message, at rx[0] */
static uint8_t asm_op;             /* its opcode, 0 if none */
static uint8_t tx[WS_TX_MAX];
static size_t  tx_len;

static TickType_t t_state, last_rx, ping_sent;
static bool       ping_outstanding, server_closed;
static char       reason[64];

/* True once more than ms have passed since `since`. Reads the tick here, not
 * from a copy taken earlier in the pass: handlers called during the pass
 * stamp newer ticks, and an unsigned difference against an older copy would
 * wrap and fire every timeout at once. */
static bool elapsed(TickType_t since, uint32_t ms) {
    return (int32_t)(xTaskGetTickCount() - since) > (int32_t)pdMS_TO_TICKS(ms);
}

static void set_state(enum ws_state s) {
    state   = s;
    t_state = xTaskGetTickCount();
}

static void finish(const char *why) {
    if (reason[0] == '\0') {
        snprintf(reason, sizeof reason, "%s", why);
    }
    tls_close();
    set_state(WS_CLOSED);
}

const char *ws_reason(void) {
    return reason;
}

/* Queue one masked frame (RFC 6455 5.2, 5.3): every client frame is masked
 * with a fresh random key from the DRBG. */
static bool queue_frame(uint8_t opcode, const uint8_t *data, size_t len) {
    size_t hdr = 2u + 4u + (len > 125u ? 2u : 0u);
    if (len > 0xFFFFu || tx_len + hdr + len > sizeof tx) {
        return false;
    }
    uint8_t *f = tx + tx_len;
    f[0]       = (uint8_t)(0x80u | opcode); /* FIN */
    size_t i   = 2;
    if (len > 125u) {
        f[1]   = 0x80u | 126u;
        f[i++] = (uint8_t)(len >> 8);
        f[i++] = (uint8_t)len;
    } else {
        f[1] = (uint8_t)(0x80u | len);
    }
    uint8_t *mask = f + i;
    rng_bytes(mask, 4);
    i += 4u;
    for (size_t k = 0; k < len; k++) {
        f[i + k] = data[k] ^ mask[k & 3u];
    }
    tx_len += hdr + len;
    return true;
}

static void flush(void) {
    while (tx_len > 0u) {
        int n = tls_write(tx, tx_len);
        if (n <= 0) {
            if (n < 0) {
                int         code;
                const char *where = tls_error(&code);
                char        b[48];
                snprintf(b, sizeof b, "tls %s (%d)", where, code);
                finish(b);
            }
            return;
        }
        memmove(tx, tx + n, tx_len - (size_t)n);
        tx_len -= (size_t)n;
    }
}

static void protocol_error(uint16_t code, const char *why) {
    snprintf(reason, sizeof reason, "%s", why);
    ws_close(code);
}

/* --- Opening handshake (RFC 6455 4.1, 4.2.2) -------------------------------- */
static void send_upgrade(void) {
    uint8_t nonce[16];
    char    key[25];
    size_t  n;
    rng_bytes(nonce, sizeof nonce);
    (void)mbedtls_base64_encode((unsigned char *)key, sizeof key, &n, nonce, sizeof nonce);
    key[n] = '\0';

    /* Accept value = base64(SHA-1(key + GUID)) (RFC 6455 4.2.2 item 5.4). */
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char              cat[24 + sizeof guid];
    unsigned char     digest[20];
    memcpy(cat, key, 24);
    memcpy(cat + 24, guid, sizeof guid - 1);
    (void)mbedtls_sha1((const unsigned char *)cat, 24 + sizeof guid - 1, digest);
    (void)mbedtls_base64_encode(
        (unsigned char *)accept_expected, sizeof accept_expected, &n, digest, sizeof digest);
    accept_expected[n] = '\0';

    /* HTTP/1.1, not the 1.0 that stock probably sends (docs/server-api.md
     * "Transport"). Subprotocol "binary": tanoclo/ws-server/lib/proxy-manager.js:197-202. */
    int len      = snprintf((char *)tx,
                            sizeof tx,
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: %s\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Sec-WebSocket-Protocol: binary\r\n"
                            "\r\n",
                            req_path,
                            req_host,
                            key);
    tx_len       = (size_t)len;
    request_sent = true;
    flush();
}

static bool name_is(const char *line, size_t len, const char *name) {
    size_t n = strlen(name);
    if (len <= n || line[n] != ':') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

static void header_value(const char *line, size_t len, const char **v, size_t *vlen) {
    const char *p   = (const char *)memchr(line, ':', len) + 1;
    const char *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *v    = p;
    *vlen = (size_t)(end - p);
}

/* Returns true once the response head is complete and checked. */
static bool read_upgrade_response(void) {
    const uint8_t *end = NULL;
    for (size_t i = 3; i < rx_len; i++) {
        if (rx[i - 3] == '\r' && rx[i - 2] == '\n' && rx[i - 1] == '\r' && rx[i] == '\n') {
            end = rx + i + 1;
            break;
        }
    }
    if (end == NULL) {
        if (rx_len >= HTTP_HEAD_MAX) {
            finish("upgrade: response head too long");
        }
        return false;
    }
    const char *p      = (const char *)rx;
    const char *stop   = (const char *)end - 2;
    bool        status = false, upgrade = false, accept = false, proto_ok = true;
    for (bool first = true; p < stop; first = false) {
        const char *eol = p;
        while (eol < stop && !(eol[0] == '\r' && eol[1] == '\n')) {
            eol++;
        }
        size_t      len = (size_t)(eol - p);
        const char *v;
        size_t      vlen;
        if (first) {
            /* "HTTP/1.1 101 Switching Protocols" */
            status = len >= 12 && memcmp(p, "HTTP/1.", 7) == 0 && memcmp(p + 8, " 101", 4) == 0;
            if (!status) {
                LOG("ws: upgrade refused: %.*s", (int)(len < 60 ? len : 60), p);
            }
        } else if (name_is(p, len, "upgrade")) {
            header_value(p, len, &v, &vlen);
            upgrade = vlen == 9 && strncasecmp(v, "websocket", 9) == 0;
        } else if (name_is(p, len, "sec-websocket-accept")) {
            header_value(p, len, &v, &vlen);
            accept = vlen == strlen(accept_expected) && memcmp(v, accept_expected, vlen) == 0;
        } else if (name_is(p, len, "sec-websocket-protocol")) {
            header_value(p, len, &v, &vlen);
            proto_ok = vlen == 6 && memcmp(v, "binary", 6) == 0; /* only what was asked for */
        }
        p = eol + 2;
    }
    size_t head = (size_t)(end - rx);
    memmove(rx, end, rx_len - head); /* any bytes after the head are frames */
    rx_len -= head;
    if (!status || !upgrade || !accept || !proto_ok) {
        LOG("ws: upgrade response rejected: status 101 %s, Upgrade %s, Accept %s, protocol %s",
            status ? "yes" : "no",
            upgrade ? "ok" : "missing",
            accept ? "ok" : "wrong",
            proto_ok ? "ok" : "not binary");
        finish("upgrade rejected");
        return false;
    }
    return true;
}

/* --- Frames ----------------------------------------------------------------- */
static void on_control(uint8_t op, const uint8_t *data, size_t len) {
    switch (op) {
        case OP_PING:
            (void)queue_frame(OP_PONG, data, len); /* RFC 6455 5.5.3: same payload */
            break;
        case OP_PONG:
            break; /* liveness only; any frame clears the outstanding ping */
        case OP_CLOSE: {
            unsigned code = len >= 2 ? ((unsigned)data[0] << 8 | data[1]) : 1005u;
            if (state == WS_CLOSING) {
                finish(reason[0] ? reason : "closed");
                return;
            }
            snprintf(reason, sizeof reason, "closed by server, code %u", code);
            /* Echo the code (RFC 6455 5.5.1), then close TLS once it is sent. */
            uint8_t echo[2] = {(uint8_t)(code >> 8), (uint8_t)code};
            (void)queue_frame(OP_CLOSE, echo, len >= 2 ? 2u : 0u);
            server_closed = true;
            set_state(WS_CLOSING);
            break;
        }
        default:
            break;
    }
}

/* Parses complete frames from rx[asm_len..rx_len). */
static void read_frames(void) {
    while (state == WS_OPEN || state == WS_CLOSING) {
        size_t   off   = asm_len;
        size_t   avail = rx_len - off;
        uint8_t *f     = rx + off;
        if (avail < 2u) {
            return;
        }
        bool    fin    = (f[0] & 0x80u) != 0u;
        uint8_t op     = f[0] & 0x0Fu;
        bool    masked = (f[1] & 0x80u) != 0u;
        size_t  len    = f[1] & 0x7Fu;
        size_t  hdr    = 2;
        if ((f[0] & 0x70u) != 0u || masked) {
            /* No extensions were negotiated, and a server must not mask
             * (RFC 6455 5.1, 5.2). */
            protocol_error(CLOSE_PROTOCOL_ERROR, "frame with RSV bits or mask");
            return;
        }
        if (len == 126u) {
            if (avail < 4u) {
                return;
            }
            len = (size_t)f[2] << 8 | f[3];
            hdr = 4;
        } else if (len == 127u) {
            if (avail < 10u) {
                return;
            }
            if (f[2] | f[3] | f[4] | f[5] | f[6] | f[7]) {
                protocol_error(CLOSE_TOO_BIG, "message too big");
                return;
            }
            len = (size_t)f[8] << 8 | f[9];
            hdr = 10;
        }
        bool control = (op & 0x8u) != 0u;
        if (control && (!fin || len > 125u)) {
            protocol_error(CLOSE_PROTOCOL_ERROR, "bad control frame");
            return;
        }
        if (!control && asm_len + len > WS_RX_MAX) {
            protocol_error(CLOSE_TOO_BIG, "message too big");
            return;
        }
        if (avail < hdr + len) {
            return; /* incomplete: wait for more */
        }
        last_rx                = xTaskGetTickCount();
        ping_outstanding       = false;
        const uint8_t *payload = f + hdr;

        if (control) {
            on_control(op, payload, len);
        } else if ((op == OP_CONT) != (asm_op != 0u)) {
            protocol_error(CLOSE_PROTOCOL_ERROR, "bad fragment sequence");
            return;
        } else {
            /* Move the payload down so fragments join at rx[0]. */
            memmove(rx + asm_len, payload, len);
            asm_len += len;
            if (op != OP_CONT) {
                asm_op = op;
            }
            size_t used = hdr + len;
            memmove(rx + asm_len, rx + asm_len - len + used, rx_len - off - used);
            rx_len -= hdr;
            if (fin) {
                if (asm_op == OP_BINARY && handler != NULL && state == WS_OPEN) {
                    handler(rx, asm_len);
                } else if (asm_op != OP_BINARY) {
                    LOG("ws: %u-byte message with opcode %u ignored",
                        (unsigned)asm_len,
                        (unsigned)asm_op);
                }
                memmove(rx, rx + asm_len, rx_len - asm_len);
                rx_len -= asm_len;
                asm_len = 0;
                asm_op  = 0;
            }
            continue;
        }
        size_t used = hdr + len;
        memmove(f, f + used, avail - used);
        rx_len -= used;
    }
}

/* --- API ------------------------------------------------------------------- */
bool ws_open(const ip_addr_t *ip,
             uint16_t         port,
             const char      *host,
             const char      *path,
             ws_message_fn    on_message) {
    tls_close();
    rx_len = asm_len = tx_len = 0;
    asm_op                    = 0;
    request_sent = ping_outstanding = server_closed = false;
    reason[0]                                       = '\0';
    req_host                                        = host;
    req_path                                        = path;
    handler                                         = on_message;
    if (!tls_open(ip, port, host)) {
        finish("tls not set up");
        return false;
    }
    set_state(WS_CONNECTING);
    return true;
}

void ws_close(uint16_t code) {
    if (state == WS_OPEN) {
        uint8_t c[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        if (reason[0] == '\0') {
            snprintf(reason, sizeof reason, "closed by bridge, code %u", (unsigned)code);
        }
        (void)queue_frame(OP_CLOSE, c, sizeof c);
        set_state(WS_CLOSING);
        flush();
    } else if (state != WS_CLOSED && state != WS_IDLE && state != WS_CLOSING) {
        finish("closed by bridge before open");
    }
}

bool ws_send(const uint8_t *data, size_t len) {
    if (state != WS_OPEN || !queue_frame(OP_BINARY, data, len)) {
        return false;
    }
    flush();
    return true;
}

enum ws_state ws_poll(bool *busy) {
    *busy = false;
    if (state == WS_IDLE || state == WS_CLOSED) {
        return state;
    }
    bool           tls_busy;
    enum tls_state ts = tls_poll(&tls_busy);
    *busy             = tls_busy;

    if (ts == TLS_FAILED) {
        int         code;
        const char *where = tls_error(&code);
        char        b[48];
        snprintf(b, sizeof b, "tls %s (%d)", where, code);
        finish(b);
        return state;
    }
    if (state == WS_CONNECTING) {
        if (ts != TLS_OPEN) {
            return state;
        }
        set_state(WS_UPGRADING);
        last_rx = xTaskGetTickCount();
    }
    if (state == WS_UPGRADING && !request_sent) {
        send_upgrade();
    }
    flush();

    /* Read whatever TLS has decrypted. */
    for (;;) {
        if (state == WS_CLOSED || rx_len >= sizeof rx) {
            break;
        }
        int n = tls_read(rx + rx_len, sizeof rx - rx_len);
        if (n < 0) {
            int         code;
            const char *where = tls_error(&code);
            char        b[48];
            snprintf(b, sizeof b, "tls %s (%d)", where, code);
            finish(b);
            return state;
        }
        if (n == 0) {
            break;
        }
        rx_len += (size_t)n;
        *busy = true;
        if (state == WS_UPGRADING) {
            if (!read_upgrade_response()) {
                continue;
            }
            set_state(WS_OPEN);
            last_rx = xTaskGetTickCount();
        }
        read_frames();
    }
    if (state == WS_OPEN || state == WS_CLOSING) {
        read_frames(); /* frames that arrived with the upgrade response */
    }
    if (state == WS_CLOSED) {
        return state;
    }
    flush();

    switch (state) {
        case WS_UPGRADING:
            if (elapsed(t_state, UPGRADE_WAIT_MS)) {
                finish("no upgrade response");
            }
            break;
        case WS_OPEN:
            if (ping_outstanding) {
                if (elapsed(ping_sent, PONG_TIMEOUT_MS)) {
                    finish("no pong from server");
                }
            } else if (elapsed(last_rx, PING_IDLE_MS)) {
                if (queue_frame(OP_PING, NULL, 0)) {
                    ping_outstanding = true;
                    ping_sent        = xTaskGetTickCount();
                    flush();
                }
            }
            break;
        case WS_CLOSING:
            if ((server_closed && tx_len == 0u) || elapsed(t_state, CLOSE_WAIT_MS)) {
                finish(reason[0] ? reason : "closed");
            }
            break;
        default:
            break;
    }
    return state;
}
