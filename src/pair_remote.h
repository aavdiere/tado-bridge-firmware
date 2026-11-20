#ifndef PAIR_REMOTE_H
#define PAIR_REMOTE_H

/* The server's remote pairing trigger (#40; owner decision 2026-11-17,
 * docs/open-questions.md row 28a): tanoclo's CON POST
 * d/I/IB0000FT0100<digits>/pair opens the pairing window the pair button
 * opens (#19, src/pair.h), or with duration 0 closes it. Hardware
 * independent: the request's checks, the CoAP code of each outcome, the
 * duplicate cache and the hand-over from the net task to the radio task
 * (tests/host/test_pair_remote.c). src/session.c answers the request,
 * src/radio.c opens or closes the window, with every guard of the button
 * (test channel only, the target named in the build only, airtime budget,
 * identity).
 *
 * The request, from tanoclo's pushDevicePair() (tanoclo/ws-server/lib/
 * commands/device.js:341-388), sent through internalPushViabridge() to the
 * bridge's own address (the IPv6 address of the bridge's `devices` row,
 * lib/command-api.js:102-157):
 *   CON POST, options 7 = ff ff and 12 = 2a (42), Uri-Path
 *   d / I / IB0000FT0100<serial without its leading "IB"> / pair,
 *   payload 01 04 00 00 <dur u16 BE> 02 01 00: one-byte tags and lengths,
 *   tag 0x01 the duration in seconds (u32 BE; 1..65535, default 300; 0 to
 *   close), tag 0x02 of unknown meaning (ignored).
 * tanoclo resends it at 6, 13, 26, 50 and 90 s until an ACK with its
 * Message ID comes back; it reads neither code nor payload
 * (lib/coap-transport.js:35, 195-265). It runs its own timer and sends
 * duration 0 after the duration it asked for (device.js:357-376). */

#include "coap.h"

#include <stdbool.h>
#include <stdint.h>

/* The path segment before the serial digits (device.js:383). */
#define PAIR_REMOTE_PREFIX  "IB0000FT0100"
#define PAIR_REMOTE_TAG_DUR 0x01u

/* Outcomes of one request, each with its CoAP code (pair_remote_code()). */
enum pair_remote_rc {
    PAIR_REMOTE_FORWARD,     /* checks passed: for the radio task (never answered as such) */
    PAIR_REMOTE_OPENED,      /* 2.04: a window opened */
    PAIR_REMOTE_WAS_OPEN,    /* 2.04: a window was open already; left as it is */
    PAIR_REMOTE_CLOSED,      /* 2.04: the open window closed */
    PAIR_REMOTE_WAS_CLOSED,  /* 2.04: no window was open */
    PAIR_REMOTE_NOT_OURS,    /* 4.04: the path names another serial */
    PAIR_REMOTE_METHOD,      /* 4.05: not a POST */
    PAIR_REMOTE_MALFORMED,   /* 4.00: no well-formed duration */
    PAIR_REMOTE_NO_TARGET,   /* 5.01: no PAIR_TARGET_EUI64 in the build */
    PAIR_REMOTE_RX_ONLY,     /* 5.01: receive-only image (RADIO_TX_ALLOWED 0) */
    PAIR_REMOTE_SELF_TARGET, /* 5.01: the build's target is the bridge's own EUI-64 */
    PAIR_REMOTE_NO_FLASH,    /* 5.03: flash not ready at boot */
    PAIR_REMOTE_NO_IDENTITY, /* 5.03 */
    PAIR_REMOTE_RADIO,       /* 5.03: radio not ready to transmit */
    PAIR_REMOTE_QUEUED,      /* 5.03: frames of the last window still queued */
    PAIR_REMOTE_NETWORK,     /* 5.03: a network record exists but is not in use */
    PAIR_REMOTE_OPEN_FAILED, /* 5.03: key, random numbers or target refused */
    PAIR_REMOTE_BUSY,        /* 5.03: another pair request is in work */
    PAIR_REMOTE_NO_RADIO,    /* 5.03: the radio task did not take it in time */
    PAIR_REMOTE_RC_COUNT,
};

/* Uri-Path is exactly four segments d / I / <any> / pair. */
bool pair_remote_path(const struct coap_msg *m);

/* What a request on that path asks, in this order: a path for another
 * serial (the segment must be PAIR_REMOTE_PREFIX followed by serial without
 * a leading "IB", as tanoclo builds it; an empty serial matches nothing)
 * is NOT_OURS; a method other than POST is METHOD; a payload that is not a
 * list of one-byte tag, one-byte length, value, with exactly one tag 0x01
 * of four bytes, is MALFORMED (other tags are skipped); a duration above 0
 * with no target in the build is NO_TARGET, in a receive-only image
 * RX_ONLY. Otherwise FORWARD, *open = duration above 0, *dur_s = the
 * duration. *dur_s is set whenever the duration was read (also on
 * NO_TARGET and RX_ONLY), else 0. */
enum pair_remote_rc pair_remote_check(const struct coap_msg *m,
                                      const char            *serial,
                                      bool                   have_target,
                                      bool                   tx_allowed,
                                      bool                  *open,
                                      uint32_t              *dur_s);

/* The window length for a duration above 0: dur_s seconds, at most
 * PAIR_WINDOW_MS (src/pair.h). */
uint32_t pair_remote_window_ms(uint32_t dur_s);

uint8_t pair_remote_code(enum pair_remote_rc rc);
/* A short text for the log ("no identity", ...); the button's log lines
 * use the same texts (src/radio.c). */
const char *pair_remote_text(enum pair_remote_rc rc);

/* --- Duplicates (RFC 7252 4.5) --------------------------------------------- */
/* The requests the radio task acted on, by Message ID, for
 * PAIR_REMOTE_LIFETIME_MS (EXCHANGE_LIFETIME, RFC 7252 4.8.2; tanoclo's
 * resends end 185 s after the first send). A resent copy gets the answer
 * of the first, and nothing is done again: so a late copy of an "open"
 * never reopens a window a later "close" closed. Kept across sessions:
 * tanoclo resends by bridge, also over a new connection. Refusals the net
 * task decides alone (NOT_OURS to RX_ONLY, BUSY) are not kept: a copy is
 * checked again and gets the same answer, or a better one. */
#define PAIR_REMOTE_SEEN        8u
#define PAIR_REMOTE_LIFETIME_MS 247000u

struct pair_remote_seen {
    struct {
        bool     used;
        uint16_t mid;
        uint8_t  rc;
        uint32_t secs, at_ms;
    } e[PAIR_REMOTE_SEEN];
    unsigned next;
};

/* A request with this Message ID was acted on less than
 * PAIR_REMOTE_LIFETIME_MS ago: its outcome. */
bool pair_remote_seen_find(const struct pair_remote_seen *s,
                           uint16_t                       mid,
                           uint32_t                       now_ms,
                           enum pair_remote_rc           *rc,
                           uint32_t                      *secs);
/* Remember one; replaces the oldest when full. */
void pair_remote_seen_add(struct pair_remote_seen *s,
                          uint16_t                 mid,
                          enum pair_remote_rc      rc,
                          uint32_t                 secs,
                          uint32_t                 now_ms);

/* --- Hand-over, net task (poster) to radio task (runner) -------------------- */
/* One request at a time, four states:
 *   empty -> posted      pair_remote_post(), poster
 *   posted -> claimed    pair_remote_claim(), runner: it acts on it
 *   posted -> empty      pair_remote_withdraw(), poster: it is never acted on
 *   claimed -> done      pair_remote_finish(), runner
 *   done -> empty        pair_remote_result(), poster
 * The two changes out of "posted" are compare-and-swap, so exactly one
 * wins (as src/handoff.c). No lock and no critical section: the radio
 * task's ACK interrupt is never masked. The poster never waits. */

/* Poster. False if the slot is not empty. */
bool pair_remote_post(bool open, uint32_t window_ms);
/* Poster. True if the request was still unclaimed and is now withdrawn. */
bool pair_remote_withdraw(void);
/* Poster. The runner's outcome, once; the slot is then empty. */
bool pair_remote_result(enum pair_remote_rc *rc, uint32_t *secs);

/* Runner. A request is posted and not yet claimed. */
bool pair_remote_pending(void);
/* Runner. Take the posted request; false if there is none (or it was
 * withdrawn a moment ago). */
bool pair_remote_claim(bool *open, uint32_t *window_ms);
/* Runner. The claimed request's outcome; secs: the window's length
 * (OPENED) or the time it has left (WAS_OPEN), else 0. */
void pair_remote_finish(enum pair_remote_rc rc, uint32_t secs);

#endif
