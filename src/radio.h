#ifndef RADIO_H
#define RADIO_H

#include "txgate.h"

#include <stdbool.h>

struct fwupd_op;

/* Channel plan: f = 863.125 MHz + n x 199.951 kHz, n = 0..49 (docs/radio.md
 * "PHY"). */

/* The bridge's channel: the test network's channel 16 (866.3242 MHz), owner
 * decision of 2026-11-17 (channel 10 before; docs/README.md "Project
 * decisions", src/txgate.h). The radio receives and, unless the image is
 * built receive-only (RADIO_TX_ALLOWED=0, src/cc110l.h), transmits there.
 * The network record (#37, docs/design.md "Persistent storage") holds this
 * channel only: src/network.c asserts it, and a record for another channel
 * is not used. */
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

/* The net task posted the server's pair request (#40,
 * pair_remote_post(), src/pair_remote.h): wake the radio task, which acts
 * on it at its next quiet moment (pairing_remote(), src/radio.c) and
 * leaves the outcome for pair_remote_result(). Any task. */
void radio_request_remote_pairing(void);

/* The build names a pairing target (PAIR_TARGET_EUI64 in
 * config/local.cmake): without one no window ever opens. Any task. */
bool radio_pair_target_named(void);

/* The factory reset button was held (src/app.c, P6): at its next quiet
 * moment the radio task erases the network record and the counter log
 * (network_factory_reset()), keeps the identity record, and restarts the
 * MCU, which then boots without a network. Any task. */
void radio_request_factory_reset(void);

/* The update task (#24, src/fwupd_task.c): run one flash operation of the
 * update (src/fwupd.h) in the radio task, which owns SPI1, at its next
 * moment for flash work (no frame arriving, the coordinator idle, and held
 * while the operation waits), and wait for it. Returns op->ok; op->network
 * tells whether a network is active. If the radio task has not taken the
 * operation within RADIO_UPDATE_OP_WAIT_MS (its loop does not run: the
 * radio did not come up), it is withdrawn and never runs, and this returns
 * false with op->done false (src/handoff.h). FWUPD_OP_RESTART restarts the
 * MCU and does not return when taken; when it returns, the restart is the
 * caller's. One caller at a time. */
bool radio_update_op(struct fwupd_op *op);
/* The limit, with why it is 10 s: src/radio.c. */
#define RADIO_UPDATE_OP_WAIT_MS 10000u

/* The radio is up: configured, its interrupts on, the PLL calibrated (for
 * the update's confirmation, #24). Any task; a snapshot. */
bool radio_up(void);

#endif
