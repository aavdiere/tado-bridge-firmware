#ifndef RADIO_H
#define RADIO_H

/* Channel plan: f = 863.125 MHz + n x 199.951 kHz, n = 0..49 (docs/radio.md
 * "PHY"). */

/* Receive channel for P2: the owner's stock network, channel 26
 * (868.3237 MHz), found by the SDR scan (docs/test-bench.md "Channel scan
 * result"). P2 only listens there, which docs/test-bench.md rule 3 allows, so
 * that frames from real devices arrive. The test network (P5 onward) uses
 * channel 10 (865.1245 MHz). */
#define RADIO_RX_CHANNEL 26u

/* Protected channel: the owner's stock network (docs/test-bench.md rule 2).
 * Never transmit on it. Unused in P2, which cannot transmit at all
 * (RADIO_TX_ALLOWED in cc110l.h); the P5 transmit guard must refuse it. */
#define RADIO_PROTECTED_CHANNEL 26u

/* Radio task body (#9): flash ID at boot, CC110L reset and configuration,
 * then receive-only on RADIO_RX_CHANNEL, logging each frame and per-minute
 * counters. Owns SPI1 (docs/design.md "Tasks"). Never returns. */
void radio_task_run(void);

/* Ask the radio task to read the flash JEDEC ID on the shared bus (#10).
 * Any task; no effect before the radio task has started. */
void radio_request_flash_check(void);

#endif
