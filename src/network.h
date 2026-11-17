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
 * network is only created by an explicit network_create(), which pairing
 * (#19) will call; there is no caller yet. */

/* Boot, before the radio starts: load the network record; if there is
 * one, reserve frame counters (one counter-log write) and set the key. */
void network_boot(void);

/* A fresh network on `channel` (only the test channel 10; never 26) with a
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
 * sector erase (up to 240 ms). */
bool network_create(uint8_t channel, uint8_t key_out[CCM_KEY_LEN]);

/* Radio task, at a quiet moment (no frame arriving, no transaction on the
 * air): moves the counter reservation up once less than 2048 remains. */
void network_service(uint32_t now_ms);

/* A network is loaded and its key set. */
bool network_active(void);

#endif
