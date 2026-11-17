/* CSL coordinator (#17).
 *
 * Ported from freedo's bridge firmware, firmware/cc1101_bridge/
 * cc1101_bridge.ino: anchors (:1033-1122), job pool and per-neighbour queues
 * (:1124-1212), the state machine (:1257-1599) and the wake-up frame
 * (:1350-1363); timing constants :176-199.
 * Copyright 2026 The freedo contributors. Licensed under the Apache License,
 * Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0.
 *
 * Changes for this project:
 * - C, and the radio behind csl_port_* so the logic runs on the host.
 * - freedo's host builds the frames and retries; here the bridge is on its
 *   own, so a job holds the datagram, is sealed when its transaction
 *   starts (a retry keeps the MAC seq and takes the next frame counter,
 *   freedo docs/PROTOCOL.md:1317-1321), and is retried up to CSL_ATTEMPTS
 *   times.
 * - Every job may fall back to the full-period train (freedo's "bootstrap"
 *   flag is always on), so there is no "no anchor" failure.
 * - Listen before transmit before the first frame of every transaction
 *   (owner decision; freedo leaves it out).
 * - No follow window: the receiver keeps running and acknowledging after
 *   the transaction, so the device's reply is taken like any other uplink.
 * - Anchors are keyed by the full address, not the 16-bit short one.
 * - The wake-up frame cap is a runaway limit only (freedo: 253), so a train
 *   has no gap before its last frame however short the radio's turnaround
 *   is. */
#include "csl.h"

#include "txgate.h"

#include <string.h>

#define JOB_NONE   0xFFu
#define POOL_SLOTS (CSL_JOB_POOL + 1u)
_Static_assert(POOL_SLOTS < JOB_NONE, "job index fits");
_Static_assert(CSL_BODY_MAX >= MAC_DATA_MAX_DGRAM, "a job holds a full datagram");

enum { KIND_DATA, KIND_BROADCAST };

struct job {
    uint8_t  next;
    uint8_t  kind, flags, key;
    uint8_t  attempts, busy;
    uint8_t  seq;
    bool     seq_set;
    uint16_t id;
    uint8_t  len;
    uint8_t  body[CSL_BODY_MAX];
};

struct neighbor {
    bool     used;
    uint8_t  dst[MAC_EUI_LEN];
    uint8_t  head, tail, count;
    uint64_t not_before; /* retry gap, or listen-before-transmit retry */
};

struct anchor {
    bool     valid;
    uint8_t  dev[MAC_EUI_LEN];
    uint8_t  misses;
    uint64_t sample_us, refresh_us;
};

enum state { ST_IDLE, ST_WAIT, ST_LISTEN, ST_WAKE, ST_ACK };

static const uint8_t bcast_dst[MAC_EUI_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static struct {
    struct csl_config cfg;
    uint32_t          counter, counter_limit;
    uint8_t           seq;

    struct job      jobs[CSL_JOB_POOL + 1u]; /* one more, kept for a broadcast */
    uint8_t         free_head, free_count;
    struct neighbor nb[CSL_NEIGHBORS];
    uint8_t         cursor;
    struct anchor   anchors[CSL_ANCHORS];

    /* The transaction in flight. Its job stays at the head of its queue. */
    enum state state;
    uint8_t    nb_idx;
    bool       anchored, saw_invalid;
    uint16_t   sent;
    uint32_t   counter_before;
    uint64_t   listen_us, start_us, wake_end_us, window_end_us, deadline_us, data_sent_us;
    uint8_t    frame[MAC_FRAME_MAX];
    uint8_t    frame_len;
} g;

/* --- Anchors (freedo :1033-1122) ------------------------------------------- */
static struct anchor *anchor_find(const uint8_t dev[MAC_EUI_LEN]) {
    for (unsigned i = 0; i < CSL_ANCHORS; i++) {
        if (g.anchors[i].valid && memcmp(g.anchors[i].dev, dev, MAC_EUI_LEN) == 0) {
            return &g.anchors[i];
        }
    }
    return NULL;
}

bool csl_anchor_known(const uint8_t dev[MAC_EUI_LEN]) {
    return anchor_find(dev) != NULL;
}

void csl_anchor_store(const uint8_t dev[MAC_EUI_LEN], uint16_t phase, uint64_t end_us) {
    struct anchor *a = anchor_find(dev);
    if (!a) {
        for (unsigned i = 0; i < CSL_ANCHORS && !a; i++) {
            if (!g.anchors[i].valid) {
                a = &g.anchors[i];
            }
        }
        if (!a) { /* evict the least recently refreshed */
            a = &g.anchors[0];
            for (unsigned i = 1; i < CSL_ANCHORS; i++) {
                if (g.anchors[i].refresh_us < a->refresh_us) {
                    a = &g.anchors[i];
                }
            }
        }
    }
    memcpy(a->dev, dev, MAC_EUI_LEN);
    /* A stamp from the future or more than a period old is a missed edge. */
    uint64_t now = csl_port_now();
    if (end_us > now || now - end_us > CSL_PERIOD_US) {
        end_us = now;
    }
    /* The phase counts back to a sample the device woke on; later samples
     * are whole periods apart, so only the phase modulo one period counts.
     * A phase larger than the uptime moves the anchor one period forward
     * instead of underflowing. */
    uint64_t back = ((uint64_t)phase * CSL_UNIT_US) % CSL_PERIOD_US;
    a->sample_us  = end_us >= back ? end_us - back : end_us + CSL_PERIOD_US - back;
    a->refresh_us = end_us;
    a->misses     = 0;
    a->valid      = true;
}

static void anchor_miss(const uint8_t dev[MAC_EUI_LEN]) {
    struct anchor *a = anchor_find(dev);
    if (a && ++a->misses >= CSL_ANCHOR_MISSES) {
        a->valid = false;
    }
}

/* The anchor a job may use. One that has missed once is skipped in favour
 * of the full train: a device that rebooted samples at a new phase, and
 * its uplinks do not show it (freedo anchorForJob with the bootstrap flag). */
static struct anchor *anchor_for(const uint8_t dev[MAC_EUI_LEN]) {
    struct anchor *a = anchor_find(dev);
    return a && a->misses == 0 ? a : NULL;
}

/* Half the wake train around a sample: the base guard plus the clock error
 * that can have built up since the last refresh. */
static uint32_t anchor_guard(const struct anchor *a, uint64_t sample_us) {
    uint64_t age   = sample_us > a->refresh_us ? sample_us - a->refresh_us : 0;
    uint64_t guard = CSL_SAMPLE_GUARD_US + age * CSL_GUARD_PPM / 1000000u;
    return guard > CSL_GUARD_MAX_US ? CSL_GUARD_MAX_US : (uint32_t)guard;
}

/* The first sample that leaves its whole guard after `from`. A closer one
 * would get a train that starts late; the job waits one more period. */
static uint64_t anchor_next_sample(const struct anchor *a, uint64_t from, uint32_t *guard_us) {
    uint64_t sample   = a->sample_us;
    uint64_t earliest = from + CSL_SAMPLE_GUARD_US;
    if (sample < earliest) {
        sample += ((earliest - sample + CSL_PERIOD_US - 1u) / CSL_PERIOD_US) * CSL_PERIOD_US;
    }
    for (;;) { /* the guard is at most half a period: one more period at most */
        uint32_t guard = anchor_guard(a, sample);
        if (sample >= from + guard) {
            *guard_us = guard;
            return sample;
        }
        sample += CSL_PERIOD_US;
    }
}

/* --- Job pool and queues (freedo :1124-1212) -------------------------------- */
static void pool_init(void) {
    for (unsigned i = 0; i < POOL_SLOTS; i++) {
        g.jobs[i].next = i + 1u < POOL_SLOTS ? (uint8_t)(i + 1u) : JOB_NONE;
    }
    g.free_head  = 0;
    g.free_count = POOL_SLOTS;
    for (unsigned i = 0; i < CSL_NEIGHBORS; i++) {
        g.nb[i].used  = false;
        g.nb[i].head  = JOB_NONE;
        g.nb[i].tail  = JOB_NONE;
        g.nb[i].count = 0;
    }
    g.cursor = 0;
    g.state  = ST_IDLE;
}

void csl_init(const struct csl_config *cfg) {
    memset(&g, 0, sizeof g);
    g.cfg           = *cfg;
    g.counter       = cfg->counter;
    g.counter_limit = cfg->counter_limit;
    g.seq           = cfg->seq;
    pool_init();
}

void csl_flush(void) {
    pool_init();
}

uint32_t csl_counter(void) {
    return g.counter;
}

void csl_set_counter_limit(uint32_t limit) {
    if (limit > g.counter_limit) {
        g.counter_limit = limit;
    }
}

uint32_t csl_counter_limit(void) {
    return g.counter_limit;
}

/* One slot stays free for a broadcast while none is queued, so that Data
 * frames for a device that does not answer cannot keep a Router
 * Advertisement out. Broadcasts use queue 0 (nb_find). */
static unsigned reserved(void) {
    return g.nb[0].count == 0 ? 1u : 0u;
}

unsigned csl_free_jobs(void) {
    return g.free_count > reserved() ? g.free_count - reserved() : 0u;
}

/* The queue of dst. Slot 0 is kept for broadcasts, so that 16 devices
 * with queued frames cannot keep a Router Advertisement out. */
static struct neighbor *nb_find(const uint8_t dst[MAC_EUI_LEN], bool create) {
    bool             bcast = memcmp(dst, bcast_dst, MAC_EUI_LEN) == 0;
    unsigned         lo = bcast ? 0u : 1u, hi = bcast ? 1u : CSL_NEIGHBORS;
    struct neighbor *free_slot = NULL;
    for (unsigned i = lo; i < hi; i++) {
        struct neighbor *n = &g.nb[i];
        if (n->used && memcmp(n->dst, dst, MAC_EUI_LEN) == 0) {
            return n;
        }
        if (!n->used && !free_slot) {
            free_slot = n;
        }
    }
    if (!create || !free_slot) {
        return NULL;
    }
    free_slot->used = true;
    memcpy(free_slot->dst, dst, MAC_EUI_LEN);
    free_slot->head       = JOB_NONE;
    free_slot->tail       = JOB_NONE;
    free_slot->count      = 0;
    free_slot->not_before = 0;
    return free_slot;
}

static bool enqueue(const uint8_t  dst[MAC_EUI_LEN],
                    uint8_t        kind,
                    const uint8_t *body,
                    size_t         len,
                    size_t         max,
                    uint16_t       id,
                    unsigned       flags,
                    enum csl_key   key) {
    if (len == 0 || len > max || key >= CSL_KEY_COUNT || g.free_head == JOB_NONE
        || (kind == KIND_DATA && csl_free_jobs() == 0)) {
        return false;
    }
    struct neighbor *n = nb_find(dst, true);
    if (!n) {
        return false;
    }
    uint8_t     idx = g.free_head;
    struct job *j   = &g.jobs[idx];
    g.free_head     = j->next;
    g.free_count--;

    memset(j, 0, offsetof(struct job, body));
    j->next  = JOB_NONE;
    j->kind  = kind;
    j->flags = (uint8_t)flags;
    j->key   = (uint8_t)key;
    j->id    = id;
    j->len   = (uint8_t)len;
    memcpy(j->body, body, len);

    if (n->tail == JOB_NONE) {
        n->head = idx;
    } else {
        g.jobs[n->tail].next = idx;
    }
    n->tail = idx;
    n->count++;
    return true;
}

bool csl_enqueue_data(const uint8_t  dst[MAC_EUI_LEN],
                      const uint8_t *body,
                      size_t         len,
                      uint16_t       id,
                      unsigned       flags,
                      enum csl_key   key) {
    if (memcmp(dst, bcast_dst, MAC_EUI_LEN) == 0 || memcmp(dst, g.cfg.own, MAC_EUI_LEN) == 0) {
        return false;
    }
    return enqueue(dst, KIND_DATA, body, len, MAC_DATA_MAX_DGRAM, id, flags, key);
}

bool csl_enqueue_broadcast(
    const uint8_t *pkt, size_t len, uint16_t id, unsigned flags, enum csl_key key) {
    return enqueue(bcast_dst, KIND_BROADCAST, pkt, len, MAC_BCAST_MAX_PKT, id, flags, key);
}

/* Remove the head job of a queue. */
static void nb_pop(struct neighbor *n) {
    uint8_t idx = n->head;
    n->head     = g.jobs[idx].next;
    if (n->head == JOB_NONE) {
        n->tail = JOB_NONE;
    }
    n->count--;
    if (n->count == 0) {
        n->used = false;
    }
    g.jobs[idx].next = g.free_head;
    g.free_head      = idx;
    g.free_count++;
}

/* --- Transaction ----------------------------------------------------------- */
static void
report(const struct neighbor *n, const struct job *j, enum csl_outcome outcome, bool final) {
    struct csl_report r = {
        .id          = j->id,
        .outcome     = outcome,
        .final       = final,
        .anchored    = g.anchored,
        .attempt     = j->attempts,
        .wake_frames = g.sent,
        .seq         = j->seq,
    };
    memcpy(r.dst, n->dst, MAC_EUI_LEN);
    csl_port_report(&r);
}

/* Drop the head job for good, and with it the rest of its datagram. */
static void drop_datagram(struct neighbor *n, enum csl_outcome outcome) {
    uint16_t id = g.jobs[n->head].id;
    report(n, &g.jobs[n->head], outcome, true);
    nb_pop(n);
    g.sent = 0;
    while (n->used && n->head != JOB_NONE && g.jobs[n->head].id == id) {
        report(n, &g.jobs[n->head], CSL_ABANDONED, true);
        nb_pop(n);
    }
}

/* End the transaction in flight. */
static void finish(enum csl_outcome outcome) {
    struct neighbor *n   = &g.nb[g.nb_idx];
    struct job      *j   = &g.jobs[n->head];
    uint64_t         now = csl_port_now();
    g.state              = ST_IDLE;
    if (outcome == CSL_DELIVERED) {
        report(n, j, outcome, true);
        nb_pop(n);
        return;
    }
    if (outcome == CSL_BUSY && ++j->busy < CSL_LBT_BUSY_MAX) {
        /* Nothing went out: the counter is free again, and this was no attempt. */
        g.counter = g.counter_before;
        j->attempts--;
        report(n, j, outcome, false);
        n->not_before = now + CSL_LBT_RETRY_US;
        return;
    }
    j->busy = 0;
    if (outcome == CSL_NO_KEY || j->attempts >= CSL_ATTEMPTS) {
        drop_datagram(n, outcome);
        return;
    }
    report(n, j, outcome, false);
    n->not_before = now + CSL_RETRY_GAP_US;
}

/* Seal the head job of n into g.frame. */
static bool build_frame(struct neighbor *n, struct job *j) {
    const struct ccm_key *k = g.cfg.key[j->key];
    if (!k || !k->set) {
        return false;
    }
    if (!j->seq_set) {
        j->seq     = g.seq++;
        j->seq_set = true;
    }
    g.counter_before = g.counter;
    uint32_t counter = g.counter++;
    size_t   len;
    if (j->kind == KIND_BROADCAST) {
        len = mac_build_broadcast(
            k, g.frame, sizeof g.frame, false, j->seq, g.cfg.own, counter, j->body, j->len);
    } else {
        /* Frame Pending: more fragments follow, or another frame for this
         * device waits behind this one (freedo docs/PROTOCOL.md:1326-1330). */
        bool pending = (j->flags & CSL_JOB_MORE) || n->count > 1;
        len          = mac_build_data(k,
                                      g.frame,
                                      sizeof g.frame,
                                      pending,
                                      j->seq,
                                      n->dst,
                                      g.cfg.own,
                                      counter,
                                      j->body,
                                      j->len);
    }
    g.frame_len = (uint8_t)len;
    return len != 0;
}

/* Set up the timing of the head job of neighbour i (freedo cslStart). */
static void start(unsigned i, uint64_t now) {
    struct neighbor *n = &g.nb[i];
    struct job      *j = &g.jobs[n->head];

    g.nb_idx      = (uint8_t)i;
    g.cursor      = (uint8_t)((i + 1u) % CSL_NEIGHBORS);
    g.sent        = 0;
    g.saw_invalid = false;
    g.anchored    = false;
    j->attempts++;
    if (!build_frame(n, j)) {
        g.state = ST_WAIT; /* finish() expects a transaction */
        finish(CSL_NO_KEY);
        return;
    }

    uint32_t       lead = csl_port_random() % CSL_LBT_SLOTS * CSL_LBT_SLOT_US + CSL_LBT_LISTEN_US;
    struct anchor *a    = j->kind == KIND_DATA ? anchor_for(n->dst) : NULL;
    if (a) {
        uint32_t guard  = 0;
        uint64_t sample = anchor_next_sample(a, now + lead, &guard);
        g.start_us      = sample - guard;
        g.wake_end_us   = sample + guard;
        g.anchored      = true;
    } else {
        g.start_us    = now + lead;
        g.wake_end_us = g.start_us + (j->kind == KIND_DATA ? CSL_BOOTSTRAP_US : CSL_PERIOD_US);
    }
    g.listen_us   = g.start_us - CSL_LBT_LISTEN_US;
    g.deadline_us = g.wake_end_us + CSL_ACK_WINDOW_US + CSL_TXN_MARGIN_US;
    g.state       = ST_WAIT;
}

/* Pick the neighbour whose train starts first, round-robin on a tie
 * (freedo cslScheduleNext). Returns the wait until a queue becomes ready
 * when none is. */
/* Wait between checks while the counter reservation is used up. */
#define COUNTER_WAIT_US 100000u

static uint32_t schedule(uint64_t now) {
    /* Each start() takes one counter (build_frame), so this check covers
     * every attempt (#37). */
    if (g.counter >= g.counter_limit) {
        return COUNTER_WAIT_US;
    }
    int      best       = -1;
    uint64_t best_start = 0, soonest = UINT64_MAX;
    for (unsigned step = 0; step < CSL_NEIGHBORS; step++) {
        unsigned         i = (g.cursor + step) % CSL_NEIGHBORS;
        struct neighbor *n = &g.nb[i];
        if (!n->used || n->count == 0) {
            continue;
        }
        if (n->not_before > now) {
            soonest = n->not_before < soonest ? n->not_before : soonest;
            continue;
        }
        uint64_t       st = now;
        struct anchor *a  = g.jobs[n->head].kind == KIND_DATA ? anchor_for(n->dst) : NULL;
        if (a) {
            uint32_t guard = 0;
            st             = anchor_next_sample(a, now + CSL_LBT_MAX_US, &guard) - guard;
        }
        if (best < 0 || st < best_start) {
            best       = (int)i;
            best_start = st;
        }
    }
    if (best >= 0) {
        start((unsigned)best, now);
        return 0;
    }
    if (soonest == UINT64_MAX) {
        return UINT32_MAX;
    }
    return soonest - now > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)(soonest - now);
}

static unsigned tx_flags(const struct job *j) {
    return (j->flags & CSL_JOB_PAIRING) ? TXGATE_PAIRING : 0u;
}

static bool send_wake(const struct neighbor *n, const struct job *j, uint16_t rendezvous) {
    uint8_t w[MAC_WAKE_LEN];
    /* The short address is the first two address bytes swapped (freedo
     * tado/net.go:57-64, cslParseTx :1696); ff ff wakes every device. */
    uint8_t dst_short[2] = {n->dst[1], n->dst[0]};
    mac_build_wakeup(w, j->seq, dst_short, rendezvous);
    return csl_port_send(w, sizeof w, tx_flags(j));
}

/* The last wake-up frame, then the Data frame at once (freedo cslSendData). */
static void send_data(void) {
    struct neighbor *n = &g.nb[g.nb_idx];
    struct job      *j = &g.jobs[n->head];
    if (g.sent == 0 || !send_wake(n, j, 0) || !csl_port_send(g.frame, g.frame_len, tx_flags(j))) {
        finish(CSL_TX_FAILED);
        return;
    }
    g.sent++;
    if (j->kind == KIND_BROADCAST) {
        finish(CSL_DELIVERED);
        return;
    }
    g.data_sent_us  = csl_port_now();
    g.window_end_us = g.data_sent_us + CSL_ACK_WINDOW_US;
    g.state         = ST_ACK;
}

static uint32_t until(uint64_t t, uint64_t now) {
    return t > now ? (uint32_t)(t - now) : 0u;
}

uint32_t csl_step(void) {
    uint64_t now = csl_port_now();
    if (g.state == ST_IDLE) {
        uint32_t wait = schedule(now);
        if (g.state == ST_IDLE) {
            return wait;
        }
    }
    if (now >= g.deadline_us) { /* never while idle: a transaction overran */
        finish(CSL_TX_FAILED);
        return 0;
    }
    struct neighbor *n = &g.nb[g.nb_idx];
    struct job      *j = &g.jobs[n->head];
    switch (g.state) {
        case ST_WAIT:
            if (now < g.listen_us) {
                return until(g.listen_us, now);
            }
            csl_port_listen_begin();
            g.state = ST_LISTEN;
            return until(g.start_us, now);

        case ST_LISTEN:
            if (now < g.start_us) {
                return until(g.start_us, now);
            }
            if (csl_port_listen_busy()) {
                finish(CSL_BUSY);
                return 0;
            }
            g.state = ST_WAKE;
            return 0;

        case ST_WAKE: {
            /* The wake window closes on time: the Data frame has to land on
             * the device's sample. */
            if (now >= g.wake_end_us) {
                send_data();
                return 0;
            }
            uint64_t left = g.wake_end_us - now;
            if (left < CSL_UNIT_US || g.sent >= CSL_WAKE_CAP) {
                return (uint32_t)left;
            }
            uint32_t rendezvous = (uint32_t)(left / CSL_UNIT_US);
            if (j->kind == KIND_BROADCAST) {
                /* The first frame announces a whole period, the rest count
                 * down (freedo cslStepWake). */
                if (g.sent == 0 || rendezvous > MAC_CSL_UNITS) {
                    rendezvous = MAC_CSL_UNITS;
                }
            }
            if (!send_wake(n, j, (uint16_t)rendezvous)) {
                finish(CSL_TX_FAILED);
                return 0;
            }
            g.sent++;
            return 0;
        }

        case ST_ACK:
            if (now < g.window_end_us) {
                return until(g.window_end_us, now);
            }
            anchor_miss(n->dst);
            finish(g.saw_invalid ? CSL_BAD_ACK : CSL_NO_ACK);
            return 0;

        default:
            return UINT32_MAX;
    }
}

bool csl_on_frame(const uint8_t *frame, size_t len, bool crc_ok, uint64_t end_us) {
    if (g.state != ST_ACK) {
        return false;
    }
    if (!crc_ok || !mac_ack_answers(frame, len, g.frame)) {
        /* Only a frame that can be a damaged Enhanced ACK turns "no ACK"
         * into "bad ACK": a CRC failure or the Enhanced ACK frame control.
         * An uplink of another device says nothing about this Data frame. */
        if (!crc_ok || mac_classify(frame, len) == MAC_ENH_ACK) {
            g.saw_invalid = true;
        }
        return false;
    }
    /* An end-of-frame stamp from before our Data frame is not this ACK's:
     * an anchor placed on it misses by up to a period. */
    if (end_us < g.data_sent_us) {
        end_us = csl_port_now();
    }
    struct neighbor *n = &g.nb[g.nb_idx];
    csl_anchor_store(n->dst, (uint16_t)(frame[21] | frame[22] << 8), end_us);
    finish(CSL_DELIVERED);
    return true;
}

bool csl_may_ack(void) {
    return g.state != ST_WAKE && g.state != ST_ACK;
}

bool csl_busy(void) {
    if (g.state == ST_IDLE) {
        return false;
    }
    /* Waiting for a sample that is still far away leaves the bus free. */
    return g.state != ST_WAIT || until(g.listen_us, csl_port_now()) < 10000u;
}
