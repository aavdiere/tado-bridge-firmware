/* CoAP relay between the radio link and the server's WebSocket frame (#17).
 * Hardware independent; tested on the host.
 *
 * Uplink: a decrypted Data frame from a device becomes, after duplicate
 * suppression and 6LoWPAN reassembly, a 28-byte frame header plus the CoAP
 * message, for the net task to send (docs/server-api.md "Frame").
 * Downlink: a frame from the server for a device becomes a link-local
 * 6LoWPAN datagram, fragmented if needed, queued on the CSL coordinator.
 * The CoAP message is carried as bytes in both directions and never
 * changed, so message IDs and tokens survive (docs/server-api.md "Bridge
 * only relays"). */
#ifndef RELAY_H
#define RELAY_H

#include "lowpan.h"
#include "mac.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- The 28-byte frame (tanoclo/ws-server/lib/ws-bridge.js:11-15, 62-130) --- */
#define RELAY_HDR_LEN  28u
#define RELAY_DIR_UP   0x0001u
#define RELAY_DIR_DOWN 0x0002u
/* Largest CoAP message a fragmented datagram carries: 1280 - 48. */
#define RELAY_COAP_MAX (LOWPAN_IPV6_MAX - LOWPAN_UNCOMP_HDR)
#define RELAY_MSG_MAX  (RELAY_HDR_LEN + RELAY_COAP_MAX)

#define RELAY_PORT_COAP 5683u
#define RELAY_PORT_BRIDGE \
    4005u /* source port of the bridge's requests (freedo build_coap.go:38-40) */

struct relay_frame {
    uint16_t       direction;
    uint8_t        ipv6[16];
    uint16_t       port;
    const uint8_t *coap; /* inside the message */
    size_t         coap_len;
};

/* False unless the header is well formed and the length field matches. */
bool relay_frame_parse(const uint8_t *msg, size_t len, struct relay_frame *f);

/* Writes the uplink header for a CoAP message of coap_len bytes: direction
 * 1, the address, field_a/b/c 0 (tanoclo accepts zeros; their stock meaning
 * is open, docs/open-questions.md row 10), the port and the length. */
void relay_frame_header(uint8_t       hdr[RELAY_HDR_LEN],
                        const uint8_t ipv6[16],
                        uint16_t      port,
                        size_t        coap_len);

/* fe80:: plus the interface identifier of a wire EUI-64, and back. The
 * second is false unless the address is link-local with a tado OUI. */
void relay_ipv6_from_wire(const uint8_t wire[MAC_EUI_LEN], uint8_t ipv6[16]);
bool relay_wire_from_ipv6(const uint8_t ipv6[16], uint8_t wire[MAC_EUI_LEN]);

/* --- Per-device state ------------------------------------------------------- */
/* Downlink datagram ids run 0..RELAY_ID_LAST; the ids above are the link
 * layer's own (Router Advertisements, d/pair, #19). */
#define RELAY_ID_LAST 0xFFEFu

#define RELAY_PEERS         16u    /* owner decision: 16 devices (docs/design.md) */
#define RELAY_DUP_WINDOW_MS 10000u /* freedo tado/mac_duplicate.go:13 */

struct relay_peer {
    bool     used;
    uint8_t  eui[MAC_EUI_LEN];
    uint32_t seen_ms; /* last uplink, for eviction */
    /* Last Data frame taken, to recognise a MAC retransmission. */
    bool     have_last;
    uint8_t  last_seq;
    uint8_t  last_len;
    uint32_t last_ms;
    uint8_t  last[MAC_DATA_MAX_DGRAM];
    /* UDP ports of the last datagram from the device, for our answers. */
    bool     have_ports;
    uint16_t sport, dport;
};

struct relay {
    uint8_t             own[MAC_EUI_LEN];
    struct relay_peer   peer[RELAY_PEERS];
    struct lowpan_reasm reasm;
    uint16_t            frag_tag; /* next datagram tag for our fragments */
    uint16_t            next_id;  /* next downlink datagram id */
};

void relay_init(struct relay *r, const uint8_t own[MAC_EUI_LEN]);

/* --- Uplink ----------------------------------------------------------------- */
enum relay_up {
    RELAY_UP_FORWARD,   /* up is filled in */
    RELAY_UP_HELD,      /* a fragment; the datagram is not complete yet */
    RELAY_UP_DUPLICATE, /* MAC retransmission of the frame taken last */
    RELAY_UP_NOT_OURS,  /* IPv6 destination is another node (zone plane): not relayed */
    RELAY_UP_NOT_UDP,   /* ICMPv6 or anything else that is not UDP */
    RELAY_UP_BAD,       /* refused fragment, or an address form the link does not use */
};

struct relay_uplink {
    uint8_t        ipv6[16]; /* the device */
    uint16_t       port;     /* its UDP source port */
    const uint8_t *coap; /* in d's buffer or in the reassembly buffer: use before the next call */
    size_t         coap_len;
};

/* One decrypted unicast Data frame addressed to us. */
enum relay_up
relay_uplink(struct relay *r, const struct mac_data *d, uint32_t now_ms, struct relay_uplink *up);

/* --- Downlink --------------------------------------------------------------- */
enum relay_down {
    RELAY_DOWN_QUEUED,
    RELAY_DOWN_BAD_FRAME,   /* not a downlink frame, or no CoAP in it */
    RELAY_DOWN_BAD_ADDRESS, /* not a link-local device address, or our own */
    RELAY_DOWN_TOO_LARGE,
    RELAY_DOWN_QUEUE_FULL, /* not enough free jobs for all fragments; nothing queued */
    RELAY_DOWN_NOT_PEER,   /* channel-26 build only: not the named spare device (txgate.h rule 6) */
};

/* One frame from the server. Queues the Data frames on the CSL coordinator
 * (csl_enqueue_data) under the network key. */
enum relay_down relay_downlink(struct relay *r, const uint8_t *msg, size_t len);

#endif
