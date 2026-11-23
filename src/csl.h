/* CSL coordinator (#17): wake-up trains, anchors, the per-device downlink
 * queue, listen before transmit and MAC retries. Ported from freedo's
 * bridge firmware (Apache-2.0, see csl.c). Hardware independent: the radio
 * and the clock are reached through the csl_port_* functions, which
 * src/radio_csl.c implements for the CC110L and the host tests implement as a
 * simulation. Radio task only; no allocation.
 *
 * One transaction at a time: wait for the device's next sample instant,
 * listen, send wake-up frames that count the time down, send the Data frame
 * at the rendezvous, and wait for the device's Enhanced ACK, which also
 * refreshes the anchor (docs/radio-runtime.md "Sending to a sleeping
 * device"). */
#ifndef CSL_H
#define CSL_H

#include "mac.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Timing, from freedo cc1101_bridge.ino:176-199 unless noted. */
#define CSL_PERIOD_US       1000000u /* devices sample once a second */
#define CSL_UNIT_US         320u     /* rendezvous and phase unit */
#define CSL_SAMPLE_GUARD_US 40000u   /* train starts this long before the sample... */
#define CSL_GUARD_PPM       30u      /* ...plus this much of the anchor's age */
#define CSL_GUARD_MAX_US    (CSL_PERIOD_US / 2u)
#define CSL_BOOTSTRAP_US    1000000u /* train without an anchor: a whole period */
#define CSL_WAKE_CAP        300u     /* runaway limit; a 1 s train is about 255 frames */
#define CSL_ANCHOR_MISSES   4u       /* missed ACKs in a row that drop an anchor */
#define CSL_ACK_WINDOW_US   50000u   /* freedo internal/radio/live.go:167 */
#define CSL_TXN_MARGIN_US   400000u

/* Listen before transmit, as the stock coordinator does it: a random
 * backoff of 0-6 x 640 us, then a 3.968 ms listen that ends where the train
 * starts (freedo docs/PROTOCOL.md:1594-1597; owner decision 2026-11-11,
 * docs/radio-runtime.md "Listen before transmit"). */
#define CSL_LBT_SLOT_US   640u
#define CSL_LBT_SLOTS     7u /* 0..6 */
#define CSL_LBT_LISTEN_US 3968u
#define CSL_LBT_MAX_US    ((CSL_LBT_SLOTS - 1u) * CSL_LBT_SLOT_US + CSL_LBT_LISTEN_US)
#define CSL_LBT_RETRY_US  20000u /* next try after a busy channel */
#define CSL_LBT_BUSY_MAX  10u    /* busy this often in a row counts as a failed attempt */

/* MAC retries: same seq, next frame counter (docs/radio.md "Retry gap",
 * provisional: 2-3 s, three attempts). */
#define CSL_ATTEMPTS     3u
#define CSL_RETRY_GAP_US 2000000u

#define CSL_ANCHORS   16u                /* owner decision: 16 devices (docs/design.md) */
#define CSL_NEIGHBORS (CSL_ANCHORS + 1u) /* and one queue for broadcasts */
#define CSL_JOB_POOL  24u                /* docs/design.md "RAM estimate" */
#define CSL_BODY_MAX  MAC_BCAST_MAX_PKT  /* 103 >= MAC_DATA_MAX_DGRAM (97) */

enum csl_key { CSL_KEY_NETWORK, CSL_KEY_PAIRING, CSL_KEY_COUNT };

struct csl_config {
    uint8_t  own[MAC_EUI_LEN];
    uint16_t pan; /* the network's PAN, in every wake-up frame (#43) */
    /* Keys by enum csl_key; a NULL or unset key fails the jobs that need
     * it. The pairing key is for #19. */
    const struct ccm_key *key[CSL_KEY_COUNT];
    uint32_t              counter;       /* first frame counter to use */
    uint32_t              counter_limit; /* first counter not reserved (#37) */
    uint8_t               seq;           /* first MAC sequence number */
};

void csl_init(const struct csl_config *cfg);

/* The next frame counter that would be used (for the counter log, #37). */
uint32_t csl_counter(void);

/* Counter reservation (#37, docs/design.md "Counter log"): no transaction
 * starts while the next counter is at or above the limit, so no counter at
 * or above the value persisted in flash ever goes on the air. Jobs wait in
 * their queues until the limit moves up. The limit only moves up. */
void     csl_set_counter_limit(uint32_t limit);
uint32_t csl_counter_limit(void);

/* --- Queue ---------------------------------------------------------------- */
#define CSL_JOB_MORE    0x01u /* more fragments of the same datagram follow */
#define CSL_JOB_PAIRING 0x02u /* part of pairing: passed to the transmit gate (#19) */
/* A broadcast with Frame Pending set (59 e8): the first copy of a Router
 * Advertisement in a pairing window, which another copy under the pairing
 * key follows (freedo docs/PROTOCOL.md:668-672, #19). */
#define CSL_JOB_PENDING 0x04u

/* Free job slots; a fragmented datagram is queued only if all of its
 * fragments fit. */
unsigned csl_free_jobs(void);

/* Queue one unicast Data frame for dst: body is the datagram or one
 * fragment body, at most MAC_DATA_MAX_DGRAM bytes. id names the datagram;
 * when a job fails for good, the queued jobs with the same id and dst go
 * with it. False if the pool or the neighbour table is full. */
bool csl_enqueue_data(const uint8_t  dst[MAC_EUI_LEN],
                      const uint8_t *body,
                      size_t         len,
                      uint16_t       id,
                      unsigned       flags,
                      enum csl_key   key);

/* Queue one broadcast (a Router Advertisement): a full-period train to
 * ff ff, then the frame, no ACK. pkt at most MAC_BCAST_MAX_PKT bytes. */
bool csl_enqueue_broadcast(
    const uint8_t *pkt, size_t len, uint16_t id, unsigned flags, enum csl_key key);

/* Broadcasts queued (including one in flight), and whether n more fit the
 * pool now (#19: the two copies of a pairing-window Router Advertisement
 * are queued together or not at all). */
unsigned csl_broadcasts_queued(void);
bool     csl_room_for_broadcasts(unsigned n);

/* Jobs flagged CSL_JOB_PAIRING still queued, including one in flight. */
unsigned csl_pairing_jobs(void);

/* Drop everything queued and the transaction in flight (radio re-init). */
void csl_flush(void);

/* The next MAC sequence number and frame counter, for a frame sent outside
 * the coordinator: the Enhanced Beacon that answers a Beacon Request takes
 * the bridge's next ones (freedo docs/PROTOCOL.md:651-656, #19). False, and
 * nothing taken, if the counter is at the reservation limit. A transaction
 * in flight that ends without sending keeps its counter used, so no counter
 * goes on the air twice. */
bool csl_take_seq_counter(uint8_t *seq, uint32_t *counter);

/* --- State machine -------------------------------------------------------- */
/* Run the state machine once. Returns the time in microseconds until it
 * needs to run again (0: at once; UINT32_MAX: nothing to do until a frame
 * arrives or a job is queued). */
uint32_t csl_step(void);

/* A received frame (without the radio's CRC bytes), its radio CRC result
 * and the time its last bit arrived. Returns true if it was the Enhanced
 * ACK the transaction in flight was waiting for. */
bool csl_on_frame(const uint8_t *frame, size_t len, bool crc_ok, uint64_t end_us);

/* False while we wait for a device's Enhanced ACK or send a train: an ACK
 * of our own then would cover it (freedo README.md:110-117). */
bool csl_may_ack(void);

/* True while a transaction has reserved the air: from the start of its
 * listen to its end. Short work of the radio task (register checks, a
 * flash read) has to wait (docs/design.md "Sharing SPI1 with the radio");
 * flash programs and erases wait for csl_idle() instead. Same as
 * !csl_quiet_for(10000). */
bool csl_busy(void);

/* No transaction holds the air now, and the one waiting (if any) starts
 * its listen at least us microseconds from now. A frame sent outside the
 * coordinator that ends within us then cannot shorten that listen (#19:
 * the Enhanced Beacon). Idle with jobs queued counts as quiet: a new
 * transaction is set up only by csl_step(), and its listen starts after
 * that call. */
bool csl_quiet_for(uint32_t us);

/* No transaction set up: nothing scheduled that work outside the
 * coordinator could push late. Flash program and erase cycles start only
 * then (#22 bench finding, docs/design.md "Flash work and the
 * coordinator"): csl_step() does not run during flash work, so a job
 * waiting in its ST_WAIT, however far its listen, would start its train
 * late by the length of the work (a sector erase: up to 240 ms). Stays
 * true until the next csl_step(). Same as csl_quiet_for(UINT32_MAX). */
bool csl_idle(void);

/* Flash work is due (#22): while on, csl_step() starts no new transaction,
 * so the coordinator becomes idle once the one in flight has ended (as
 * usual, on its own timing; nothing is cut short) and the work cannot be
 * put off by a full queue. Queued jobs wait; nothing else changes (frames
 * are still taken and acknowledged, csl_take_seq_counter() still works).
 * The radio task sets it on every loop pass and clears it as soon as the
 * work is done; csl_init() clears it. */
void csl_hold(bool on);

/* --- Anchors -------------------------------------------------------------- */
/* An Enhanced ACK from dev, with its CSL phase, ended at end_us. */
void csl_anchor_store(const uint8_t dev[MAC_EUI_LEN], uint16_t phase, uint64_t end_us);
bool csl_anchor_known(const uint8_t dev[MAC_EUI_LEN]);

/* --- Port: implemented by src/radio_csl.c, and by tests/host --------------- */
/* Monotonic microseconds; must not wrap. */
uint64_t csl_port_now(void);

/* Send one frame and return when it has left the chip. flags: the
 * TXGATE_* flags for the transmit gate. False if the frame was refused or
 * the transmission failed. The receiver is on again afterwards. */
bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags);

/* Listen before transmit: begin marks the start of the listen; busy says
 * whether a packet was on the air at any time since. */
void csl_port_listen_begin(void);
bool csl_port_listen_busy(void);

/* Uniform random number for the backoff. */
uint32_t csl_port_random(void);

enum csl_outcome {
    CSL_DELIVERED, /* Enhanced ACK received (broadcast: sent) */
    CSL_NO_ACK,    /* no answer in the ACK window */
    CSL_BAD_ACK,   /* something that may have been a damaged ACK */
    CSL_TX_FAILED, /* the radio did not send, or the transaction overran */
    CSL_BUSY,      /* the channel stayed busy */
    CSL_NO_KEY,    /* key not set, or the frame could not be built */
    CSL_ABANDONED, /* an earlier fragment of the datagram failed */
};

struct csl_report {
    uint16_t         id;
    uint8_t          dst[MAC_EUI_LEN];
    enum csl_outcome outcome;
    bool             final;       /* false: the job stays queued for a retry */
    bool             anchored;    /* short train on a known anchor */
    uint8_t          attempt;     /* 1.. */
    uint16_t         wake_frames; /* sent in this attempt */
    uint8_t          seq;
};
/* Every attempt and every dropped job is reported, after the fact. */
void csl_port_report(const struct csl_report *r);

#endif
