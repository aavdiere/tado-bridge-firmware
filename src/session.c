/* Server session (#15). Protocol: docs/server-api.md; design:
 * docs/design.md "TLS and WebSocket (#15)". What tanoclo expects, from its
 * code (tanoclo/ws-server/...):
 * - Every message is a 28-byte frame plus CoAP (lib/ws-bridge.js:11-15).
 * - POST auth/key and POST auth/token carry the full serial as TLV 0x0260
 *   (lib/handlers.js:118-128, 214-226). The serial must be a row in the
 *   `devices` table (getDeviceByFullSerial, lib/db-devices.js:58-70), or the
 *   server does not answer at all (handlers.js:129-133, 228-232). auth/token
 *   then needs the serial or its home in `websocket_whitelist`, unless that
 *   table is empty (lib/db-auth.js:45-52), else 4.01 (handlers.js:246-251).
 * - Replies are piggybacked ACKs with code 2.05 (handlers.js:154, 309).
 * - GET time: 2.05 with 0D + u32le Unix seconds (lib/coap.js:503-508). */
#include "session.h"

#include "board.h"
#include "coap.h"
#include "log.h"
#include "provision.h"
#include "rng.h"
#include "tls.h"
#include "ws.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#ifndef NET_SERVER_HOST
#error "NET_SERVER_HOST not defined (cmake/CMakeLists.txt)"
#endif
#ifndef NET_SERVER_PORT
#error "NET_SERVER_PORT not defined (cmake/CMakeLists.txt)"
#endif
#define SERVER_PATH "/hw/v2" /* tanoclo/ws-server/server.js:404 */

#define BACKOFF_MIN_MS 2000u
#define BACKOFF_MAX_MS 60000u
#define STABLE_MS      60000u /* ready this long: the next failure starts at BACKOFF_MIN_MS again */
#define REQUEST_TIMEOUT_MS 10000u
#define LED_BLINK_MS       250u

/* 28-byte frame (docs/server-api.md "Frame"). */
#define FRAME_HDR     28u
#define DIR_UPLINK    0x0001u
#define DIR_DOWNLINK  0x0002u
#define COAP_UDP_PORT 5683u
#define COAP_MAX      256u /* the bridge's own requests are small */

/* TLV field IDs (docs/server-api.md "Bridge sends"). */
#define FID_SERIAL   0x0260u
#define FID_KEY      0x0261u
#define FID_TOKEN    0x025Eu
#define FID_LIFETIME 0x025Fu
#define TIME_TAG     0x0Du /* protobuf field 1, fixed32 (tanoclo/ws-server/lib/coap.js:505) */

enum phase { P_OFF, P_WAIT, P_CONNECT, P_AUTH_KEY, P_AUTH_TOKEN, P_TIME, P_READY };
static const char *const phase_name[] = {
    "off", "wait", "connect", "auth/key", "auth/token", "time", "ready"};

static enum phase phase;
static bool       have_identity;
static uint8_t    bridge_ipv6[16];
static TickType_t t_phase, t_attempt, retry_at;
static uint32_t   backoff_ms = BACKOFF_MIN_MS;
static uint32_t   attempts, connects;
static bool       led_blink;
static TickType_t next_blink;

static uint16_t mid;
static uint16_t pending_mid;
static uint8_t  pending_token[4];
static bool     pending;

static uint8_t session_token[8];
static bool    have_token;

static bool       time_valid;
static uint32_t   time_unix;
static TickType_t time_tick;

static const char *serial(void) {
#ifdef PROVISION_SERIAL
    return PROVISION_SERIAL;
#else
    return "";
#endif
}

/* Fresh tick, signed difference: see elapsed() in src/ws.c. */
static bool elapsed(TickType_t since, uint32_t ms) {
    return (int32_t)(xTaskGetTickCount() - since) > (int32_t)pdMS_TO_TICKS(ms);
}

static void set_phase(enum phase p) {
    phase   = p;
    t_phase = xTaskGetTickCount();
}

/* Link-local address from the EUI-64, universal/local bit inverted (RFC
 * 4291 2.5.1, appendix A). The address stock uses for its own messages is
 * unknown (docs/open-questions.md row 10) [I]; tanoclo keeps whatever the
 * bridge sends as its address (handlers.js:269, 289-291). */
static bool identity_init(void) {
#if defined(PROVISION_SERIAL) && defined(PROVISION_EUI64)
    static const uint8_t eui[8] = PROVISION_EUI64;
    memset(bridge_ipv6, 0, sizeof bridge_ipv6);
    bridge_ipv6[0] = 0xFE;
    bridge_ipv6[1] = 0x80;
    memcpy(bridge_ipv6 + 8, eui, 8);
    bridge_ipv6[8] ^= 0x02u;
    return true;
#else
    return false;
#endif
}

/* --- Frames and requests ------------------------------------------------------ */
static bool send_coap(const uint8_t *coap, size_t n) {
    uint8_t f[FRAME_HDR + COAP_MAX];
    if (n > COAP_MAX) {
        return false;
    }
    f[0] = (uint8_t)(DIR_UPLINK >> 8);
    f[1] = (uint8_t)DIR_UPLINK;
    f[2] = 16u;
    memcpy(f + 3, bridge_ipv6, 16);
    /* field_a (u16), field_b (u8), field_c (u16): 0. tanoclo accepts zeros
     * on uplink (tanoclo/ws-server/test/test_ws_comprehensive.js:144-149);
     * their stock meaning is open (docs/open-questions.md row 10). */
    memset(f + 19, 0, 3);
    f[22] = (uint8_t)(COAP_UDP_PORT >> 8);
    f[23] = (uint8_t)COAP_UDP_PORT;
    f[24] = 0;
    f[25] = 0;
    f[26] = (uint8_t)(n >> 8);
    f[27] = (uint8_t)n;
    memcpy(f + FRAME_HDR, coap, n);
    return ws_send(f, FRAME_HDR + n);
}

/* One confirmable request at a time; the reply is matched by message ID
 * and token. uri: Uri-Path segments separated by '/'. */
static bool request(uint8_t code, const char *uri, const uint8_t *payload, size_t plen) {
    uint8_t            buf[COAP_MAX];
    struct coap_writer w;
    pending_mid = mid++;
    rng_bytes(pending_token, sizeof pending_token);
    coap_begin(
        &w, buf, sizeof buf, COAP_CON, code, pending_mid, pending_token, sizeof pending_token);
    for (const char *s = uri; *s;) {
        const char *e = strchr(s, '/');
        size_t      n = e ? (size_t)(e - s) : strlen(s);
        coap_option(&w, COAP_OPT_URI_PATH, s, n);
        s += n + (e ? 1u : 0u);
    }
    /* After auth/token, every request carries the session token as option
     * 2048. tanoclo only compares it and logs a mismatch
     * (tanoclo/ws-server/lib/message-router/index.js:461-467); its docs say
     * it is checked on every payload (docs/server-api.md, conflict table) [I]. */
    if (have_token) {
        coap_option(&w, COAP_OPT_SESSION, session_token, sizeof session_token);
    }
    coap_payload(&w, payload, plen);
    if (w.overflow || !send_coap(buf, w.len)) {
        LOG("session: %s not sent (buffer full)", uri);
        return false;
    }
    pending = true;
    return true;
}

static bool request_auth(const char *uri) {
    uint8_t     p[32];
    size_t      n = 0;
    const char *s = serial();
    if (!tlv_put(p, sizeof p, &n, FID_SERIAL, s, strlen(s))) {
        return false;
    }
    return request(COAP_POST, uri, p, n);
}

/* --- Time ----------------------------------------------------------------------- */
/* Days since 1970-01-01 to a civil date (proleptic Gregorian), H. Hinnant's
 * civil_from_days. */
static void civil(uint32_t days, unsigned *y, unsigned *m, unsigned *d) {
    uint32_t z   = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp  = (5u * doy + 2u) / 153u;
    *d           = doy - (153u * mp + 2u) / 5u + 1u;
    *m           = mp < 10u ? mp + 3u : mp - 9u;
    *y           = yoe + era * 400u + (*m <= 2u ? 1u : 0u);
}

static bool take_time(const uint8_t *p, size_t n, const char *how) {
    if (n != 5u || p[0] != TIME_TAG) {
        return false;
    }
    uint32_t t = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    uint32_t before;
    bool     had = session_unix_time(&before);
    time_unix    = t;
    time_tick    = xTaskGetTickCount();
    time_valid   = true;
    unsigned y, mo, d;
    civil(t / 86400u, &y, &mo, &d);
    uint32_t s         = t % 86400u;
    char     drift[40] = "";
    if (had) {
        snprintf(drift, sizeof drift, ", local clock was %+ld s off", (long)before - (long)t);
    }
    LOG("time: %04u-%02u-%02u %02lu:%02lu:%02lu UTC (unix %lu) from %s%s",
        y,
        mo,
        d,
        (unsigned long)(s / 3600u),
        (unsigned long)(s / 60u % 60u),
        (unsigned long)(s % 60u),
        (unsigned long)t,
        how,
        drift);
    return true;
}

bool session_unix_time(uint32_t *now) {
    if (!time_valid) {
        return false;
    }
    *now = time_unix + (uint32_t)(xTaskGetTickCount() - time_tick) / configTICK_RATE_HZ;
    return true;
}

/* --- Connection lifecycle ----------------------------------------------------- */
static void schedule_retry(const char *why) {
    TickType_t now = xTaskGetTickCount();
    if (phase == P_READY && elapsed(t_phase, STABLE_MS)) {
        backoff_ms = BACKOFF_MIN_MS;
    }
    /* 75-125 % of the backoff, so many bridges do not retry in step. */
    uint32_t delay = backoff_ms * 3u / 4u + rng_u32() % (backoff_ms / 2u + 1u);
    LOG("session: %s (in %s); retrying in %lu ms", why, phase_name[phase], (unsigned long)delay);
    retry_at   = now + pdMS_TO_TICKS(delay);
    backoff_ms = backoff_ms * 2u > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2u;
    pending    = false;
    have_token = false;
    set_phase(P_WAIT);
}

static void give_up(const char *why) {
    ws_close(1000); /* ws_poll() finishes the close while the session waits */
    schedule_retry(why);
}

static void on_reply(const struct coap_msg *m) {
    const uint8_t *v;
    size_t         vl;
    char           b[80];
    uint32_t       ms = (uint32_t)(xTaskGetTickCount() - t_phase);
    pending           = false;

    switch (phase) {
        case P_AUTH_KEY:
            if (m->code != COAP_CONTENT) {
                snprintf(b, sizeof b, "auth/key answered %u.%02u", m->code >> 5, m->code & 31u);
                give_up(b);
                return;
            }
            /* 0x0261: 16 bytes, random, AES-128-ECB under the device's
             * factory_key if tanoclo has one (handlers.js:135-147). What stock
             * does with it is open (docs/open-questions.md row 11); not used. */
            LOG("session: auth/key accepted in %lu ms (serial echo %s, 0x0261 %s)",
                (unsigned long)ms,
                tlv_find(m->payload, m->payload_len, FID_SERIAL, &v, &vl) && vl == strlen(serial())
                        && memcmp(v, serial(), vl) == 0
                    ? "ok"
                    : "differs",
                tlv_find(m->payload, m->payload_len, FID_KEY, &v, &vl) && vl == 16u ? "16 bytes"
                                                                                    : "missing");
            set_phase(P_AUTH_TOKEN);
            if (!request_auth("auth/token")) {
                give_up("auth/token not sent");
            }
            return;

        case P_AUTH_TOKEN: {
            if (m->code == COAP_UNAUTH) {
                /* Not whitelisted: wait the longest backoff. */
                backoff_ms = BACKOFF_MAX_MS;
                give_up("auth/token refused with 4.01 (serial or home not in websocket_whitelist)");
                return;
            }
            const uint8_t *lt;
            size_t         ltl;
            if (m->code != COAP_CONTENT || !tlv_find(m->payload, m->payload_len, FID_TOKEN, &v, &vl)
                || vl != sizeof session_token
                || !tlv_find(m->payload, m->payload_len, FID_LIFETIME, &lt, &ltl) || ltl != 2u) {
                snprintf(b,
                         sizeof b,
                         "auth/token answered %u.%02u without token and lifetime",
                         m->code >> 5,
                         m->code & 31u);
                give_up(b);
                return;
            }
            memcpy(session_token, v, sizeof session_token);
            have_token = true;
            /* The lifetime (1440 min from tanoclo, handlers.js:235) is logged but
             * not acted on: tanoclo does not expire tokens [I]. */
            LOG("session: auth/token accepted in %lu ms: 8-byte session token, lifetime %u min",
                (unsigned long)ms,
                (unsigned)(lt[0] << 8 | lt[1]));
            set_phase(P_TIME);
            if (!request(COAP_GET, "time", NULL, 0)) {
                give_up("time request not sent");
            }
            return;
        }

        case P_TIME:
            if (m->code != COAP_CONTENT || !take_time(m->payload, m->payload_len, "GET time")) {
                snprintf(b,
                         sizeof b,
                         "time answered %u.%02u, %u bytes",
                         m->code >> 5,
                         m->code & 31u,
                         (unsigned)m->payload_len);
                give_up(b);
                return;
            }
            LOG("session: ready: authenticated as %s, time set, %lu ms after the connect started "
                "(attempt %lu, connection %lu)",
                serial(),
                (unsigned long)(xTaskGetTickCount() - t_attempt),
                (unsigned long)attempts,
                (unsigned long)connects);
            attempts = 0;
            set_phase(P_READY);
            return;

        default:
            return;
    }
}

/* Every binary message from the server (ws.c handler). */
static void on_message(const uint8_t *d, size_t n) {
    if (n < FRAME_HDR || d[2] != 16u) {
        LOG("session: %u-byte message is not a bridge frame", (unsigned)n);
        return;
    }
    size_t          clen = (size_t)(d[26] << 8 | d[27]);
    struct coap_msg m;
    if (clen > n - FRAME_HDR || !coap_parse(d + FRAME_HDR, clen, &m)) {
        LOG("session: frame with bad CoAP (%u of %u bytes)",
            (unsigned)clen,
            (unsigned)(n - FRAME_HDR));
        return;
    }
    bool to_us = memcmp(d + 3, bridge_ipv6, 16) == 0;
    if (pending && m.type == COAP_ACK && m.mid == pending_mid && m.tkl == sizeof pending_token
        && memcmp(m.token, pending_token, sizeof pending_token) == 0) {
        on_reply(&m);
        return;
    }
    /* Time broadcast: NON 2.05 without URI every 4 h (tanoclo/ws-server/server.js:220-246). */
    if (m.type == COAP_NON && m.code == COAP_CONTENT
        && take_time(m.payload, m.payload_len, "broadcast")) {
        return;
    }
    char path[40];
    coap_path(&m, path, sizeof path);
    /* Requests for the bridge itself (d/rfkey, d/pair, ...) and for devices
     * come with P5 (docs/server-api.md "Bridge handles itself"). */
    LOG("session: downlink %s type %u code %u.%02u mid %04x /%s (%u bytes payload) not handled yet",
        to_us ? "for the bridge" : "for a device",
        (unsigned)m.type,
        (unsigned)(m.code >> 5),
        (unsigned)(m.code & 31u),
        (unsigned)m.mid,
        path,
        (unsigned)m.payload_len);
}

static void leds(void) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - next_blink) >= 0) {
        next_blink = now + pdMS_TO_TICKS(LED_BLINK_MS);
        led_blink  = !led_blink;
    }
    bool on = phase == P_READY || (phase >= P_CONNECT && led_blink);
    board_led(LED_CLOUD, on);
}

void session_poll(bool net_up, const ip_addr_t *server, bool *busy) {
    static bool started;
    *busy = false;
    if (!started) {
        started       = true;
        have_identity = identity_init();
        bool tls      = have_identity && tls_setup();
        if (!have_identity) {
            LOG("session: no bridge identity built in (BRIDGE_SERIAL and BRIDGE_EUI64 in "
                "config/local.cmake, docs/build.md P4); not connecting");
        } else if (!tls) {
            have_identity = false; /* tls.c logged why */
        } else {
            LOG("session: bridge %s, server wss://%s:%u%s",
                serial(),
                NET_SERVER_HOST,
                (unsigned)NET_SERVER_PORT,
                SERVER_PATH);
        }
        mid = (uint16_t)rng_u32(); /* random start (RFC 7252 4.4) */
    }
    if (!have_identity) {
        return;
    }

    bool wb;
    if (phase == P_OFF || phase == P_WAIT) {
        (void)ws_poll(&wb); /* finish a close handshake that is still running */
    }

    if (!net_up) {
        if (phase != P_OFF) {
            if (phase >= P_CONNECT) {
                ws_close(1001); /* going away; abandoned after CLOSE_WAIT_MS if nothing gets out */
            }
            LOG("session: network down; connection dropped (in %s)", phase_name[phase]);
            set_phase(P_OFF);
            pending    = false;
            have_token = false;
        }
        leds();
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (phase == P_OFF) {
        set_phase(P_WAIT);
        retry_at = now;
    }
    if (phase == P_WAIT) {
        if ((int32_t)(now - retry_at) >= 0) {
            attempts++;
            connects++;
            t_attempt = now;
            set_phase(P_CONNECT);
            if (!ws_open(server, NET_SERVER_PORT, NET_SERVER_HOST, SERVER_PATH, on_message)) {
                schedule_retry(ws_reason());
            }
        }
        leds();
        return;
    }

    enum ws_state s = ws_poll(&wb);
    *busy           = wb;
    if (phase < P_CONNECT) {
        /* A reply handler inside ws_poll() gave up; the close is under way. */
    } else if (s == WS_CLOSED) {
        schedule_retry(ws_reason());
    } else if (phase == P_CONNECT && s == WS_OPEN) {
        LOG("session: WebSocket open on %s after %lu ms",
            SERVER_PATH,
            (unsigned long)(xTaskGetTickCount() - t_attempt));
        set_phase(P_AUTH_KEY);
        if (!request_auth("auth/key")) {
            give_up("auth/key not sent");
        }
    } else if (pending && elapsed(t_phase, REQUEST_TIMEOUT_MS)) {
        char b[96];
        snprintf(b,
                 sizeof b,
                 "no reply to %s in %u s%s",
                 phase_name[phase],
                 REQUEST_TIMEOUT_MS / 1000u,
                 phase == P_AUTH_KEY ? "; is the serial a device in tanoclo's database?" : "");
        give_up(b);
    }
    leds();
}
