#ifndef IDENTITY_H
#define IDENTITY_H

#include "store.h"

#include <stdbool.h>

/* The bridge's identity (#21): serial and EUI-64, and from a version 2
 * record also the Ethernet MAC (#39), from the identity record in external
 * flash (src/store.h, docs/design.md "Persistent storage"), read once at
 * boot by the radio task (it owns SPI1) and then fixed.
 *
 * Fail closed: without a valid record the bridge has no identity. The
 * radio link then has no own address, so nothing is acknowledged, answered
 * or sent and no network record is read (src/network.c); the session does
 * not authenticate to the server (src/session.c); the power LED shows
 * fault code 4 (docs/hardware.md "LED roles"). */

enum identity_state {
    IDENTITY_PENDING, /* the radio task has not read the record yet */
    IDENTITY_NONE,    /* no valid record (or the flash was not usable): no identity */
    IDENTITY_OK,
};

/* Radio task, once, after flash_boot(): reads the record if flash_ok, then
 * publishes the result to the other tasks. */
void identity_boot(bool flash_ok);

/* Any task. Once it is not IDENTITY_PENDING it never changes again. The
 * net task waits for it before it sets the Ethernet MAC (src/net.c). */
enum identity_state identity_state(void);

/* The identity, or NULL unless identity_state() is IDENTITY_OK. */
const struct store_id *identity(void);

#endif
