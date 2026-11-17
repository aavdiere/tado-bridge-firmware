/* Pairing (#19): the window in which one device, named by its EUI-64,
 * receives the bridge's network key. Ported from freedo (Apache-2.0, see
 * pair.c). Hardware independent: no radio, no RTOS, no allocation; the
 * link layer (src/link.c) runs it in the radio task, and it is tested on
 * the host (tests/host/test_pair.c).
 *
 * The flow (docs/radio.md "Link security and pairing", freedo
 * docs/PROTOCOL.md:623-739):
 *   1. A joining device broadcasts Enhanced Beacon Requests under the
 *      pairing key, one every 8 s on each channel it scans.
 *   2. The bridge answers the target's request with an Enhanced Beacon,
 *      2.4-7.4 ms after the request ends, with no wake-up train
 *      (pair_check_request(), pair_build_beacon(); the timing is the radio
 *      task's, src/radio.c).
 *   3. While the window is open every Router Advertisement goes twice: under
 *      the network key with Frame Pending, then under the pairing key; a
 *      Router Solicitation of the target under the pairing key asks for
 *      such a pair (pair_take_ra_pair()).
 *   4. d/pair goes to the target under the pairing key: CON POST, empty
 *      token, Content-Format 42, an 8-byte single-use option 2048, and one
 *      TLV, tag 0x12, length 16, the network key (pair_dpair_due()).
 *   5. The device answers 2.04 under the network key (pair_reply()); the
 *      window closes.
 *
 * Rules (CLAUDE.md "Hard rules", docs/test-bench.md "The other tado
 * network"):
 *   - Only the device named when the window opened is answered or sent
 *     the key. A request or frame from any other address is ignored before
 *     anything is decrypted.
 *   - The window is short (PAIR_WINDOW_MS) and bounded in what it sends
 *     (PAIR_BEACONS_MAX, PAIR_RA_PAIRS_MAX, PAIR_DPAIR_MAX).
 *   - The network key is held here only while the window is open and is
 *     wiped when it closes. The channel (test channel 10 only) is the
 *     transmit gate's business (src/txgate.h rule 5); every frame of the
 *     window carries TXGATE_PAIRING. */
#ifndef PAIR_H
#define PAIR_H

#include "ccm.h"
#include "mac.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The valve's own commissioning window lasts two minutes (freedo
 * tado/pair_window.go:31-33); freedo caps a bridge window at 10 minutes. */
#define PAIR_WINDOW_MS 120000u

#define PAIR_REQUEST_LEN 27u /* freedo tado/beacon.go:16-30 */
#define PAIR_BEACON_LEN  32u

/* Per window. A joiner repeats its request every 8 s (freedo
 * docs/PROTOCOL.md:662-663): 15 in two minutes. A Router Advertisement
 * pair costs about 2 s of airtime (two 1 s trains), against the 30 s per
 * hour budget (src/airtime.h): three pairs and three d/pair datagrams of
 * up to three attempts each stay well inside it [I]. */
#define PAIR_BEACONS_MAX  16u
#define PAIR_RA_PAIRS_MAX 3u
#define PAIR_DPAIR_MAX    3u

/* d/pair goes this long after the target's first pairing-key frame that
 * is not a Beacon Request (its Router Solicitation, 1.5-2.8 s after the
 * beacon, freedo docs/PROTOCOL.md:664-665), so that the advertisement pair
 * it asked for goes first, as on the commercial bridge; or this long after
 * the first beacon if nothing else comes [I]. */
#define PAIR_DPAIR_DELAY_MS    3000u
#define PAIR_DPAIR_FALLBACK_MS 12000u

#define PAIR_SESSION_LEN \
    8u /* option 2048, new for each request (freedo tado/build_resources.go:457-459) */
/* Random bytes pair_open() takes: the first MID (2), then one session
 * value per d/pair datagram. */
#define PAIR_RAND_LEN (2u + PAIR_SESSION_LEN * PAIR_DPAIR_MAX)
/* The d/pair CoAP message with tag 0x12 (freedo tado/build_resources_test.go:196-203). */
#define PAIR_COAP_LEN 43u

/* Beacon answer timing, measured from the end of the request (the falling
 * GDO0 edge). The commercial beacon starts 2.4-7.4 ms after the request
 * ends (freedo docs/PROTOCOL.md:657-658), after a 3.968 ms listen (freedo
 * firmware/cc1101_bridge/cc1101_bridge.ino:228-230). The radio task calls
 * the driver at PAIR_BEACON_TX_AT_US; the driver needs about 0.15 ms to the
 * STX strobe and the chip 75 us from IDLE to TX (docs/datasheets/
 * cc110l.txt:2542), so the preamble starts about 3.9 ms after the request
 * ends [I]. A call later than PAIR_BEACON_TX_LATEST_US would start the
 * preamble after about 6.75 ms; it is not made, and the device asks again
 * 8 s later. */
#define PAIR_BEACON_TX_AT_US     3700u
#define PAIR_BEACON_TX_LATEST_US 6500u
#define PAIR_BEACON_START_MIN_US 2400u
#define PAIR_BEACON_START_MAX_US 7400u

/* "tado pairing key" (tanoclo/docs/rf_protocol.md:162-163, docs/pairing.md:79;
 * freedo tado/build_resources.go:20): public, shared by every tado device. */
extern const uint8_t pair_static_key[CCM_KEY_LEN];

/* Closed, nothing remembered. */
void pair_init(void);

/* Opens a window for the device whose wire EUI-64 is target, to hand over
 * net_key. rnd: PAIR_RAND_LEN random bytes. Replaces any earlier window.
 * False (and closed) if target is not a node address (tado suffix
 * c5 1b 00) or the key cannot be set. */
bool pair_open(const uint8_t target[MAC_EUI_LEN],
               const uint8_t net_key[CCM_KEY_LEN],
               const uint8_t rnd[PAIR_RAND_LEN],
               uint32_t      now_ms);

/* Closes the window: the pairing key schedule and the copy of the network
 * key are wiped. The target and the d/pair message IDs are remembered
 * until the next pair_open(), so a late answer is still recognised. */
void pair_close(void);

/* The window is open; closes it once PAIR_WINDOW_MS have passed. */
bool pair_active(uint32_t now_ms);

/* The pairing key schedule: set only while the window is open. */
const struct ccm_key *pair_key(void);

/* The window is open and wire is its target. */
bool pair_is_target(const uint8_t wire[MAC_EUI_LEN]);

/* --- Enhanced Beacon -------------------------------------------------------- */
enum pair_req {
    PAIR_REQ_OK,             /* answer it */
    PAIR_REQ_CLOSED,         /* no window */
    PAIR_REQ_MALFORMED,      /* not a 27-byte Beacon Request to ff ff / ff ff */
    PAIR_REQ_NOT_TARGET,     /* from another device: ignored, not decrypted */
    PAIR_REQ_LIMIT,          /* PAIR_BEACONS_MAX answered in this window */
    PAIR_REQ_MIC,            /* does not verify under the pairing key */
    PAIR_REQ_NOT_BEACON_REQ, /* verifies, but the MAC command is not 0x07 */
    PAIR_REQ_COUNT,
};

/* A received Enhanced Beacon Request (without the radio's CRC):
 * 0b e8 seq | ff ff | ff ff | src(8) | 04 | sealed [counter(4) 07] | MIC.
 * PAIR_REQ_OK counts it against PAIR_BEACONS_MAX. */
enum pair_req pair_check_request(const uint8_t *req, size_t len, uint32_t now_ms);

/* The Enhanced Beacon that answers dst from own under the pairing key:
 * 18 ec seq | cd ab | dst(8) | own(8) | 04 | counter(4 LE) | FCS | MIC
 * (freedo tado/beacon.go:70-98). Returns PAIR_BEACON_LEN, or 0 if the
 * window is closed. */
size_t pair_build_beacon(uint8_t       out[PAIR_BEACON_LEN],
                         uint8_t       seq,
                         const uint8_t dst[MAC_EUI_LEN],
                         const uint8_t own[MAC_EUI_LEN],
                         uint32_t      counter);

/* --- The target's other frames ---------------------------------------------- */
/* The target sent a frame under the pairing key that is not a Beacon
 * Request (its Router Solicitation, or a Data frame): it has taken our
 * beacon; d/pair becomes due PAIR_DPAIR_DELAY_MS later. */
void pair_note_joiner(uint32_t now_ms);

/* Counts one Router Advertisement pair: false once PAIR_RA_PAIRS_MAX went
 * in this window, or if it is closed. */
bool pair_take_ra_pair(uint32_t now_ms);

/* --- d/pair ------------------------------------------------------------------- */
/* The d/pair message: 40 02 mid | b1 'd' | 04 'pair' | 11 2a |
 * e8 06 e7 session(8) | ff 12 10 key(16) (freedo tado/build_resources.go:
 * 462-485). Returns PAIR_COAP_LEN, or 0 if cap is too small. */
size_t pair_build_coap(uint8_t      *out,
                       size_t        cap,
                       uint16_t      mid,
                       const uint8_t session[PAIR_SESSION_LEN],
                       const uint8_t key[CCM_KEY_LEN]);

/* When a d/pair datagram is due: builds it into coap (cap at least
 * PAIR_COAP_LEN) and returns its length; 0 otherwise. Only one is in
 * flight at a time; the caller reports how it ended with pair_dpair_done().
 * The caller wipes coap after queuing it: it holds the network key. */
size_t pair_dpair_due(uint8_t *coap, size_t cap, uint32_t now_ms);

/* The coordinator is done with the d/pair datagram: its Enhanced ACK came
 * (delivered) or every attempt failed. Another datagram may follow on the
 * target's next pairing-key frame, up to PAIR_DPAIR_MAX per window. */
void pair_dpair_done(bool delivered);

/* A CoAP message from src under the network key. */
enum pair_reply {
    PAIR_REPLY_NONE,   /* not an answer to our d/pair: relay it as usual */
    PAIR_REPLY_PAIRED, /* 2.04 from the target: paired; the window is closed */
    PAIR_REPLY_OTHER,  /* another answer to our d/pair (not relayed) */
};
enum pair_reply pair_reply(const uint8_t src[MAC_EUI_LEN], const uint8_t *coap, size_t len);

/* --- Counters ----------------------------------------------------------------- */
struct pair_stats {
    uint32_t windows, paired;
    uint32_t req[PAIR_REQ_COUNT];
    uint32_t joiner_frames, ra_pairs, dpair_sent, dpair_delivered, dpair_failed;
};
const struct pair_stats *pair_stats(void);

#endif
