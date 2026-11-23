/* Host tests for the CSL coordinator, src/csl.c (#17), against a simulated
 * radio and simulated sleeping devices: virtual time, frames with their
 * real airtime, devices that sample once a second with a clock error and
 * answer a Data frame with an Enhanced ACK. */
#include "csl.h"

#include "txgate.h"

#include "test.h"
#include "vectors_freedo.h"

#include <stdlib.h>

/* --- Simulated radio --------------------------------------------------------- */
/* The network's PAN in every configuration here (#43). */
#define TEST_PAN    MAC_PAN_TADO
#define BYTE_US     160u /* 50 kbps */
#define TX_SETUP_US 250u /* strobes, FIFO write, IDLE to TX */
#define ACK_TURN_US 370u /* a device answers 0.35-0.39 ms after the Data frame */

struct sent {
    uint64_t start_us, end_us;
    uint8_t  len;
    unsigned flags;
    uint8_t  frame[MAC_FRAME_MAX];
};

static uint64_t    now_us;
static struct sent sent[4096];
static unsigned    n_sent;
static bool        tx_fail, channel_busy;
static unsigned    listens;
static uint64_t    listen_at;
static uint32_t    next_random;

static struct csl_report reports[256];
static unsigned          n_reports;

static struct ccm_key key;
static const uint8_t  own[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0xC5, 0x1B, 0x00};

/* A sleeping device: samples at phase_us + k x period on its own clock. */
struct device {
    uint8_t  eui[8];
    uint64_t first_sample_us;
    int      ppm;      /* clock error: its period is 1 s x (1 + ppm / 1e6) */
    bool     deaf;     /* does not answer */
    bool     bad_ack;  /* answers with a wrong FCS */
    unsigned heard;    /* Data frames taken */
    uint8_t  last_seq;
    uint32_t last_counter;
    bool     last_pending;
};
static struct device *devices[4];
static unsigned       n_devices;

static uint64_t dev_period(const struct device *d) {
    return (uint64_t)(1000000 + d->ppm);
}

/* The device's last sample at or before t. */
static uint64_t dev_last_sample(const struct device *d, uint64_t t) {
    if (t < d->first_sample_us) {
        return d->first_sample_us;
    }
    return d->first_sample_us + (t - d->first_sample_us) / dev_period(d) * dev_period(d);
}

/* Did a sample instant of d fall into [from, to]? */
static bool dev_sampled(const struct device *d, uint64_t from, uint64_t to) {
    uint64_t s = dev_last_sample(d, to);
    return s >= from && s <= to;
}

uint64_t csl_port_now(void) {
    return now_us;
}

uint32_t csl_port_random(void) {
    return next_random;
}

void csl_port_listen_begin(void) {
    listens++;
    listen_at = now_us;
}

bool csl_port_listen_busy(void) {
    return channel_busy;
}

void csl_port_report(const struct csl_report *r) {
    if (n_reports < 256) {
        reports[n_reports++] = *r;
    }
}

/* Start of the train in flight (first wake-up frame with this seq). */
static uint64_t train_start;
static int      train_seq = -1;

static void deliver_ack(struct device *d, const uint8_t *data, size_t len, uint64_t data_end);

bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    if (tx_fail) {
        return false;
    }
    /* The same rules the real driver applies. */
    static const struct txgate_radio radio = {0x21, 0x32, 0x76, TXGATE_TEST_CHANNEL, 0x22, 0xF8, 0};
    CHECK_EQ(txgate_check_frame(&radio, own, frame, len, NULL, flags), TXGATE_OK);

    struct sent *s = &sent[n_sent < 4096 ? n_sent++ : 4095];
    s->start_us    = now_us + TX_SETUP_US;
    s->len         = (uint8_t)len;
    s->flags       = flags;
    memcpy(s->frame, frame, len);
    now_us   = s->start_us + (4u + 4u + 1u + len + 2u) * BYTE_US;
    s->end_us = now_us;

    enum mac_class c = mac_classify(frame, len);
    if (c == MAC_WAKEUP) {
        if (train_seq != frame[1]) {
            train_seq   = frame[1];
            train_start = s->start_us;
        }
    } else if (c == MAC_DATA || c == MAC_DATA_PENDING) {
        for (unsigned i = 0; i < n_devices; i++) {
            struct device *d = devices[i];
            /* A device takes the Data frame if one of its samples fell
             * inside the train that announced it. */
            if (memcmp(frame + 3, d->eui, 8) == 0 && train_seq == frame[2]
                && dev_sampled(d, train_start, s->start_us)) {
                deliver_ack(d, frame, len, s->end_us);
            }
        }
        train_seq = -1;
    } else {
        train_seq = -1;
    }
    return true;
}

/* The Enhanced ACK of a device, handed to the state machine when the test
 * loop reaches its arrival time. */
static struct {
    bool     pending, crc_ok;
    uint64_t end_us;
    uint8_t  frame[MAC_ACK_LEN];
} ack;

static void deliver_ack(struct device *d, const uint8_t *data, size_t len, uint64_t data_end) {
    uint8_t         pt[MAC_FRAME_MAX];
    struct mac_data m;
    CHECK_EQ(mac_open_data(&key, data, len, pt, sizeof pt, &m), MAC_OPEN_OK);
    d->heard++;
    d->last_seq     = m.seq;
    d->last_counter = m.counter;
    d->last_pending = m.pending;
    if (d->deaf) {
        return;
    }
    uint64_t t     = data_end + ACK_TURN_US + (4u + 4u + 1u + MAC_ACK_LEN + 2u) * BYTE_US;
    uint64_t phase = (t - dev_last_sample(d, t)) / CSL_UNIT_US;
    mac_build_enh_ack(ack.frame, m.seq, own, d->eui, (uint16_t)phase);
    ack.crc_ok  = !d->bad_ack;
    ack.end_us  = t;
    ack.pending = true;
}

/* Run the state machine until virtual time `until_us`. */
static void run_until(uint64_t until_us) {
    for (unsigned guard = 0; guard < 2000000 && now_us < until_us; guard++) {
        if (ack.pending && now_us >= ack.end_us) {
            ack.pending = false;
            (void)csl_on_frame(ack.frame, MAC_ACK_LEN, ack.crc_ok, ack.end_us);
        }
        uint32_t wait = csl_step();
        if (wait == 0) {
            continue;
        }
        uint64_t next = wait == UINT32_MAX ? until_us : now_us + wait;
        if (ack.pending && ack.end_us < next) {
            next = ack.end_us;
        }
        now_us = next < until_us ? next : until_us;
    }
}

static void reset(void) {
    struct csl_config cfg = {.pan = TEST_PAN, .key = {&key, NULL}, .counter = 1000, .counter_limit = UINT32_MAX, .seq = 0x40};
    memcpy(cfg.own, own, 8);
    csl_init(&cfg);
    now_us       = 5000000;
    n_sent       = 0;
    n_reports    = 0;
    tx_fail      = false;
    channel_busy = false;
    listens      = 0;
    next_random  = 0;
    n_devices    = 0;
    ack.pending  = false;
    train_seq    = -1;
}

static struct device *add_device(struct device *d, uint8_t tag, uint64_t first_sample, int ppm) {
    memset(d, 0, sizeof *d);
    d->eui[0] = tag;
    d->eui[1] = (uint8_t)(tag + 1);
    d->eui[2] = 0x33;
    d->eui[3] = 0x44;
    d->eui[4] = 0x55;
    memcpy(d->eui + 5, mac_eui_suffix, 3);
    d->first_sample_us    = first_sample;
    d->ppm                = ppm;
    devices[n_devices++] = d;
    return d;
}

static const uint8_t body[20] = {0x7E, 0x33, 0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

static unsigned count_class(enum mac_class c, unsigned from) {
    unsigned n = 0;
    for (unsigned i = from; i < n_sent; i++) {
        n += mac_classify(sent[i].frame, sent[i].len) == c;
    }
    return n;
}

/* --- Tests ------------------------------------------------------------------- */
static void test_bootstrap_then_anchored(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 123456, 0);
    next_random = 3; /* backoff 3 x 640 us */

    uint64_t t0 = now_us;
    CHECK(!csl_anchor_known(d.eui));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL - 1);
    run_until(t0 + 1500000);

    /* Listen before transmit: the listen starts after the backoff and lasts
     * 3.968 ms, and the train starts where it ends. */
    CHECK_EQ(listens, 1);
    CHECK_EQ(listen_at - t0, 3 * CSL_LBT_SLOT_US);
    CHECK_EQ(sent[0].start_us - TX_SETUP_US - listen_at, CSL_LBT_LISTEN_US);

    /* A full-period train: wake-up frames back to back, the rendezvous time
     * counting down to 0, all with the seq of the Data frame that follows. */
    unsigned wakes = count_class(MAC_WAKEUP, 0);
    CHECK(wakes > 240 && wakes < 270);
    CHECK_EQ(n_sent, wakes + 1);
    const struct sent *data = &sent[n_sent - 1];
    CHECK_EQ(mac_classify(data->frame, data->len), MAC_DATA);
    uint16_t prev = 0xFFFF;
    for (unsigned i = 0; i < wakes; i++) {
        struct mac_wakeup w;
        CHECK(mac_parse_wakeup(sent[i].frame, sent[i].len, &w));
        CHECK_EQ(w.seq, data->frame[2]);
        CHECK_EQ(w.pan, TEST_PAN);
        CHECK_EQ(w.dst_short[0], d.eui[1]);
        CHECK_EQ(w.dst_short[1], d.eui[0]);
        CHECK(w.rendezvous < prev);
        prev = w.rendezvous;
        if (i > 0) { /* back to back: under 4.5 ms from start to start */
            CHECK(sent[i].start_us - sent[i - 1].start_us < 4500);
        }
        /* The announced time is the time left to the Data frame, within
         * the two frames that still have to go out. */
        long long left = (long long)(data->start_us - sent[i].start_us);
        long long said = (long long)w.rendezvous * CSL_UNIT_US;
        CHECK(left - said > -400 && left - said < 9000);
    }
    CHECK_EQ(prev, 0);
    CHECK(sent[0].start_us + CSL_BOOTSTRAP_US <= data->start_us);
    CHECK(data->start_us - sent[0].start_us < CSL_BOOTSTRAP_US + 10000);

    CHECK_EQ(d.heard, 1);
    CHECK_EQ(d.last_seq, 0x40);
    CHECK_EQ(d.last_counter, 1000);
    CHECK(!d.last_pending);
    CHECK_EQ(n_reports, 1);
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK(reports[0].final && !reports[0].anchored);
    CHECK_EQ(reports[0].attempt, 1);
    CHECK_EQ(reports[0].id, 1);
    CHECK(csl_anchor_known(d.eui));
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    CHECK_EQ(csl_counter(), 1001);

    /* Second frame: a short train around the device's next sample. */
    unsigned first = n_sent;
    uint64_t t1    = now_us;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 2, 0, CSL_KEY_NETWORK));
    run_until(t1 + 1500000);
    wakes = count_class(MAC_WAKEUP, first);
    CHECK(wakes >= 19 && wakes <= 23); /* about 80 ms of 3.9 ms frames */
    CHECK_EQ(n_sent - first, wakes + 1);
    uint64_t sample = dev_last_sample(&d, sent[n_sent - 1].start_us);
    long long before = (long long)(sample - sent[first].start_us);
    long long after  = (long long)(sent[n_sent - 1].start_us - sample);
    CHECK(before > 35000 && before < 45000); /* 40 ms guard each side */
    CHECK(after > 35000 && after < 50000);
    CHECK_EQ(d.heard, 2);
    CHECK_EQ(d.last_seq, 0x41);
    CHECK_EQ(d.last_counter, 1001);
    CHECK_EQ(reports[1].outcome, CSL_DELIVERED);
    CHECK(reports[1].anchored);
    CHECK_EQ(listens, 2);
}

static void test_frame_pending(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 400000, 0);
    /* Two fragments of one datagram, then a separate frame. */
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 7, CSL_JOB_MORE, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 7, 0, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 8, 0, CSL_KEY_NETWORK));
    uint64_t t0 = now_us;
    run_until(t0 + 1300000);
    CHECK_EQ(d.heard, 1);
    CHECK(d.last_pending); /* more fragments */
    run_until(t0 + 2300000);
    CHECK_EQ(d.heard, 2);
    CHECK(d.last_pending); /* another frame waits in the queue */
    run_until(t0 + 3300000);
    CHECK_EQ(d.heard, 3);
    CHECK(!d.last_pending); /* the last one */
    CHECK_EQ(n_reports, 3);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
}

static void test_no_ack_retries(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 250000, 0);
    d.deaf = true;
    /* A datagram of three fragments and one unrelated frame behind it. */
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 5, CSL_JOB_MORE, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 5, CSL_JOB_MORE, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 5, 0, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 6, 0, CSL_KEY_NETWORK));
    uint64_t t0 = now_us;
    run_until(t0 + 12000000);

    /* Three attempts of the first fragment: same seq, next counter each
     * time, at least 2 s apart; then the datagram is dropped. */
    unsigned data_idx[8], nd = 0;
    for (unsigned i = 0; i < n_sent && nd < 8; i++) {
        if (mac_classify(sent[i].frame, sent[i].len) == MAC_DATA_PENDING
            || mac_classify(sent[i].frame, sent[i].len) == MAC_DATA) {
            data_idx[nd++] = i;
        }
    }
    CHECK(nd >= 3);
    for (unsigned k = 0; k < 3 && k < nd; k++) {
        uint8_t         b[MAC_FRAME_MAX];
        struct mac_data m;
        const struct sent *s = &sent[data_idx[k]];
        CHECK_EQ(mac_open_data(&key, s->frame, s->len, b, sizeof b, &m), MAC_OPEN_OK);
        CHECK_EQ(m.seq, 0x40);
        CHECK_EQ(m.counter, 1000 + k);
        if (k > 0) {
            CHECK(s->start_us - sent[data_idx[k - 1]].end_us >= CSL_RETRY_GAP_US);
        }
    }
    CHECK_EQ(reports[0].outcome, CSL_NO_ACK);
    CHECK(!reports[0].final);
    CHECK_EQ(reports[1].attempt, 2);
    CHECK(!reports[1].final);
    CHECK_EQ(reports[2].outcome, CSL_NO_ACK);
    CHECK(reports[2].final);
    CHECK_EQ(reports[2].attempt, 3);
    /* The other two fragments go with it, unsent. */
    CHECK_EQ(reports[3].outcome, CSL_ABANDONED);
    CHECK_EQ(reports[4].outcome, CSL_ABANDONED);
    CHECK_EQ(reports[3].id, 5);
    /* The unrelated frame is still tried, with its own seq. */
    CHECK(nd >= 4);
    if (nd >= 4) {
        CHECK_EQ(sent[data_idx[3]].frame[2], 0x41);
    }
    run_until(t0 + 30000000);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    /* It heard every Data frame (three attempts each of two jobs) and
     * never answered. */
    CHECK_EQ(d.heard, 2 * CSL_ATTEMPTS);
    CHECK_EQ(count_class(MAC_DATA, 0) + count_class(MAC_DATA_PENDING, 0), 2 * CSL_ATTEMPTS);
}

static void test_anchor_miss_falls_back(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 250000, 0);
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    run_until(now_us + 1500000);
    CHECK(csl_anchor_known(d.eui));

    /* The device restarts at another phase: the short train misses it, the
     * retry is a full train and finds it again. */
    d.first_sample_us += 500000;
    unsigned first = n_sent;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 2, 0, CSL_KEY_NETWORK));
    run_until(now_us + 6000000);
    CHECK_EQ(reports[1].outcome, CSL_NO_ACK);
    CHECK(reports[1].anchored && !reports[1].final);
    CHECK_EQ(reports[2].outcome, CSL_DELIVERED);
    CHECK(!reports[2].anchored);
    CHECK_EQ(reports[2].attempt, 2);
    CHECK(count_class(MAC_WAKEUP, first) > 250);

    /* Four misses in a row drop the anchor. */
    d.deaf = true;
    for (uint16_t id = 3; id < 5; id++) {
        CHECK(csl_enqueue_data(d.eui, body, sizeof body, id, 0, CSL_KEY_NETWORK));
    }
    run_until(now_us + 30000000);
    CHECK(!csl_anchor_known(d.eui));
}

static void test_bad_ack(void) {
    reset();
    struct device d, other;
    add_device(&d, 0xA0, 250000, 0);
    add_device(&other, 0xB0, 250000, 0);
    d.bad_ack = true; /* its ACK arrives with a bad radio CRC */
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    run_until(now_us + 1500000);
    CHECK_EQ(reports[0].outcome, CSL_BAD_ACK);
    CHECK(!csl_anchor_known(d.eui));

    /* An ACK for another seq, or from another device, closes nothing. */
    reset();
    add_device(&d, 0xA0, 250000, 0);
    d.deaf = true;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    uint64_t t0 = now_us;
    while (d.heard == 0 && now_us < t0 + 2000000) {
        run_until(now_us + 100);
    }
    CHECK_EQ(d.heard, 1);
    CHECK(!csl_may_ack()); /* waiting for the device's ACK */
    uint8_t a[MAC_ACK_LEN];
    mac_build_enh_ack(a, 0x41, own, d.eui, 0);
    CHECK(!csl_on_frame(a, sizeof a, true, now_us));
    mac_build_enh_ack(a, 0x40, own, other.eui, 0);
    CHECK(!csl_on_frame(a, sizeof a, true, now_us));
    mac_build_enh_ack(a, 0x40, d.eui, own, 0); /* our own ACK format, reversed */
    CHECK(!csl_on_frame(a, sizeof a, true, now_us));
    /* An uplink of another device in the window is no damaged ACK. */
    uint8_t up[MAC_FRAME_MAX];
    size_t  n = mac_build_data(&key, up, sizeof up, false, 9, own, other.eui, 1, body, sizeof body);
    CHECK(!csl_on_frame(up, n, true, now_us));
    /* The right one does, and its phase places the anchor. */
    mac_build_enh_ack(a, 0x40, own, d.eui, 100);
    CHECK(csl_on_frame(a, sizeof a, true, now_us));
    CHECK(csl_may_ack());
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK(csl_anchor_known(d.eui));
    CHECK(!csl_on_frame(a, sizeof a, true, now_us)); /* no transaction any more */
}

static void test_listen_busy(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 250000, 0);
    channel_busy = true;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    uint64_t t0 = now_us;
    run_until(t0 + 100000);
    /* Nothing was sent; the job waits and listens again 20 ms later. */
    CHECK_EQ(n_sent, 0);
    CHECK(listens >= 3 && listens <= 5);
    CHECK_EQ(reports[0].outcome, CSL_BUSY);
    CHECK(!reports[0].final);
    CHECK_EQ(reports[0].wake_frames, 0);
    CHECK_EQ(csl_counter(), 1000); /* no frame counter used up */
    CHECK(csl_may_ack());          /* the receiver may answer uplinks meanwhile */

    /* The channel clears: delivered on the first attempt. */
    channel_busy = false;
    run_until(t0 + 1500000);
    CHECK_EQ(d.heard, 1);
    CHECK_EQ(d.last_counter, 1000);
    CHECK_EQ(reports[n_reports - 1].outcome, CSL_DELIVERED);
    CHECK_EQ(reports[n_reports - 1].attempt, 1);

    /* A channel that never clears: every CSL_LBT_BUSY_MAX busy listens count
     * as one failed attempt, and the job is dropped after three. */
    reset();
    add_device(&d, 0xA0, 250000, 0);
    channel_busy = true;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    run_until(now_us + 20000000);
    CHECK_EQ(n_sent, 0);
    CHECK_EQ(listens, CSL_ATTEMPTS * CSL_LBT_BUSY_MAX);
    CHECK_EQ(reports[n_reports - 1].outcome, CSL_BUSY);
    CHECK(reports[n_reports - 1].final);
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
}

static void test_tx_failure_and_no_key(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 250000, 0);
    tx_fail = true;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    run_until(now_us + 100000);
    CHECK_EQ(reports[0].outcome, CSL_TX_FAILED);
    CHECK(!reports[0].final);
    tx_fail = false;
    run_until(now_us + 4000000);
    CHECK_EQ(reports[n_reports - 1].outcome, CSL_DELIVERED);
    CHECK_EQ(reports[n_reports - 1].attempt, 2);

    /* A job for a key that is not set fails at once and sends nothing. */
    unsigned before = n_sent;
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 2, 0, CSL_KEY_PAIRING));
    run_until(now_us + 100000);
    CHECK_EQ(n_sent, before);
    CHECK_EQ(reports[n_reports - 1].outcome, CSL_NO_KEY);
    CHECK(reports[n_reports - 1].final);
}

static void test_broadcast(void) {
    reset();
    uint8_t pkt[84] = {0x7B, 0x3B, 58, 1, 134};
    next_random     = 6;
    uint64_t t0     = now_us;
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 9, 0, CSL_KEY_NETWORK));
    run_until(t0 + 1200000);
    unsigned wakes = count_class(MAC_WAKEUP, 0);
    CHECK(wakes > 240 && wakes < 270);
    CHECK_EQ(n_sent, wakes + 1);
    CHECK_EQ(sent[0].start_us - TX_SETUP_US - t0, CSL_LBT_MAX_US);
    struct mac_wakeup w;
    CHECK(mac_parse_wakeup(sent[0].frame, sent[0].len, &w));
    CHECK_EQ(w.rendezvous, MAC_CSL_UNITS); /* the first frame announces a whole period */
    CHECK_EQ(w.dst_short[0], 0xFF);
    CHECK_EQ(w.dst_short[1], 0xFF);
    CHECK(mac_parse_wakeup(sent[wakes - 1].frame, MAC_WAKE_LEN, &w));
    CHECK_EQ(w.rendezvous, 0);
    const struct sent *b = &sent[n_sent - 1];
    CHECK_EQ(mac_classify(b->frame, b->len), MAC_BROADCAST);
    CHECK_EQ(b->frame[0], 0x49);
    uint8_t out[MAC_FRAME_MAX];
    size_t  n = 0;
    CHECK_EQ(mac_open(&key, b->frame, b->len, out, sizeof out, &n), MAC_OPEN_OK);
    CHECK_EQ(n, 2 + sizeof pkt);
    CHECK_MEM(out + 2, pkt, sizeof pkt);
    /* Sent means done: no ACK window. */
    CHECK_EQ(n_reports, 1);
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK(csl_may_ack());
}

static void test_limits(void) {
    reset();
    uint8_t dst[8] = {0, 1, 2, 3, 4, 0xC5, 0x1B, 0x00};
    uint8_t big[MAC_DATA_MAX_DGRAM + 1] = {0};
    CHECK(!csl_enqueue_data(dst, big, sizeof big, 1, 0, CSL_KEY_NETWORK));
    CHECK(!csl_enqueue_data(dst, big, 0, 1, 0, CSL_KEY_NETWORK));
    CHECK(!csl_enqueue_data(own, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    static const uint8_t ff[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!csl_enqueue_data(ff, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    CHECK(!csl_enqueue_broadcast(big, MAC_BCAST_MAX_PKT + 1, 1, 0, CSL_KEY_NETWORK));

    /* One job each for 16 devices, then the 17th device is refused. */
    for (unsigned i = 0; i < CSL_ANCHORS; i++) {
        dst[0] = (uint8_t)(i + 1);
        CHECK(csl_enqueue_data(dst, body, sizeof body, (uint16_t)i, 0, CSL_KEY_NETWORK));
    }
    dst[0] = 99;
    CHECK(!csl_enqueue_data(dst, body, sizeof body, 99, 0, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_broadcast(body, sizeof body, 100, 0, CSL_KEY_NETWORK));
    /* Fill the pool. */
    dst[0] = 1;
    while (csl_free_jobs()) {
        CHECK(csl_enqueue_data(dst, body, sizeof body, 200, 0, CSL_KEY_NETWORK));
    }
    CHECK(!csl_enqueue_data(dst, body, sizeof body, 201, 0, CSL_KEY_NETWORK));
    csl_flush();
    CHECK_EQ(csl_free_jobs(), CSL_JOB_POOL);
    CHECK_EQ(csl_step(), UINT32_MAX);
    CHECK(!csl_busy());
}

static void test_two_devices(void) {
    reset();
    struct device a, b;
    add_device(&a, 0xA0, 100000, 0);
    add_device(&b, 0xB0, 600000, 0);
    for (uint16_t i = 0; i < 3; i++) {
        CHECK(csl_enqueue_data(a.eui, body, sizeof body, (uint16_t)(10 + i), 0, CSL_KEY_NETWORK));
        CHECK(csl_enqueue_data(b.eui, body, sizeof body, (uint16_t)(20 + i), 0, CSL_KEY_NETWORK));
    }
    run_until(now_us + 8000000);
    CHECK_EQ(a.heard, 3);
    CHECK_EQ(b.heard, 3);
    CHECK_EQ(n_reports, 6);
    for (unsigned i = 0; i < n_reports; i++) {
        CHECK_EQ(reports[i].outcome, CSL_DELIVERED);
    }
    /* With both anchors known the trains alternate: each device's sample
     * comes round before the other's next one. */
    CHECK(reports[2].anchored && reports[3].anchored);
    CHECK(memcmp(reports[2].dst, reports[3].dst, 8) != 0);
    CHECK(memcmp(reports[4].dst, reports[5].dst, 8) != 0);
}

/* Anchored trains keep covering the device's sample with a clock error and
 * long gaps between refreshes (freedo README.md:137-156: device clocks
 * within -6 to +18 ppm, refreshes mostly under 300 s; here -25 to +25 ppm
 * and gaps up to an hour). */
static void test_drift(void) {
    static const int      ppms[] = {-25, -6, 0, 18, 25};
    static const unsigned gaps[] = {1, 27, 300, 3600};
    for (unsigned p = 0; p < sizeof ppms / sizeof ppms[0]; p++) {
        reset();
        struct device d;
        add_device(&d, 0xA0, 777777, ppms[p]);
        CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
        run_until(now_us + 1500000);
        CHECK_EQ(d.heard, 1);
        for (unsigned k = 0; k < sizeof gaps / sizeof gaps[0]; k++) {
            run_until(now_us + (uint64_t)gaps[k] * 1000000u);
            unsigned first = n_sent, r0 = n_reports;
            CHECK(csl_enqueue_data(d.eui, body, sizeof body, (uint16_t)(2 + k), 0,
                                   CSL_KEY_NETWORK));
            run_until(now_us + 2500000);
            CHECK_EQ(n_reports, r0 + 1);
            CHECK_EQ(reports[r0].outcome, CSL_DELIVERED);
            CHECK(reports[r0].anchored);
            CHECK_EQ(reports[r0].attempt, 1);
            /* The guard grows with the age of the anchor: 40 ms plus 30 ppm
             * each side. */
            unsigned  wakes  = count_class(MAC_WAKEUP, first);
            long long expect = 2 * (40000 + 30ll * gaps[k]) / 3900;
            CHECK(wakes >= expect - 3 && wakes <= expect + 4);
        }
    }
}

static void test_pairing_flag(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 250000, 0);
    static struct ccm_key pair;
    ccm_set_key(&pair, v_key2.p);
    struct csl_config cfg = {.pan = TEST_PAN, .key = {&key, &pair}, .counter = 5, .counter_limit = UINT32_MAX, .seq = 1};
    memcpy(cfg.own, own, 8);
    csl_init(&cfg);
    CHECK(csl_enqueue_broadcast(body, sizeof body, 1, CSL_JOB_PAIRING, CSL_KEY_PAIRING));
    run_until(now_us + 1200000);
    CHECK(n_sent > 200);
    for (unsigned i = 0; i < n_sent; i++) {
        CHECK_EQ(sent[i].flags, TXGATE_PAIRING);
    }
    uint8_t out[MAC_FRAME_MAX];
    size_t  n = 0;
    CHECK_EQ(mac_open(&pair, sent[n_sent - 1].frame, sent[n_sent - 1].len, out, sizeof out, &n),
             MAC_OPEN_OK);
    CHECK_EQ(mac_open(&key, sent[n_sent - 1].frame, sent[n_sent - 1].len, out, sizeof out, &n),
             MAC_OPEN_MIC);
}

/* #37: nothing starts at or above the counter reservation, and the limit
 * only moves up. */
static void test_counter_limit(void) {
    reset();
    struct csl_config cfg = {.pan = TEST_PAN, .key = {&key, NULL}, .counter = 1000, .counter_limit = 1000, .seq = 1};
    memcpy(cfg.own, own, 8);
    csl_init(&cfg);
    uint8_t pkt[20] = {0x7B, 0x3B, 58, 1, 134};
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 9, 0, CSL_KEY_NETWORK));
    run_until(now_us + 3000000);
    CHECK_EQ(n_sent, 0);
    CHECK_EQ(n_reports, 0);
    CHECK_EQ(csl_counter(), 1000);

    csl_set_counter_limit(1001);
    CHECK_EQ(csl_counter_limit(), 1001);
    run_until(now_us + 1500000);
    CHECK(n_sent > 0);
    CHECK_EQ(n_reports, 1);
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK_EQ(csl_counter(), 1001);

    /* Used up again: the next one waits; a lower limit changes nothing. */
    unsigned before = n_sent;
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 9, 0, CSL_KEY_NETWORK));
    csl_set_counter_limit(500);
    CHECK_EQ(csl_counter_limit(), 1001);
    run_until(now_us + 3000000);
    CHECK_EQ(n_sent, before);
    CHECK_EQ(csl_counter(), 1001);
    csl_set_counter_limit(1002);
    run_until(now_us + 1500000);
    CHECK(n_sent > before);
    CHECK_EQ(csl_counter(), 1002);
}

/* #19: the pairing-window Router Advertisement pair: the network-key copy
 * with Frame Pending (59 e8), then the pairing-key copy (49 e8), with
 * consecutive seq and counter (freedo docs/PROTOCOL.md:668-672). */
static void test_ra_pair(void) {
    reset();
    static struct ccm_key pair;
    ccm_set_key(&pair, v_key2.p);
    struct csl_config cfg = {.pan = TEST_PAN, .key = {&key, &pair}, .counter = 70, .counter_limit = UINT32_MAX, .seq = 9};
    memcpy(cfg.own, own, 8);
    csl_init(&cfg);
    uint8_t pkt[84] = {0x7B, 0x3B, 58, 1, 134};
    CHECK_EQ(csl_broadcasts_queued(), 0);
    CHECK(csl_room_for_broadcasts(2));
    CHECK(csl_enqueue_broadcast(
        pkt, sizeof pkt, 0xFFFF, CSL_JOB_PAIRING | CSL_JOB_PENDING, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 0xFFFF, CSL_JOB_PAIRING, CSL_KEY_PAIRING));
    CHECK_EQ(csl_broadcasts_queued(), 2);
    run_until(now_us + 2600000);
    CHECK_EQ(csl_broadcasts_queued(), 0);
    CHECK_EQ(n_reports, 2);
    const struct sent *b[2] = {NULL, NULL};
    unsigned           nb = 0;
    for (unsigned i = 0; i < n_sent; i++) {
        if (mac_classify(sent[i].frame, sent[i].len) == MAC_BROADCAST && nb < 2) {
            b[nb++] = &sent[i];
        }
    }
    CHECK_EQ(nb, 2);
    if (nb == 2) {
        CHECK_EQ(b[0]->frame[0], 0x59);
        CHECK_EQ(b[1]->frame[0], 0x49);
        CHECK_EQ(b[1]->frame[2], (uint8_t)(b[0]->frame[2] + 1));
        uint8_t out[MAC_FRAME_MAX];
        size_t  n = 0, n2 = 0;
        CHECK_EQ(mac_open(&key, b[0]->frame, b[0]->len, out, sizeof out, &n), MAC_OPEN_OK);
        uint16_t c0 = (uint16_t)(b[0]->frame[14] | b[0]->frame[15] << 8);
        CHECK_EQ(mac_open(&pair, b[1]->frame, b[1]->len, out, sizeof out, &n2), MAC_OPEN_OK);
        uint16_t c1 = (uint16_t)(b[1]->frame[14] | b[1]->frame[15] << 8);
        CHECK_EQ(c0, 70);
        CHECK_EQ(c1, 71);
        CHECK_EQ(n, n2);
        CHECK_MEM(out + 2, pkt, sizeof pkt);
        CHECK_EQ(b[0]->flags, TXGATE_PAIRING);
        CHECK_EQ(b[1]->flags, TXGATE_PAIRING);
    }
    /* Not enough room for two: the caller queues neither. */
    reset();
    uint8_t dst[8] = {0, 1, 2, 3, 4, 0xC5, 0x1B, 0x00};
    while (csl_enqueue_data(dst, body, sizeof body, 1, 0, CSL_KEY_NETWORK)) {
    }
    CHECK(!csl_room_for_broadcasts(2));
    CHECK(csl_room_for_broadcasts(1));
}

/* #19: the beacon takes the next seq and counter; a transaction that then
 * finds the channel busy does not hand that counter out again, and nothing
 * is taken at the reservation limit. */
static void test_take_seq_counter(void) {
    reset();
    struct csl_config cfg = {.pan = TEST_PAN, .key = {&key, NULL}, .counter = 300, .counter_limit = 303, .seq = 0x20};
    memcpy(cfg.own, own, 8);
    csl_init(&cfg);
    uint8_t  seq = 0;
    uint32_t ctr = 0;
    CHECK(csl_take_seq_counter(&seq, &ctr));
    CHECK_EQ(seq, 0x20);
    CHECK_EQ(ctr, 300);
    CHECK_EQ(csl_counter(), 301);

    /* A broadcast seals with 301, the beacon takes 302 during its listen,
     * the listen finds the channel busy: the retry must not reuse 302. */
    uint8_t pkt[20] = {0x7B, 0x3B, 58, 1, 134};
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 9, 0, CSL_KEY_NETWORK));
    channel_busy = true;
    CHECK_EQ(csl_step(), CSL_LBT_LISTEN_US); /* sealed with 301, listening now */
    CHECK(csl_take_seq_counter(&seq, &ctr));
    CHECK_EQ(seq, 0x22);
    CHECK_EQ(ctr, 302);
    run_until(now_us + 30000); /* listen, busy */
    CHECK(n_reports >= 1);
    CHECK_EQ(reports[0].outcome, CSL_BUSY);
    CHECK_EQ(csl_counter(), 303);
    CHECK(!csl_take_seq_counter(&seq, &ctr)); /* at the limit */
    CHECK_EQ(csl_counter(), 303);
    channel_busy = false;
    csl_set_counter_limit(400);
    run_until(now_us + 1500000);
    const struct sent *b = &sent[n_sent - 1];
    CHECK_EQ(mac_classify(b->frame, b->len), MAC_BROADCAST);
    CHECK_EQ(b->frame[14] | b->frame[15] << 8, 303);
}

/* csl_quiet_for(), the beacon's gate (#19 review L2): quiet only while no
 * transaction holds the air and the waiting one's listen is far enough
 * away; csl_busy() is the same at 10 ms. */
static void test_quiet_for(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 300000, 0);
    CHECK(csl_quiet_for(UINT32_MAX)); /* idle, nothing queued */
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    CHECK(csl_quiet_for(UINT32_MAX)); /* idle: no listen is set up before csl_step() */
    run_until(now_us + 1500000);      /* bootstrap; the ACK gives an anchor */
    CHECK(csl_anchor_known(d.eui));
    CHECK(csl_quiet_for(UINT32_MAX));

    /* The next job waits for the device's sample, its listen far ahead. */
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 2, 0, CSL_KEY_NETWORK));
    uint32_t to_listen = csl_step();
    CHECK(to_listen > 100000u && to_listen < CSL_PERIOD_US);
    CHECK(csl_quiet_for(to_listen));
    CHECK(!csl_quiet_for(to_listen + 1u));
    CHECK(!csl_busy());
    now_us += to_listen - 15000u; /* the listen 15 ms away: still quiet for the beacon */
    CHECK(csl_quiet_for(15000u));
    CHECK(!csl_busy());
    now_us += 1; /* 14.999 ms: a beacon now could shorten it */
    CHECK(!csl_quiet_for(15000u));
    CHECK(!csl_busy());
    now_us += 5000u; /* under 10 ms: the bus is reserved as well */
    CHECK(!csl_quiet_for(15000u));
    CHECK(csl_busy());

    /* Listening, then sending the train: never quiet. */
    unsigned before = listens;
    CHECK_EQ(csl_step(), 9999u); /* still waiting for the listen */
    now_us += 9999u;
    (void)csl_step();
    CHECK_EQ(listens, before + 1u);
    CHECK(!csl_quiet_for(0));
    CHECK(csl_busy());
    run_until(now_us + 200000);
    CHECK_EQ(d.heard, 2);
    CHECK(csl_quiet_for(UINT32_MAX));
    CHECK(!csl_busy());

    /* A broadcast listens within the backoff, at most 3.84 ms away. */
    next_random = 6;
    uint8_t pkt[20] = {0x7B, 0x3B, 58, 1, 134};
    CHECK(csl_enqueue_broadcast(pkt, sizeof pkt, 9, 0, CSL_KEY_NETWORK));
    CHECK_EQ(csl_step(), 6u * CSL_LBT_SLOT_US);
    CHECK(!csl_quiet_for(15000u));
    CHECK(csl_quiet_for(6u * CSL_LBT_SLOT_US));
}

/* --- Flash work and the coordinator (#22 bench finding) ------------------------
 * The radio task loop (src/radio.c radio_task_run()) as far as flash work
 * goes: on every pass the hold is set if work is due, csl_step() runs,
 * then the work runs if the gate allows it. The work takes work_us with no
 * csl_step(), as a sector erase does on the bridge (frames are still
 * received during it, but nothing in the coordinator moves). frame_on_idle:
 * the worst case for the hold, a frame arriving (which blocks flash work)
 * on the first pass after every transaction. The loop sleeps at most
 * 100 ms, and 1 ms while a frame arrives. */
struct loop_sim {
    bool (*gate)(void); /* the moment for flash work */
    bool (*due)(void);  /* flash work wanted */
    void (*done)(void); /* after one piece of work */
    void (*refill)(void);
    void (*after_step)(void); /* right after csl_step(): a lower-priority task gets the CPU */
    bool     use_hold, frame_on_idle;
    uint32_t work_us;
    uint64_t first_work_us; /* when the first piece started (0: never) */
    unsigned works, at_limit;
};

static void run_loop(struct loop_sim *s, uint64_t until_us) {
    bool was_idle = csl_idle();
    for (unsigned guard = 0; guard < 4000000 && now_us < until_us; guard++) {
        if (ack.pending && now_us >= ack.end_us) {
            ack.pending = false;
            (void)csl_on_frame(ack.frame, MAC_ACK_LEN, ack.crc_ok, ack.end_us);
        }
        if (s->refill) {
            s->refill();
        }
        csl_hold(s->use_hold && s->due());
        uint32_t wait = csl_step();
        if (s->after_step) {
            s->after_step();
        }
        s->at_limit += csl_counter() >= csl_counter_limit();
        bool idle  = csl_idle();
        bool frame = s->frame_on_idle && idle && !was_idle;
        was_idle   = idle;
        if (s->due() && !frame && s->gate()) {
            if (s->works++ == 0) {
                s->first_work_us = now_us;
            }
            now_us += s->work_us;
            s->done();
            csl_hold(s->use_hold && s->due());
            continue;
        }
        if (wait == 0) {
            continue;
        }
        uint64_t next = now_us + (wait < 100000u ? wait : 100000u);
        if (frame && next > now_us + 1000u) {
            next = now_us + 1000u;
        }
        if (ack.pending && ack.end_us < next) {
            next = ack.end_us;
        }
        now_us = next < until_us ? next : until_us;
    }
}

static bool gate_old(void) { /* before the fix: quiet() in src/radio.c */
    return !csl_busy();
}

static bool erase_pending;
static bool erase_due(void) {
    return erase_pending;
}
static void erase_done(void) {
    erase_pending = false;
}

/* The first Data frame sent since `from`, and the wake-up frames before it. */
static const struct sent *data_after(unsigned from, unsigned *wakes) {
    *wakes = 0;
    for (unsigned i = from; i < n_sent; i++) {
        enum mac_class c = mac_classify(sent[i].frame, sent[i].len);
        if (c == MAC_WAKEUP) {
            (*wakes)++;
        } else if (c == MAC_DATA || c == MAC_DATA_PENDING) {
            return &sent[i];
        }
    }
    return NULL;
}

/* A job waits in ST_WAIT for an anchored device's sample, its listen 80 ms
 * away (csl_busy() false), when a 150 ms sector erase becomes due (the
 * bench's longest cycle was 151 ms). */
static struct device *setup_waiting_job(struct device *d) {
    reset();
    add_device(d, 0xA0, 300000, 0);
    CHECK(csl_enqueue_data(d->eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    run_until(now_us + 1500000); /* bootstrap; the ACK gives an anchor */
    CHECK(csl_anchor_known(d->eui));
    CHECK(csl_enqueue_data(d->eui, body, sizeof body, 2, 0, CSL_KEY_NETWORK));
    uint32_t to_listen = csl_step();
    CHECK(to_listen > 100000u);
    now_us += to_listen - 80000u;
    CHECK(!csl_busy());
    erase_pending = true;
    return d;
}

static void test_flash_waits_for_idle(void) {
    struct device   d;
    struct loop_sim s = {.due = erase_due, .done = erase_done, .work_us = 150000u};

    /* The old gate: the erase runs in ST_WAIT and the short train starts
     * about 70 ms late, as on the bench (4 wake-up frames, no ACK). */
    setup_waiting_job(&d);
    unsigned first = n_sent, r0 = n_reports, wakes = 0;
    s.gate     = gate_old;
    s.use_hold = false;
    run_loop(&s, now_us + 3000000);
    CHECK_EQ(s.works, 1);
    CHECK(data_after(first, &wakes) != NULL);
    CHECK(wakes < 10);
    CHECK(n_reports > r0);
    CHECK(reports[r0].outcome != CSL_DELIVERED);
    CHECK_EQ(reports[r0].attempt, 1);

    /* The fix: the erase waits for csl_idle(), after the transaction; the
     * train is whole and delivered at the first attempt. */
    setup_waiting_job(&d);
    first = n_sent;
    r0    = n_reports;
    CHECK(!csl_idle());
    s = (struct loop_sim){.gate     = csl_idle,
                          .due      = erase_due,
                          .done     = erase_done,
                          .use_hold = true,
                          .work_us  = 150000u};
    run_loop(&s, now_us + 3000000);
    CHECK_EQ(s.works, 1);
    const struct sent *data = data_after(first, &wakes);
    CHECK(data != NULL);
    CHECK(wakes >= 19 && wakes <= 23);
    CHECK_EQ(n_reports, r0 + 1u);
    CHECK_EQ(reports[r0].outcome, CSL_DELIVERED);
    CHECK_EQ(reports[r0].attempt, 1);
    CHECK(reports[r0].anchored);
    CHECK(data && s.first_work_us > data->end_us);
    CHECK_EQ(d.heard, 2);

    /* The longest wait for idle: a whole bootstrap train (about 1 s) and
     * its ACK window; nothing in the coordinator is cut short. */
    reset();
    add_device(&d, 0xA0, 300000, 0);
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 3, 0, CSL_KEY_NETWORK));
    CHECK(csl_step() < CSL_LBT_MAX_US);
    uint64_t t0   = now_us;
    erase_pending = true;
    s             = (struct loop_sim){.gate     = csl_idle,
                                      .due      = erase_due,
                                      .done     = erase_done,
                                      .use_hold = true,
                                      .work_us  = 240000u};
    run_loop(&s, now_us + 3000000);
    CHECK_EQ(n_reports, 1);
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK(count_class(MAC_WAKEUP, 0) > 240);
    CHECK(s.first_work_us - t0 < CSL_LBT_MAX_US + CSL_BOOTSTRAP_US + CSL_ACK_WINDOW_US);
}

/* csl_hold(): no new transaction while on; the one in flight runs to its
 * end as usual; idle stays idle; beacons still get their seq and counter. */
static void test_hold(void) {
    reset();
    struct device d;
    add_device(&d, 0xA0, 300000, 0);
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 1, 0, CSL_KEY_NETWORK));
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 2, 0, CSL_KEY_NETWORK));
    csl_hold(true);
    CHECK_EQ(csl_step(), UINT32_MAX);
    CHECK(csl_idle());
    CHECK(csl_quiet_for(UINT32_MAX));
    run_until(now_us + 3000000);
    CHECK_EQ(n_sent, 0);
    CHECK_EQ(csl_counter(), 1000);
    CHECK(csl_idle());
    uint8_t  seq = 0;
    uint32_t ctr = 0;
    CHECK(csl_take_seq_counter(&seq, &ctr)); /* the beacon is not held */
    CHECK_EQ(ctr, 1000);

    /* Released: job 1 starts; held again while it waits: it still goes out
     * whole and is delivered, and job 2 waits. */
    csl_hold(false);
    CHECK(csl_step() < CSL_LBT_MAX_US);
    CHECK(!csl_idle());
    csl_hold(true);
    run_until(now_us + 3000000);
    CHECK_EQ(n_reports, 1);
    CHECK_EQ(reports[0].outcome, CSL_DELIVERED);
    CHECK(count_class(MAC_WAKEUP, 0) > 240);
    CHECK(csl_idle());
    CHECK_EQ(d.heard, 1);
    unsigned before = n_sent;
    run_until(now_us + 2000000);
    CHECK_EQ(n_sent, before);

    csl_hold(false);
    run_until(now_us + 1500000);
    CHECK_EQ(n_reports, 2);
    CHECK_EQ(reports[1].outcome, CSL_DELIVERED);
    CHECK_EQ(d.heard, 2);

    /* csl_init() (a new key) clears it. */
    csl_hold(true);
    reset();
    CHECK(csl_enqueue_data(d.eui, body, sizeof body, 3, 0, CSL_KEY_NETWORK));
    CHECK(csl_step() < CSL_LBT_MAX_US);
}

/* The counter reservation under continuous downlink traffic (#37, #22):
 * two devices with a queue that never empties, a frame arriving on the
 * first pass after every transaction, and the reservation's write taking
 * a whole sector erase (240 ms). Scaled down: extend once fewer than
 * CTR_LOW counters remain, CTR_AHEAD ahead (store.h: 2048, 4096). The
 * hold makes the coordinator idle after the transaction in flight, so the
 * write comes before the counter reaches the limit, and no train is late. */
#define CTR_LOW   2u
#define CTR_AHEAD 6u
static struct device *traffic_dev[2];
static unsigned       traffic_n;

static bool ctr_due(void) {
    return csl_counter_limit() - csl_counter() < CTR_LOW;
}
static void ctr_extend(void) {
    csl_set_counter_limit(csl_counter() + CTR_AHEAD);
}
static void traffic_refill(void) {
    while (csl_free_jobs() > CSL_JOB_POOL - 4u) {
        struct device *d = traffic_dev[traffic_n % 2u];
        CHECK(csl_enqueue_data(d->eui, body, sizeof body, (uint16_t)traffic_n, 0, CSL_KEY_NETWORK));
        traffic_n++;
    }
}

static void test_counter_reservation_under_traffic(void) {
    struct device a, b;
    for (int hold = 1; hold >= 0; hold--) {
        reset();
        struct csl_config cfg = {.pan           = TEST_PAN,
                                 .key           = {&key, NULL},
                                 .counter       = 1000,
                                 .counter_limit = 1000 + CTR_AHEAD,
                                 .seq           = 1};
        memcpy(cfg.own, own, 8);
        csl_init(&cfg);
        traffic_dev[0]    = add_device(&a, 0xA0, 100000, 0);
        traffic_dev[1]    = add_device(&b, 0xB0, 600000, 0);
        traffic_n         = 0;
        struct loop_sim s = {.gate          = csl_idle,
                             .due           = ctr_due,
                             .done          = ctr_extend,
                             .refill        = traffic_refill,
                             .use_hold      = hold != 0,
                             .frame_on_idle = true,
                             .work_us       = 240000u};
        run_loop(&s, now_us + 30000000);
        if (hold) {
            printf("test_csl: reservation under traffic: %u writes, %u reports, counter %lu\n",
                   s.works,
                   n_reports,
                   (unsigned long)csl_counter());
            CHECK_EQ(s.at_limit, 0); /* never at the limit */
            CHECK(s.works >= 8);
            CHECK(n_reports >= 40);
            for (unsigned i = 0; i < n_reports; i++) {
                CHECK_EQ(reports[i].outcome, CSL_DELIVERED);
                CHECK_EQ(reports[i].attempt, 1);
            }
        } else {
            /* Without the hold the next job starts on the pass after each
             * transaction, and the write waits until the limit stops the
             * coordinator. */
            CHECK(s.at_limit > 0);
        }
    }
}

/* The update's flash windows (#24, src/fwupd.h): the update task posts its
 * next operation as soon as the last one is done, but it runs at a lower
 * priority than the radio task, so the post lands after the radio task's
 * next csl_step() (in its xTaskNotifyWait()). Every operation is taken as
 * the longest, a sector erase with its blank check (250 ms); two devices
 * with a queue that never empties, and a frame arriving on the first pass
 * after every transaction. The coordinator gets one transaction between
 * two operations: every downlink is delivered at its first attempt with a
 * whole train, and the update still moves on. Without the hold, operations
 * would wait for gaps in a queue that never empties. */
static bool upd_posted;
static bool upd_due(void) {
    return upd_posted;
}
static void upd_done(void) {
    upd_posted = false;
}
static void upd_repost(void) {
    upd_posted = true;
}

static void test_update_windows_under_traffic(void) {
    struct device a, b;
    for (int hold = 1; hold >= 0; hold--) {
        reset();
        traffic_dev[0]    = add_device(&a, 0xA0, 100000, 0);
        traffic_dev[1]    = add_device(&b, 0xB0, 600000, 0);
        traffic_n         = 0;
        upd_posted        = true;
        struct loop_sim s = {.gate          = csl_idle,
                             .due           = upd_due,
                             .done          = upd_done,
                             .refill        = traffic_refill,
                             .after_step    = upd_repost,
                             .use_hold      = hold != 0,
                             .frame_on_idle = true,
                             .work_us       = 250000u};
        unsigned first = n_sent;
        run_loop(&s, now_us + 60000000);
        if (hold) {
            unsigned short_trains = 0, wakes = 0;
            for (unsigned i = first; i < n_sent; i++) {
                enum mac_class c = mac_classify(sent[i].frame, sent[i].len);
                if (c == MAC_WAKEUP) {
                    wakes++;
                } else if (c == MAC_DATA || c == MAC_DATA_PENDING) {
                    short_trains += wakes >= 19 && wakes <= 23;
                    wakes = 0;
                }
            }
            printf("test_csl: update windows under traffic: %u operations of 250 ms, %u downlinks "
                   "delivered, %u with a short train of 19-23 wake-up frames\n",
                   s.works,
                   n_reports,
                   short_trains);
            CHECK(s.works >= 50);
            CHECK(n_reports >= 50);
            for (unsigned i = 0; i < n_reports; i++) {
                CHECK_EQ(reports[i].outcome, CSL_DELIVERED);
                CHECK_EQ(reports[i].attempt, 1);
            }
            CHECK(short_trains + 2u >= n_reports); /* the bootstrap trains are long */
            CHECK_EQ(s.at_limit, 0);
        } else {
            /* The gate alone: an operation starts only in the gaps the
             * queue leaves, and with a frame on the first idle pass there
             * are none. */
            CHECK(s.works < 5);
        }
    }
}

int main(void) {
    ccm_set_key(&key, v_key.p);
    test_bootstrap_then_anchored();
    test_frame_pending();
    test_no_ack_retries();
    test_anchor_miss_falls_back();
    test_bad_ack();
    test_listen_busy();
    test_tx_failure_and_no_key();
    test_broadcast();
    test_limits();
    test_two_devices();
    test_drift();
    test_pairing_flag();
    test_counter_limit();
    test_ra_pair();
    test_take_seq_counter();
    test_quiet_for();
    test_flash_waits_for_idle();
    test_hold();
    test_counter_reservation_under_traffic();
    test_update_windows_under_traffic();
    return TEST_REPORT("test_csl");
}
