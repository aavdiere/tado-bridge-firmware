/* Radio task, the coordinator's radio (src/csl.h "Port"): sending a frame
 * through the driver, listen before transmit, and the transaction reports.
 * Part of the radio task (src/radio.c); the state it shares,
 * src/radio_internal.h. */
#include "radio_internal.h"

#include "cc110l.h"
#include "csl.h"
#include "link.h"
#include "log.h"
#include "timebase.h"
#include "txgate.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/stm32/gpio.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

/* --- The coordinator's radio (src/csl.h "Port") ------------------------------- */
uint64_t csl_port_now(void) {
    return now64();
}

/* Backoff source: xorshift32, seeded once from the DRBG when it is ready
 * (rng_try_bytes never waits). Until then, and mixed in afterwards, the
 * microsecond clock: the backoff only has to differ between attempts. */
uint32_t lbt_state;
bool     lbt_seeded;

uint32_t csl_port_random(void) {
    uint32_t x = lbt_state ^ timebase_us();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lbt_state = x ? x : 1u;
    return x;
}

#define TX_TIMEOUT_US 100000u /* freedo TX_TMO_MS; the longest frame is 22 ms on the air */

/* A transmission failed part way: stop it, empty both FIFOs, receive again. */
static bool tx_failed(void) {
    COUNT(tx_fail);
    cc110l_tx_abort();
    rx_flush_and_arm();
    return false;
}

/* Sleep until a GDO edge or the next tick. Edges of our own frame are not
 * the receiver's business; anything else is kept for the task loop. */
static void tx_wait_tick(void) {
    uint32_t bits = 0;
    (void)xTaskNotifyWait(0, UINT32_MAX, &bits, 1);
    pending_bits |= bits & ~(NOTE_GDO0 | NOTE_SYNC | NOTE_CRCOK);
    watchdog_checkin(WDG_RADIO);
}

/* One frame through the driver: cc110l_tx_start(), or cc110l_tx_answer()
 * for a frame that answers a received one. mark: the pairing LED goes on
 * right after the STX strobe (#19, the beacon's scope marker), and *stx_us
 * (if not NULL) is the time just after it. */
bool send_frame(const uint8_t *frame,
                size_t         len,
                const uint8_t *answered,
                unsigned       flags,
                bool           mark,
                uint32_t      *stx_us) {
    if (!gdo_irq_on) {
        /* The storm guard tripped or a re-init failed: the chip may have
         * reset and is not known to hold our configuration. Nothing goes
         * out until the bring-up has passed again. */
        COUNT(tx_refused);
        return false;
    }
    /* An Enhanced ACK of ours may still be on the air (it was sent before
     * the coordinator stopped the receiver's ACKs): at most its airtime and
     * margin, 8.5 ms. The driver refuses while one is. */
    for (unsigned i = 0; i < 16 && cc110l_tx_expected(); i++) {
        tx_wait_tick();
    }
    enum cc110l_tx_result r = answered ? cc110l_tx_answer(frame, len, answered, flags)
                                       : cc110l_tx_start(frame, len, flags);
    if (r == CC_TX_OK) {
        if (mark) {
            board_led(LED_PAIRING, true);
        }
        if (stx_us) {
            *stx_us = timebase_us();
        }
    }
    if (r == CC_TX_DISABLED || r == CC_TX_REFUSED || r == CC_TX_BUSY || r == CC_TX_NO_LOCK
        || r == CC_TX_DUTY || r == CC_TX_NO_PA) {
        /* The chip was not touched: still receiving. A frame refused for
         * airtime fails its transaction; the coordinator gives up on the
         * datagram after CSL_ATTEMPTS (src/csl.h), it does not wait. */
        COUNT(tx_refused);
        if (r == CC_TX_NO_LOCK) {
            COUNT(tx_no_lock);
        } else if (r == CC_TX_DUTY) {
            COUNT(tx_duty);
        }
        return false;
    }
    /* cc110l_tx_start() emptied the RX FIFO. */
    radio_rx.assembling = false;
    radio_rx.in_packet  = false;
    fe_sample.valid     = false;
    if (r != CC_TX_OK) {
        return tx_failed();
    }
    /* Feed a frame longer than the FIFO, then wait for the packet to leave
     * the chip: out of TX and TX_END with an empty TX FIFO. MCSM1 returns
     * the chip to RX, so it never shows IDLE (freedo .ino:608-625). */
    uint32_t t0 = timebase_us();
    for (;;) {
        size_t left = 0;
        if (!cc110l_tx_feed(&left)) {
            return tx_failed();
        }
        if (left == 0) {
            uint8_t m = 0, tb = 0xFF;
            if (cc110l_read_status_stable(CC_MARCSTATE, &m)
                && cc110l_read_status_stable(CC_TXBYTES, &tb)) {
                m &= 0x1F;
                if (m == CC_MARC_TXFIFO_UNF || m == CC_MARC_IDLE) {
                    return tx_failed(); /* IDLE: the chip lost its configuration */
                }
                if (m != CC_MARC_TX && m != CC_MARC_TX_END && (tb & 0x7Fu) == 0) {
                    break;
                }
            }
        }
        if ((uint32_t)(timebase_us() - t0) > TX_TIMEOUT_US) {
            return tx_failed();
        }
        tx_wait_tick();
    }
    /* The frame is out and the chip is on its way back to RX. Without this
     * the next frame of a train would wait out the margin of this one, and
     * its rendezvous count would be 2 ms old when it goes out. */
    cc110l_tx_done();
    COUNT(tx_frames);
    if (gpio_get(GPIOA, BOARD_GDO0_PIN)) {
        /* A sync word already: its edge went to tx_wait_tick(), which
         * drops the edges of our own frame. Pump every tick as for any
         * other packet. */
        radio_rx.in_packet = true;
        radio_rx.sync_us   = timebase_us();
    }
    return true;
}

bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    return send_frame(frame, len, NULL, flags, false, NULL);
}

/* Listen before transmit: "a packet on the air" is a sync word seen at any
 * time during the listen (GDO0 edges), a frame still being read, a frame
 * of our own (an Enhanced ACK) on the air, or energy in the channel: the
 * RSSI at or above LBT_RSSI_BUSY_DBM at the start of the listen, at its
 * end, or at any loop pass in between (about one, 2-3 ms in; the task
 * waits for the rest of the 3.968 ms). Sync words show tado frames; the
 * RSSI also shows other users of the band, which the sync words miss
 * (test-bench.md: LoRaWAN-like bursts on neighbouring channels). In RX the
 * RSSI register updates continuously until a sync word freezes it
 * (cc110l.txt:2113-2115). The chip's frequency registers are read back
 * here as well: if they are not what was written, the transaction ends as
 * "busy" before its first frame, which the driver would refuse anyway (it
 * reads them back again for every frame). A failed RSSI read counts as busy;
 * threshold and statistics: below, and the minute log (src/radio.c). */
static uint32_t lbt_edges;
static bool     lbt_busy, lbt_rssi_hit;

static bool on_air_now(void) {
    return packet_arriving() || cc110l_tx_expected();
}

void lbt_sample_rssi(void) {
    uint8_t raw;
    if (!cc110l_read_status_stable(CC_RSSI, &raw)) {
        COUNT(spi_err);
        lbt_rssi_hit = true;
        return;
    }
    int x2 = rssi_x2(raw);
    if (!lbt_rssi_seen || x2 > lbt_rssi_max_x2) {
        lbt_rssi_max_x2 = x2;
        lbt_rssi_seen   = true;
    }
    if (x2 >= 2 * LBT_RSSI_BUSY_DBM) {
        lbt_rssi_hit = true;
    }
}

void csl_port_listen_begin(void) {
    lbt_edges     = gdo0_edges;
    lbt_rssi_hit  = false;
    lbt_listening = true;
    lbt_busy      = on_air_now() || !cc110l_tx_verify_channel();
    lbt_sample_rssi();
}

bool csl_port_listen_busy(void) {
    lbt_sample_rssi();
    lbt_listening = false;
    if (lbt_rssi_hit) {
        COUNT(lbt_rssi_busy);
    }
    return lbt_busy || lbt_rssi_hit || gdo0_edges != lbt_edges || on_air_now();
}

/* Listen before transmit by RSSI (see csl_port_listen_begin()). Threshold
 * [I]: -85 dBm, about 19 dB above the sensitivity at 868 MHz and 38.4
 * kBaud, -104 dBm (cc110l.txt:600-603; 50 kBaud here), and well below the
 * -41 to -72 dBm of the tado frames seen on the bench (radio-runtime.md,
 * P2). The highest RSSI of the minute's listens is logged, to set it from
 * bench data. LBT_RSSI_BUSY_DBM: src/radio_internal.h. */
bool lbt_listening;
bool lbt_rssi_seen; /* lbt_rssi_max_x2 holds a value this minute */
int  lbt_rssi_max_x2;

static const char *const outcome_name[] = {
    [CSL_DELIVERED] = "delivered",
    [CSL_NO_ACK]    = "no ack",
    [CSL_BAD_ACK]   = "bad ack",
    [CSL_TX_FAILED] = "tx failed",
    [CSL_BUSY]      = "channel busy",
    [CSL_NO_KEY]    = "no key",
    [CSL_ABANDONED] = "abandoned",
};

/* After the fact: the transaction is over when this runs. No address in the
 * log, only the MAC seq and our own datagram id. */
void csl_port_report(const struct csl_report *r) {
    link_on_report(r);
    if (r->outcome == CSL_BUSY && !r->final) {
        return; /* a busy listen is retried 20 ms later; the outcome gets its line */
    }
    LOG("tx: datagram %u seq %02x attempt %u: %s%s, %u wake frames, %s train%s",
        (unsigned)r->id,
        r->seq,
        (unsigned)r->attempt,
        outcome_name[r->outcome],
        r->final ? "" : ", will retry",
        (unsigned)r->wake_frames,
        r->anchored ? "short" : "full",
        r->outcome == CSL_TX_FAILED || r->outcome == CSL_NO_KEY
            ? cc110l_tx_verdict() == TXGATE_OK ? "" : " (transmit gate refused)"
            : "");
}
