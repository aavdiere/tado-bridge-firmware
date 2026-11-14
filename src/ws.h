#ifndef WS_H
#define WS_H

#include "lwip/ip_addr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WebSocket client (RFC 6455) over src/tls.h (#15): HTTP/1.1 upgrade with
 * Sec-WebSocket-Protocol "binary" (docs/server-api.md "Transport"), masked
 * client frames, ping and pong, close handshake, fragmented messages
 * reassembled. One connection; net task only. */

#define WS_RX_MAX 2048 /* largest message accepted from the server */
#define WS_TX_MAX 1280 /* frames queued for sending, header and mask included */

enum ws_state {
    WS_IDLE,
    WS_CONNECTING, /* TCP and TLS */
    WS_UPGRADING,  /* HTTP request sent, waiting for 101 */
    WS_OPEN,
    WS_CLOSING, /* close frame exchange, then TLS close */
    WS_CLOSED,  /* finished or failed; ws_reason() says why. ws_open() again to reconnect */
};

/* Called from ws_poll() for every complete binary message. */
typedef void (*ws_message_fn)(const uint8_t *data, size_t len);

/* Start a connection to wss://host:port/path. host is also the name the
 * server certificate must carry and the HTTP Host header. */
bool ws_open(const ip_addr_t *ip,
             uint16_t         port,
             const char      *host,
             const char      *path,
             ws_message_fn    on_message);

/* Advance; returns the state. *busy: call again soon (handshake or data). */
enum ws_state ws_poll(bool *busy);

/* Queue one binary message. False if not open or the send queue is full. */
bool ws_send(const uint8_t *data, size_t len);

/* Start the close handshake with this status code (1000 normal), or close
 * at once if not open. ws_poll() then reaches WS_CLOSED. */
void ws_close(uint16_t code);

/* Why the connection ended, for the log. */
const char *ws_reason(void);

#endif
