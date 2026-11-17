/* Radio link layer (#17): what the bridge does with a received frame and
 * with a message from the server, above the radio driver and below the
 * session. Holds the network key, the CSL coordinator's configuration, the
 * relay state and the two static queues between the radio task and the net
 * task. Hardware independent (no radio, no RTOS calls); the radio task
 * calls it, and the net task uses only the four queue functions marked so.
 *
 * Network key and frame counter come from outside: link_set_key() is the
 * interface for the network and counter records (#37) and for pairing
 * (#19). Until it is called nothing is decrypted, acknowledged or sent. */
#ifndef LINK_H
#define LINK_H

#include "csl.h"
#include "mac.h"
#include "pair.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* own: our wire EUI-64, or NULL if the bridge has no identity. */
void link_init(const uint8_t own[MAC_EUI_LEN]);

/* Radio task. Sets the network key, the first frame counter to use and the
 * first counter not reserved in flash, and restarts the coordinator (queued
 * downlinks are dropped). The caller owns the counter reservation scheme
 * (docs/design.md "Counter log", src/network.c): link_counter() is the next
 * counter that would be used, and nothing is sent at or above the limit
 * until link_set_counter_limit() moves it up. */
void link_set_key(const uint8_t key[CCM_KEY_LEN], uint32_t first_counter, uint32_t counter_limit);
void link_set_counter_limit(uint32_t counter_limit);
void link_clear_key(void);
uint32_t link_counter(void);

/* Identity and key are there: frames can be taken, acknowledged and sent. */
bool           link_ready(void);
const uint8_t *link_own(void); /* NULL without identity */

/* Radio task: is there room to take one more uplink, whatever its size? An
 * uplink that cannot be stored must not be acknowledged, so that the device
 * sends it again (freedo cc1101_bridge.ino:1235-1248). */
bool link_uplink_room(void);

enum link_rx {
    LINK_RX_BAD_CRC,
    LINK_RX_ACK_TAKEN,     /* the Enhanced ACK our transaction waited for */
    LINK_RX_ACK_OTHER,     /* an Enhanced ACK that closes nothing of ours */
    LINK_RX_FORWARDED,     /* uplink queued for the server */
    LINK_RX_HELD,          /* fragment of an incomplete datagram */
    LINK_RX_DUPLICATE,     /* MAC retransmission */
    LINK_RX_NOT_RELAYED,   /* decrypted, but not for the server (zone plane, ICMPv6) */
    LINK_RX_DROPPED,       /* decrypted, queue full (the frame was not acknowledged) */
    LINK_RX_NOT_FOR_US,    /* unicast Data for another address */
    LINK_RX_NO_KEY,        /* Data or broadcast, and no key set */
    LINK_RX_MIC_FAIL,      /* wrong key or damaged */
    LINK_RX_BROADCAST,     /* a device's broadcast (Router Solicitation, ...) */
    LINK_RX_BEACON_REQ,    /* a joining device, not answered (no window, not its target, ...) */
    LINK_RX_BEACON_ANSWER, /* the window's target: an Enhanced Beacon is ready (link_beacon()) */
    LINK_RX_PAIRING,       /* a frame of the window's target under the pairing key */
    LINK_RX_PAIRED,        /* the target's 2.04 to d/pair: paired (not relayed) */
    LINK_RX_OTHER,         /* wake-up frame, beacon, unknown */
    LINK_RX_COUNT,
};

/* Radio task: one received frame without the radio's CRC bytes, the radio's
 * CRC result, the time its last bit arrived (csl_port_now() clock) and a
 * millisecond clock. */
enum link_rx
link_on_frame(const uint8_t *frame, size_t len, bool crc_ok, uint64_t end_us, uint32_t now_ms);

/* Radio task, every pass: takes the server's messages from the downlink
 * queue and schedules Router Advertisements. rnd: a random number. */
void link_poll(uint32_t now_ms, uint32_t rnd);

struct link_stats {
    uint32_t rx[LINK_RX_COUNT];
    uint32_t down_queued, down_refused, down_full;
    uint32_t ra_queued, rs_seen;
    uint32_t ra_pairs_queued, beacons_sent, beacons_dropped, dpair_queued;
};
const struct link_stats *link_stats(void);

/* Radio task: every report of the coordinator (csl_port_report()). */
void link_on_report(const struct csl_report *r);

/* --- Pairing (#19, src/pair.h) ------------------------------------------------- */
/* Radio task. Opens a pairing window for the device whose wire EUI-64 is
 * target, to hand it net_key, which must be the key in use (link_set_key()).
 * The window's first Router Advertisement pair is due a second later.
 * False if the link has no identity or key, target is ours or not a node
 * address, or frames of the last window are still queued
 * (link_pair_frames_queued()). rnd: PAIR_RAND_LEN random bytes. The caller
 * wipes its copies of net_key and rnd afterwards. */
bool link_pair_open(const uint8_t target[MAC_EUI_LEN],
                    const uint8_t net_key[CCM_KEY_LEN],
                    const uint8_t rnd[PAIR_RAND_LEN],
                    uint32_t      now_ms);
void link_pair_close(void);
bool link_pair_active(uint32_t now_ms);

/* Frames of a pairing window are still queued with the coordinator: an
 * advertisement copy or a d/pair of a window that has closed. A new window
 * waits until they are gone, so that it never sends one of them under its
 * own pairing key, nor takes their reports for its own. Once the window is
 * closed they go within seconds: those under the pairing key fail for
 * want of it at their next attempt. */
bool link_pair_frames_queued(void);

/* After LINK_RX_BEACON_ANSWER: the Enhanced Beacon to send and the 16
 * header bytes of the request it answers, for the transmit gate; NULL if
 * none is ready. The radio task sends it 2.4-7.4 ms after the request
 * ended (pair.h, PAIR_BEACON_TX_AT_US), or not at all, then calls
 * link_beacon_done(). */
const uint8_t *link_beacon(size_t *len, const uint8_t **answered);
void           link_beacon_done(bool sent);

/* --- Net task ---------------------------------------------------------------- */
/* A frame from the server for a device (the whole WebSocket message). False
 * if the queue is full; the server repeats its commands (docs/server-api.md
 * "Who does what"). Wake the radio task afterwards. */
bool link_downlink_put(const uint8_t *msg, size_t len);

/* The oldest uplink (28-byte header and CoAP), 0 if none; it stays queued
 * until link_uplink_pop(). */
size_t link_uplink_peek(uint8_t *out, size_t cap);
void   link_uplink_pop(void);

/* IPv4 gateway and DNS server of the bridge, network byte order, for the
 * Router Advertisement (freedo tado/broadcast.go:216-232). */
void link_set_router(uint32_t gateway, uint32_t dns);

#endif
