/* TLS 1.2 client over lwIP raw TCP (#15). Design: docs/design.md "TLS
 * settings" and "TLS and WebSocket (#15)"; mbedTLS options:
 * src/tado_mbedtls_config.h.
 *
 * Why not lwIP's altcp_tls_mbedtls: it builds its own entropy context and a
 * second CTR_DRBG (vendor/lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c:88-96,
 * 740-750), allocates from lwIP's 6 KB heap (altcp_tls_mbedtls_mem.c:165-198)
 * and runs the whole handshake from inside TCP callbacks. Here the one DRBG
 * of src/rng.c is used, mbedTLS allocates from the FreeRTOS heap, and the
 * handshake advances one step per net-task pass. */
#include "tls.h"

#include "log.h"
#include "provision.h"
#include "rng.h"
#include "timebase.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

/* ssl.h tests MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT with #if after
 * config_adjust_ssl.h has undefined it (vendor/mbedtls/include/mbedtls/
 * ssl.h:659, config_adjust_ssl.h:54). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "mbedtls/net_sockets.h" /* MBEDTLS_ERR_NET_* codes only; net_sockets.c is not built */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#pragma GCC diagnostic pop

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define CONNECT_TIMEOUT_MS 10000u

/* --- mbedTLS allocator: the FreeRTOS heap ---------------------------------- */
/* pvPortMalloc(0) returns NULL and calls the malloc-failed hook, which
 * panics (src/hooks.c), so a zero-size request gets a dummy block. A real
 * shortage still panics: the heap is sized for the handshake peak
 * (docs/build.md, P4 TLS memory). */
static uint8_t zero_block;

void *tls_calloc(size_t n, size_t size) {
    if (n == 0u || size == 0u) {
        return &zero_block;
    }
    return pvPortCalloc(n, size);
}

void tls_free(void *p) {
    if (p != NULL && p != &zero_block) {
        vPortFree(p);
    }
}

/* --- State ------------------------------------------------------------------ */
static mbedtls_x509_crt    ca;
static mbedtls_ssl_config  conf;
static mbedtls_ssl_context ssl;
static bool                configured, ssl_live;

static enum tls_state  state;
static struct tcp_pcb *pcb;
static struct pbuf    *rxq; /* received, not yet given to mbedTLS */
static bool            peer_fin;
static TickType_t      t_open, t_connected;
/* Handshake figures for the log (#16, #17). */
static struct {
    uint32_t tcp_ms;          /* SYN to connected */
    uint32_t handshake_ms;    /* connected to handshake done, wall clock */
    uint32_t cpu_ms;          /* time inside the handshake steps */
    uint32_t longest_step_ms; /* longest single step: the longest the net task is blocked */
    uint32_t steps;
} timing;

static const char *err_where = "none";
static int         err_code;

static void fail(const char *where, int code) {
    if (state != TLS_FAILED) {
        err_where = where;
        err_code  = code;
    }
    state = TLS_FAILED;
}

const char *tls_error(int *code) {
    *code = err_code;
    return err_where;
}

/* --- lwIP callbacks (run inside the net task, from netif input or timers) --- */
static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    (void)tpcb;
    if (p == NULL) {
        peer_fin = true; /* FIN: whatever is queued is still read */
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return ERR_OK;
    }
    /* Window updates (tcp_recved) only once mbedTLS has taken the bytes, so
     * the queue never exceeds TCP_WND (src/lwipopts.h). */
    if (rxq == NULL) {
        rxq = p;
    } else {
        pbuf_cat(rxq, p);
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    pcb = NULL; /* lwIP has freed it */
    fail("tcp", err);
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    (void)tpcb;
    if (err != ERR_OK) {
        fail("tcp connect", err);
        return ERR_OK;
    }
    t_connected   = xTaskGetTickCount();
    timing.tcp_ms = (uint32_t)(t_connected - t_open);
    state         = TLS_HANDSHAKE;
    return ERR_OK;
}

/* --- mbedTLS transport ------------------------------------------------------ */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    if (pcb == NULL) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    size_t room = tcp_sndbuf(pcb);
    if (room == 0u || tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    size_t n   = len < room ? len : room;
    err_t  err = tcp_write(pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) {
        return MBEDTLS_ERR_SSL_WANT_WRITE; /* lwIP heap or segment pool full: later */
    }
    if (err != ERR_OK) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    (void)tcp_output(pcb);
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    if (rxq == NULL) {
        if (peer_fin) {
            return 0; /* EOF: mbedTLS reports MBEDTLS_ERR_SSL_CONN_EOF */
        }
        return pcb != NULL ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET;
    }
    u16_t n = pbuf_copy_partial(rxq, buf, (u16_t)(len < rxq->tot_len ? len : rxq->tot_len), 0);
    rxq     = pbuf_free_header(rxq, n);
    if (pcb != NULL) {
        tcp_recved(pcb, n);
    }
    return (int)n;
}

/* --- API ------------------------------------------------------------------- */
bool tls_setup(void) {
#ifndef PROVISION_CA_PEM
    LOG("tls: no trust anchor built in (SERVER_CA_PEM in config/local.cmake, docs/build.md P4)");
    return false;
#else
    static const char pem[] = PROVISION_CA_PEM;
    mbedtls_x509_crt_init(&ca);
    /* PEM input must include the terminating NUL (x509_crt.h). */
    int ret = mbedtls_x509_crt_parse(&ca, (const unsigned char *)pem, sizeof pem);
    if (ret != 0) {
        LOG("tls: trust anchor does not parse (-0x%04x)", (unsigned)-ret);
        return false;
    }
    char subject[96];
    mbedtls_x509_dn_gets(subject, sizeof subject, &ca.subject);
    LOG("tls: trust anchor %s, P-256 %s",
        subject,
        mbedtls_pk_can_do(&ca.pk, MBEDTLS_PK_ECKEY) ? "yes" : "NO");

    mbedtls_ssl_config_init(&conf);
    ret = mbedtls_ssl_config_defaults(
        &conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOG("tls: config defaults failed (-0x%04x)", (unsigned)-ret);
        return false;
    }
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, rng_random, NULL);
    /* RFC 6066 4: ask for records of at most 2048 bytes, the smaller buffer
     * (src/tado_mbedtls_config.h); within the 4 KB receive buffer. */
    ret = mbedtls_ssl_conf_max_frag_len(&conf, MBEDTLS_SSL_MAX_FRAG_LEN_2048);
    if (ret != 0) {
        LOG("tls: max_fragment_length not accepted (-0x%04x)", (unsigned)-ret);
        return false;
    }
    configured = true;
    return true;
#endif
}

bool tls_open(const ip_addr_t *ip, uint16_t port, const char *host) {
    if (!configured || state != TLS_IDLE) {
        return false;
    }
    memset(&timing, 0, sizeof timing);
    err_where = "none";
    err_code  = 0;
    peer_fin  = false;

    mbedtls_ssl_init(&ssl);
    ssl_live = true;
    int ret  = mbedtls_ssl_setup(&ssl, &conf); /* allocates the record buffers */
    if (ret == 0) {
        ret = mbedtls_ssl_set_hostname(&ssl, host); /* checked against subjectAltName */
    }
    if (ret != 0) {
        fail("ssl setup", ret);
        return true; /* reported through tls_poll() */
    }
    mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);

    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        fail("tcp_new", ERR_MEM);
        return true;
    }
    tcp_nagle_disable(pcb); /* small WebSocket messages go out at once */
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    t_open    = xTaskGetTickCount();
    state     = TLS_CONNECTING;
    err_t err = tcp_connect(pcb, ip, port, on_connected);
    if (err != ERR_OK) {
        fail("tcp_connect", err);
    }
    return true;
}

enum tls_state tls_poll(bool *busy) {
    *busy = false;
    switch (state) {
        case TLS_CONNECTING:
            if ((int32_t)(xTaskGetTickCount() - t_open)
                > (int32_t)pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
                fail("tcp connect timeout", ERR_TIMEOUT);
            }
            break;
        case TLS_HANDSHAKE: {
            uint32_t t0  = timebase_us();
            int      ret = mbedtls_ssl_handshake_step(&ssl);
            uint32_t ms  = (timebase_us() - t0 + 500u) / 1000u;
            timing.cpu_ms += ms;
            timing.steps++;
            if (ms > timing.longest_step_ms) {
                timing.longest_step_ms = ms;
            }
            if (ret == 0) {
                *busy = true;
                if (mbedtls_ssl_is_handshake_over(&ssl)) {
                    timing.handshake_ms = (uint32_t)(xTaskGetTickCount() - t_connected);
                    state               = TLS_OPEN;
                    LOG("tls: %s %s in %lu ms after TCP connect (%lu ms): %lu ms CPU in %lu steps, "
                        "longest step %lu ms; records in/out up to %d/%d bytes; heap free %u, min "
                        "%u",
                        mbedtls_ssl_get_version(&ssl),
                        mbedtls_ssl_get_ciphersuite(&ssl),
                        (unsigned long)timing.handshake_ms,
                        (unsigned long)timing.tcp_ms,
                        (unsigned long)timing.cpu_ms,
                        (unsigned long)timing.steps,
                        (unsigned long)timing.longest_step_ms,
                        mbedtls_ssl_get_max_in_record_payload(&ssl),
                        mbedtls_ssl_get_max_out_record_payload(&ssl),
                        (unsigned)xPortGetFreeHeapSize(),
                        (unsigned)xPortGetMinimumEverFreeHeapSize());
                }
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                    /* Flags from x509_crt.h, e.g. 0x04 CN_MISMATCH, 0x08 NOT_TRUSTED. */
                    LOG("tls: server certificate rejected, verify flags %08lx",
                        (unsigned long)mbedtls_ssl_get_verify_result(&ssl));
                }
                fail("handshake", ret);
            } else if (rxq != NULL) {
                *busy = true; /* data waiting: step again at once */
            }
            break;
        }
        default:
            break;
    }
    return state;
}

int tls_read(uint8_t *buf, size_t len) {
    if (state != TLS_OPEN) {
        return -1;
    }
    int ret = mbedtls_ssl_read(&ssl, buf, len);
    if (ret > 0) {
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    fail(ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? "closed by server" : "read", ret);
    return -1;
}

int tls_write(const uint8_t *buf, size_t len) {
    if (state != TLS_OPEN) {
        return -1;
    }
    int ret = mbedtls_ssl_write(&ssl, buf, len);
    if (ret >= 0) {
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    fail("write", ret);
    return -1;
}

void tls_close(void) {
    if (ssl_live) {
        if (state == TLS_OPEN && pcb != NULL) {
            (void)mbedtls_ssl_close_notify(&ssl); /* best effort, one try */
        }
        mbedtls_ssl_free(&ssl); /* frees the record buffers and the handshake */
        ssl_live = false;
    }
    if (pcb != NULL) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
        pcb = NULL;
    }
    if (rxq != NULL) {
        pbuf_free(rxq);
        rxq = NULL;
    }
    state = TLS_IDLE;
}
