#ifndef TLS_H
#define TLS_H

#include "lwip/ip_addr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TLS 1.2 client over lwIP's raw TCP API (#15, docs/design.md "TLS
 * settings"): one connection at a time, ECDHE-ECDSA-AES128-GCM-SHA256 on
 * P-256, server certificate verified against the built-in trust anchor and
 * the server's name or address. Net task only, like lwIP (NO_SYS) and the
 * DRBG (src/rng.h): no locking.
 *
 * The handshake runs one mbedTLS step per tls_poll() call, so the net task
 * can service the ENC28J60, lwIP timers and the watchdog between the P-256
 * operations. */

enum tls_state {
    TLS_IDLE,       /* nothing open */
    TLS_CONNECTING, /* TCP SYN sent */
    TLS_HANDSHAKE,  /* TCP up, TLS handshake running */
    TLS_OPEN,       /* ready for tls_read() and tls_write() */
    TLS_FAILED,     /* closed by the peer or an error; call tls_close() */
};

/* Parse the trust anchor and set up the configuration. Once, before the
 * first tls_open(). False if the anchor is missing or does not parse. */
bool tls_setup(void);

/* Start a connection. host is the name the certificate must carry (a DNS
 * name or an address literal, matched against subjectAltName). False if
 * already open or out of memory. */
bool tls_open(const ip_addr_t *ip, uint16_t port, const char *host);

/* Advance the connection. Returns the state; *busy is set when the
 * handshake made progress and the caller should call again soon. */
enum tls_state tls_poll(bool *busy);

/* Application data. tls_read: bytes read, 0 if none waiting, -1 if the
 * connection failed or the peer closed it. tls_write: bytes taken (then
 * call again with the rest), 0 if the TCP send buffer is full (call again
 * later with the same data), -1 on failure. */
int tls_read(uint8_t *buf, size_t len);
int tls_write(const uint8_t *buf, size_t len);

/* Send close_notify if open, close or abort TCP, free everything. Safe in
 * any state. */
void tls_close(void);

/* Last mbedTLS or lwIP error and where it came from, for the log. */
const char *tls_error(int *code);

#endif
