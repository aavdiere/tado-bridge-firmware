#ifndef NETWORK_H
#define NETWORK_H

#include "ccm.h"

#include <stdbool.h>
#include <stdint.h>

/* The bridge's network (#37): the network record and the counter log in
 * external flash (src/store.h) put into the link layer (src/link.h).
 * Radio task only (it owns SPI1); after link_init().
 *
 * Without a valid network record the link keeps no key: nothing is
 * decrypted, acknowledged or sent, and nothing is written to the flash. A
 * network is only created by an explicit network_create(), which the
 * pairing trigger (src/radio.c, #19) calls before it opens a window, and
 * only where no network record has ever been (network_absent()). */

/* Boot, before the radio starts, and only if the flash is ready
 * (flash_boot() in src/radio.c): load the network record; if there is
 * one, reserve frame counters (one counter-log write) and set the key. */
void network_boot(void);

/* A fresh network on `channel` (only the test channel, the build's
 * RADIO_NET_CHANNEL, default 16; never 26 except in a channel-26 build
 * with the opt-in RADIO_ALLOW_CH26, src/txgate.h) with a
 * new key from the CTR_DRBG, written to the network record and read back
 * as a boot would read it, then set as in network_boot(). Replaces any
 * earlier network: devices paired to it must be paired again. True only
 * if the record read back is the one just written (its seq and contents);
 * then key_out (if not NULL) receives the key read back, which is the one
 * now in use, for pairing to send to a device under the pairing key
 * (docs/radio.md "Link security and pairing"). The key is never logged.
 * False if refused or if any step failed; once the write has started, a
 * failure also leaves no key set until the next boot, which loads the
 * newest valid record. May wait for a DRBG reseed (about 45 ms) and for a
 * sector erase (up to 240 ms).
 * Refused unless network_absent() or network_replaceable(); after any
 * failure, refused until the next boot. Holds the radio task's loop for
 * up to about 0.8 s (DRBG
 * reseed, sector erases); frames are acknowledged during the sector erases
 * (#22, src/mx25r.h) but not during the reseed, and the coordinator waits:
 * never while a pairing window is open, and only with the coordinator idle (csl_idle()). */
bool network_create(uint8_t channel, uint8_t key_out[CCM_KEY_LEN]);

/* Radio task, with no frame arriving and the coordinator idle
 * (csl_idle(), #22): moves the counter reservation up once less than
 * STORE_CTR_LOW counters remain (one counter-log append; a sector erase
 * every STORE_CTR_ENTRIES appends; src/store.h). */
void network_service(uint32_t now_ms);
/* network_service() would write now: a network is active, less than
 * STORE_CTR_LOW counters remain reserved, and no retry wait after a failed write runs.
 * No flash access. */
bool network_service_due(uint32_t now_ms);

/* A network is loaded and its key set. */
bool network_active(void);

/* At boot the flash was read and holds no network record at all, not even
 * a damaged or refused one (store_net_probe()), and nothing has been
 * written since: one of the two states in which network_create() runs.
 * False if network_boot() did not run (no identity, flash not ready). */
bool network_absent(void);

/* The other state (stage P7.1): at boot the newest network record was a
 * sound one (store_net_other_channel(): every slot empty or valid, tado
 * PAN) for another channel, written by an image built for another
 * RADIO_NET_CHANNEL, and nothing has been written since. This image never
 * uses it (no key, nothing sent, receiving goes on) and never retunes to
 * it; network_create(), asked for by pairing (the button or the server's
 * request, pairing_open() in src/radio.c), may replace it with a new
 * network on this image's channel. *channel (if not NULL): the record's
 * channel. A damaged or unreadable record still blocks network_create(). */
bool network_replaceable(uint8_t *channel);

/* Factory reset (P6): the link drops its key and any pairing window at
 * once (nothing is acknowledged or sent from here on), no network may be
 * created in this boot, then store_factory_reset() erases the network
 * slots and the counter log; the identity stays. Radio task, at a quiet
 * moment; the caller restarts the MCU afterwards, success or not, so that
 * the next boot reads what the flash now holds. Frames are still received
 * during the erases (#22). */
bool network_factory_reset(void);

/* The key of the active network, read back from its record in flash (the
 * record the boot or network_create() activated, by seq), for a pairing
 * window to hand out. False if no network is active or the record no
 * longer reads back. The caller wipes key_out after use. Radio task. */
bool network_key(uint8_t key_out[CCM_KEY_LEN]);

#endif
