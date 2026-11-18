#ifndef RADIO_H
#define RADIO_H

#include "txgate.h"

/* Channel plan: f = 863.125 MHz + n x 199.951 kHz, n = 0..49 (docs/radio.md
 * "PHY"). */

/* The bridge's channel: the test network's channel 16 (866.3242 MHz), owner
 * decision of 2026-11-17 (channel 10 before; docs/README.md "Project
 * decisions", src/txgate.h). The radio receives and, unless the image is
 * built receive-only (RADIO_TX_ALLOWED=0, src/cc110l.h), transmits there.
 * Until P5 the receive-only images listened on the stock network's channel
 * 26 instead. P6 takes the channel from the network record (docs/design.md
 * "Persistent storage"). */
#define RADIO_CHANNEL TXGATE_TEST_CHANNEL

/* Protected channel: the owner's stock network, channel 26 (868.3237 MHz,
 * docs/test-bench.md rule 2). The transmit gate (src/txgate.c) refuses it
 * and everything within 1 MHz of it. */
#define RADIO_PROTECTED_CHANNEL TXGATE_PROTECTED_CHANNEL

/* Radio task body: flash ID at boot, CC110L reset and configuration, then
 * the link to the devices (#17): receive, Enhanced ACKs, the CSL coordinator
 * and the relay queues of src/link.h. Owns SPI1 (docs/design.md "Tasks").
 * Never returns. */
void radio_task_run(void);

/* Ask the radio task to read the flash JEDEC ID on the shared bus (#10).
 * Any task; no effect before the radio task has started. */
void radio_request_flash_check(void);

/* Wake the radio task after link_downlink_put() (src/link.h). Any task. */
void radio_notify_link(void);

/* The pair button was held (src/app.c): open a pairing window for the
 * device named in the build, or close the open one (#19, src/pair.h). The
 * radio task does it at its next quiet moment and logs the outcome. Any
 * task. */
void radio_request_pairing(void);

/* The factory reset button was held (src/app.c, P6): at its next quiet
 * moment the radio task erases the network record and the counter log
 * (network_factory_reset()), keeps the identity record, and restarts the
 * MCU, which then boots without a network. Any task. */
void radio_request_factory_reset(void);

#endif
