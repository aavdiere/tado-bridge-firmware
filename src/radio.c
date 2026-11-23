/* Radio task: receive, Enhanced ACKs, the CSL coordinator and the relay
 * queues (#17), plus the flash ID check on the shared SPI1 bus (#10). The
 * receive flow follows docs/radio-runtime.md (freedo's working receive
 * path, freedo/firmware/cc1101_bridge/cc1101_bridge.ino:772-816); the
 * protocol logic is in the hardware-independent modules src/mac.c,
 * src/lowpan.c, src/csl.c, src/relay.c and src/link.c, and everything that
 * can key the transmitter in src/cc110l.c behind the transmit gate.
 *
 * Interrupts, both at configMAX_SYSCALL_INTERRUPT_PRIORITY:
 *   GDO0 (PA0, EXTI0, both edges): IOCFG0 = 0x06 asserts on sync word and
 *     deasserts at the end of the packet, on a length-filtered packet, or when
 *     the RX FIFO overflows (cc110l.txt:2916-2918); in TX it asserts when the
 *     sync word has been sent and deasserts at the end of the packet. The
 *     rising edge wakes the task to sample FREQEST while the packet is still
 *     arriving (#11, see sample_in_packet()) and drops a prepared ACK that
 *     was not sent (it belonged to an earlier packet). The falling edge is
 *     timestamped: the end of the frame, for the CSL anchors.
 *   GDO2 (PA1, EXTI1, rising edge): IOCFG2 = 0x07 asserts when a packet has
 *     been received with CRC OK (cc110l.txt:2919). If an Enhanced ACK is
 *     prepared, the interrupt sends it (cc110l_ack_fire_from_isr()).
 * No interrupt announces a filling RX FIFO: from the sync edge to the end of the frame the task
 * pumps the FIFO every tick (1 ms, 6 bytes of a 64-byte FIFO), and every 100 ms otherwise.
 *
 * ENHANCED ACK (docs/radio.md "ACK prebuild"; target: starts within 1 ms of
 * the end of the Data frame, also while the net task holds the CPU for a
 * P-256 step). Two contexts:
 *   1. This task, the highest-priority one, while the Data frame is still
 *      arriving: once its first 16 bytes are out of the FIFO, ack_consider()
 *      builds the ACK, has the chip's frequency registers read back, and
 *      hands the ACK to the driver, which checks it against the transmit
 *      gate. The hand-over is tied to this frame: it happens with the GDO
 *      interrupts masked and only if no GDO0 edge has come since the frame
 *      was seen arriving. The shortest Data frame (one datagram byte, 31
 *      bytes) leaves 17 bytes, 2.7 ms, for this after its header [I].
 *   2. The GDO2 interrupt, at the frame's CRC-OK edge: three strobes and 30
 *      FIFO bytes, about 70 us to the STX strobe, then 75 us IDLE to TX in
 *      the chip [I]. It preempts every task and is masked only by critical
 *      sections (one SPI1 access at most, about 0.1 ms).
 * Nothing between the sync edge and the ACK logs: LOG() takes a mutex. The
 * frame's own log line is written when the frame is complete, after the
 * interrupt has sent the ACK; periodic log lines wait while a packet is
 * arriving.
 *
 * TX-state watch. A single bit error on MOSI can turn any header the guard in
 * cc110l.c approved into a transmit strobe at the chip (SRX 0x34 -> STX 0x35,
 * MARCSTATE read 0xF5 -> 0xB5 = STX, a CSn bounce mid-burst that makes the
 * TEST1 data byte 0x35 a header). The chip would then send preamble until
 * stopped. Every status byte the chip returns is checked for STATE TX,
 * FSTXON or TXFIFO_UNDERFLOW outside the airtime of a frame we started
 * (cc110l.c), and tx_watch() takes one fresh status byte and acts on that
 * flag. It runs after every radio step of the bring-up, on every bring-up
 * failure, every 100 ms of the retry wait, and at the end of every pass of
 * the task loop, so a TX state that is not ours is stopped within
 * microseconds of the next radio access and at most one loop pass (<= 100 ms
 * when idle, with no other radio access in between) after the access that
 * caused it.
 *
 * PAIRING (#19, src/pair.h). The pair button (held 1 s, src/app.c), or the
 * server's POST d/I/<serial>/pair (#40, src/pair_remote.h, from the net
 * task), asks this task to open a window for the device named in the build
 * (PAIR_TARGET_EUI64 in config/local.cmake): at a quiet moment it creates
 * the network first if there has never been one, then opens the window
 * (10 minutes from the button, the server's duration up to 10 minutes).
 * The Enhanced Beacon that answers the target's Beacon Request is built
 * while the request is taken (link_on_frame()) and sent from frame_done(),
 * before anything logs, PAIR_BEACON_TX_AT_US after the request's end edge
 * (see beacon_answer()). The pairing LED (PB8, owner spec 2026-11-17)
 * flashes while a window is open and waits for the target, is lit while an
 * exchange with the target goes on (from a beacon sent to it until its
 * 2.04, a failure or no progress, src/pair.h), and is off once the window
 * has closed. It is also the scope marker of the beacon's STX strobe: off
 * while the beacon waits for its moment, on right after the strobe.
 *
 * FILES. The radio task's parts, which share state and helpers through
 * src/radio_internal.h:
 *   - src/radio.c: the task loop and everything not listed below.
 *   - src/radio_bringup.c: the register table, bring-up, calibration, the
 *     health check and the TX-state watch.
 *   - src/radio_csl.c: the coordinator's radio (src/csl.h "Port"):
 *     sending, listen before transmit, transaction reports.
 *   - src/radio_flash.c: flash work and update operations: the flash check,
 *     the factory reset, the update's operations (#24), and the receiver
 *     served from inside flash work (#22).
 *   - src/radio_pair.c: pairing: the beacon answer and the pairing window.
 *   - src/radio_rx.c: receive: the GDO interrupts, the RX FIFO pump, the
 *     Enhanced ACK's task side, complete frames, the in-packet FREQEST
 *     sample, the wake-up train summary, and the log formatting helpers. */
#include "radio.h"

#include "radio_internal.h"

#include "airtime.h"
#include "cc110l.h"
#include "csl.h"
#include "freqoff.h"
#include "handoff.h"
#include "identity.h"
#include "link.h"
#include "log.h"
#include "mac.h"
#include "mx25r.h"
#include "network.h"
#include "pair.h"
#include "pair_led.h"
#include "pair_remote.h"
#include "rng.h"
#include "spi1.h"
#include "timebase.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

TaskHandle_t radio_self;

void radio_request_flash_check(void) {
    if (radio_self) {
        xTaskNotify(radio_self, NOTE_FLASH, eSetBits);
    }
}

void radio_notify_link(void) {
    if (radio_self) {
        xTaskNotify(radio_self, NOTE_LINK, eSetBits);
    }
}

void radio_request_pairing(void) {
    if (radio_self) {
        xTaskNotify(radio_self, NOTE_PAIR, eSetBits);
    }
}

void radio_request_factory_reset(void) {
    if (radio_self) {
        xTaskNotify(radio_self, NOTE_RESET, eSetBits);
    }
}

/* Microseconds since boot that do not wrap: TIM2's 32 bits extended. Radio
 * task only, called on every loop pass, far more often than the 71 minutes
 * of one wrap. */
uint64_t now64(void) {
    static uint32_t last, high;
    uint32_t        t = timebase_us();
    if (t < last) {
        high++;
    }
    last = t;
    return (uint64_t)high << 32 | t;
}

/* --- Counters (struct counters, COUNT(): src/radio_internal.h) -------------- */
struct counters radio_minute, radio_total;

static void log_minute(void) {
    LOG("radio: 60 s: frames %lu (crc ok %lu, bad %lu, wake %lu), overflow %lu, bad len %lu, stale "
        "%lu, "
        "rearm %lu, reinit %lu, spi err %lu, status glitch %lu, irq storm %lu",
        (unsigned long)radio_minute.frames,
        (unsigned long)radio_minute.crc_ok,
        (unsigned long)radio_minute.crc_bad,
        (unsigned long)radio_minute.wake,
        (unsigned long)radio_minute.overflow,
        (unsigned long)radio_minute.bad_len,
        (unsigned long)radio_minute.stale,
        (unsigned long)radio_minute.rearm,
        (unsigned long)radio_minute.reinit,
        (unsigned long)radio_minute.spi_err,
        (unsigned long)radio_minute.status_glitch,
        (unsigned long)radio_minute.storm);
    LOG("flash: 60 s: JEDEC ID ok %lu, mismatch %lu (%lu during a frame); total frames %lu, "
        "flash mismatches %lu",
        (unsigned long)radio_minute.flash_ok,
        (unsigned long)radio_minute.flash_bad,
        (unsigned long)radio_minute.flash_in_frame,
        (unsigned long)radio_total.frames,
        (unsigned long)radio_total.flash_bad);
    const struct mx25r_stats *fs = mx25r_stats();
    LOG("flash: 60 s: radio served %lu times during flash work, frames %lu, acks %lu; totals: "
        "cycles %lu (longest %lu us), held %lu (longest %lu us, gave up %lu)",
        (unsigned long)radio_minute.flash_serve,
        (unsigned long)radio_minute.flash_frames,
        (unsigned long)radio_minute.flash_acks,
        (unsigned long)fs->cycles,
        (unsigned long)fs->cycle_max,
        (unsigned long)fs->held,
        (unsigned long)fs->held_max,
        (unsigned long)fs->gave_up);
    struct freqoff_report fr;
    freqoff_report(&radio_fo, &fr);
    LOG("radio: 60 s: freqest in packet %lu, none %lu (stale sample dropped %lu), sync rssi "
        "differs %lu; our network %lu frames, mean %+ld Hz (%d..%d)",
        (unsigned long)radio_minute.fe_pkt,
        (unsigned long)radio_minute.fe_none,
        (unsigned long)radio_minute.fe_stale,
        (unsigned long)radio_minute.fe_rssi_diff,
        (unsigned long)fr.minute_n,
        (long)fr.minute_mean_hz,
        fr.minute_min,
        fr.minute_max);
    if (fr.verdict == FREQOFF_WARN || fr.verdict == FREQOFF_BACK) {
        char k[12];
        /* kHz in tenths, rounded half away from zero; |mean| <= 203125 Hz. */
        int32_t x10 = (fr.window_mean_hz + (fr.window_mean_hz < 0 ? -50 : 50)) / 100;
        LOG("radio: frequency offset %s: devices average %s%s kHz from us over %lu frames "
            "(limit %u.%u kHz)%s",
            fr.verdict == FREQOFF_WARN ? "warning" : "back within limit",
            x10 > 0 ? "+" : "",
            tenths(k, sizeof k, x10),
            (unsigned long)fr.window_n,
            FREQOFF_LIMIT_HZ / 1000u,
            FREQOFF_LIMIT_HZ % 1000u / 100u,
            fr.verdict == FREQOFF_WARN ? "; check the crystal (#11)" : "");
    }
    const struct link_stats *ls = link_stats();
    /* Two lines: one log line holds 191 characters (src/log.c). */
    LOG("link: 60 s: acks prepared %lu sent %lu too late %lu, tx frames %lu fail %lu refused %lu "
        "(no pll lock %lu)",
        (unsigned long)radio_minute.ack_prepared,
        (unsigned long)radio_minute.ack_sent,
        (unsigned long)radio_minute.ack_late,
        (unsigned long)radio_minute.tx_frames,
        (unsigned long)radio_minute.tx_fail,
        (unsigned long)radio_minute.tx_refused,
        (unsigned long)radio_minute.tx_no_lock);
    LOG("link: airtime last hour %lu ms of %lu ms; 60 s: refused for airtime %lu frames, %lu acks; "
        "acks failed %lu (re-armed %lu)",
        (unsigned long)(cc110l_airtime_used_us() / 1000u),
        (unsigned long)(AIRTIME_BUDGET_US / 1000u),
        (unsigned long)radio_minute.tx_duty,
        (unsigned long)radio_minute.ack_duty,
        (unsigned long)acks_failed,
        (unsigned long)radio_minute.ack_rearm);
    {
        char r[12];
        LOG("link: 60 s: listens busy by rssi %lu (threshold %d dBm), highest listen rssi %s%s",
            (unsigned long)radio_minute.lbt_rssi_busy,
            LBT_RSSI_BUSY_DBM,
            lbt_rssi_seen ? half_db(r, sizeof r, lbt_rssi_max_x2) : "none",
            lbt_rssi_seen ? " dBm" : "");
        lbt_rssi_seen = false;
    }
    LOG("link: totals: to server %lu, dup %lu, mic fail %lu, dropped %lu, downlinks %lu refused "
        "%lu, RA %lu",
        (unsigned long)ls->rx[LINK_RX_FORWARDED],
        (unsigned long)ls->rx[LINK_RX_DUPLICATE],
        (unsigned long)ls->rx[LINK_RX_MIC_FAIL],
        (unsigned long)ls->rx[LINK_RX_DROPPED],
        (unsigned long)ls->down_queued,
        (unsigned long)(ls->down_refused + ls->down_full),
        (unsigned long)ls->ra_queued);
    const struct pair_stats *ps = pair_stats();
    if (ps->windows) {
        LOG("pair: 60 s: beacons sent %lu, not sent: end not live %lu, busy %lu, late %lu, failed "
            "%lu; totals: requests answered %lu, from others %lu",
            (unsigned long)radio_minute.beacon_sent,
            (unsigned long)radio_minute.beacon_no_stamp,
            (unsigned long)radio_minute.beacon_busy,
            (unsigned long)radio_minute.beacon_late,
            (unsigned long)radio_minute.beacon_fail,
            (unsigned long)ps->req[PAIR_REQ_OK],
            (unsigned long)ps->req[PAIR_REQ_NOT_TARGET]);
        LOG("pair: totals: windows %lu, paired %lu, target frames %lu, RA pairs %lu, d/pair sent "
            "%lu delivered %lu failed %lu",
            (unsigned long)ps->windows,
            (unsigned long)ps->paired,
            (unsigned long)ps->joiner_frames,
            (unsigned long)ps->ra_pairs,
            (unsigned long)ps->dpair_sent,
            (unsigned long)ps->dpair_delivered,
            (unsigned long)ps->dpair_failed);
    }
    radio_minute = (struct counters){0};
}

/* Notification bits taken while a frame of ours is on the air, for the task
 * loop to act on afterwards. */
uint32_t pending_bits;

/* The loop's quiet moment: no packet arriving (its Enhanced ACK must not
 * wait behind a log line or the flash), no transaction on the air. Checked
 * again after each piece of work that may touch the flash, since frames
 * are taken in it (#22). */
static bool quiet(void) {
    return !packet_arriving() && !csl_busy();
}

/* The moment for flash work (#22 bench finding, docs/design.md "Flash work
 * and the coordinator"): no packet arriving, and no transaction set up at
 * all. quiet() is not enough: it lets a job wait in ST_WAIT with its listen
 * 10 ms or more away, csl_step() does not run during flash work, and a
 * sector erase (up to 240 ms) then pushed a short train's start 72 ms late
 * on the bench. Nothing but csl_step() sets a transaction up, so the
 * coordinator stays idle for the whole of the work. */
static bool flash_quiet(void) {
    return !packet_arriving() && csl_idle();
}

#define HEALTH_PERIOD_MS    10000u
#define STATS_PERIOD_MS     60000u
#define RETRY_PERIOD_MS     10000u
#define STORM_REINIT_MIN_MS 1000u

/* Our wire EUI-64: the identity record's EUI-64 reversed (docs/radio.md
 * "Source address tail"; #21). Without an identity the link has no own
 * address: nothing is acknowledged, answered or sent. */
static void link_start(void) {
    const struct store_id *id = identity();
    if (id) {
        uint8_t own[MAC_EUI_LEN];
        for (unsigned i = 0; i < MAC_EUI_LEN; i++) {
            own[i] = id->eui64[MAC_EUI_LEN - 1u - i];
        }
        if (memcmp(own + 5, mac_eui_suffix, 3) == 0) {
            link_init(own);
            cc110l_set_identity(own);
            LOG("link: identity set");
            return;
        }
        LOG("link: identity EUI-64 does not start 00 1b c5; no identity"); /* store_id_valid() */
    } else {
        LOG("link: no bridge identity (identity record): radio link off, nothing sent");
    }
    link_init(NULL);
}

void radio_task_run(void) {
    radio_self = xTaskGetCurrentTaskHandle();
    LOG("spi1: SCK %lu Hz, mode 0", (unsigned long)spi1_sck_hz());

    watchdog_checkin(WDG_RADIO);
    bool flash_ok = flash_boot();
    flash_ready   = flash_ok;
    /* Before the link, before the session may authenticate (#21) and before
     * the net task sets the Ethernet MAC (#39, src/net.c mac_choose()). */
    identity_boot(flash_ok);
    link_start();
    freqoff_init(&radio_fo);
    if (flash_ok) {
        network_boot(); /* key and counter reservation from flash, before the radio (#37) */
    } else {
        LOG("network: flash not ready: network record not read: no key, nothing sent");
    }
    gdo_irq_setup();

    while (!radio_configure()) {
        LOG("radio: bring-up failed; retrying in %u s", RETRY_PERIOD_MS / 1000);
        for (unsigned ms = 0; ms < RETRY_PERIOD_MS; ms += 100) {
            watchdog_checkin(WDG_RADIO);
            (void)tx_watch(); /* radio_configure forced IDLE; keep watching */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    radio_running                   = true;
    TickType_t      next_health     = xTaskGetTickCount() + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
    TickType_t      next_stats      = xTaskGetTickCount() + pdMS_TO_TICKS(STATS_PERIOD_MS);
    TickType_t      last_storm      = xTaskGetTickCount() - pdMS_TO_TICKS(STORM_REINIT_MIN_MS);
    bool            flash_due       = false;
    bool            pair_due        = false;
    bool            reset_due       = false;
    bool            window_was_open = false;
    bool            was_engaged     = false;
    struct pair_led pled;
    pair_led_init(&pled, pair_stats()->paired);
    for (;;) {
        /* The coordinator first: it may send (a wake-up frame per call during
         * a train) and says how long it can wait. */
        (void)now64();
        link_poll((uint32_t)xTaskGetTickCount(), csl_port_random());
        uint32_t   hint = csl_step();
        TickType_t wait = pdMS_TO_TICKS(100);
        if (hint == 0) {
            wait = 0;
        } else if (hint < 2000u) {
            /* Too short for the 1 ms tick: spin, so that a listen or a train
             * starts on its microsecond. */
            delay_us(hint);
            wait = 0;
        } else if (hint != UINT32_MAX && hint / 1000u - 1u < wait) {
            wait = pdMS_TO_TICKS(hint / 1000u - 1u);
        }
        if (wait > 20 && link_pair_engaged((uint32_t)xTaskGetTickCount())) {
            /* The quick flash of the pairing LED (100 ms halves) needs a pass every 20 ms. */
            wait = 20;
        }
        if (wait > 1 && (radio_rx.assembling || radio_rx.in_packet)) {
            wait = 1; /* a frame is arriving: pump the FIFO every tick */
        }
        if (pending_bits) {
            wait = 0; /* taken during a frame of ours or flash work (#22) */
        }

        uint32_t bits = 0;
        /* Block with a timeout, never forever, so the watchdog check-in keeps
         * coming while the channel is quiet. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        bits |= pending_bits;
        pending_bits = 0;
        watchdog_checkin(WDG_RADIO);

        TickType_t now = xTaskGetTickCount();
        if (bits & NOTE_STORM) {
            COUNT(storm);
            fe_sample.valid = false; /* edges during a storm are not packet edges (#28) */
            /* Re-init at once, but at most once per STORM_REINIT_MIN_MS; a
             * storm that comes straight back leaves the interrupts off until
             * the next health check re-initialises. */
            if ((uint32_t)(now - last_storm) >= pdMS_TO_TICKS(STORM_REINIT_MIN_MS)) {
                last_storm = now;
                LOG("radio: GDO interrupt storm (> %u edges in %u us); interrupts off, "
                    "re-initialising",
                    STORM_MAX_EDGES,
                    STORM_WINDOW_US);
                radio_reinit();
            }
        }

        rx_service(bits);
        if (lbt_listening) {
            if (csl_busy()) {
                lbt_sample_rssi(); /* a pass inside the listen */
            } else {
                lbt_listening = false; /* the transaction ended without its verdict */
            }
        }

        if (!lbt_seeded && rng_try_bytes(&lbt_state, sizeof lbt_state)) {
            lbt_seeded = true;
        }
        /* The driver's age check runs on the 32-bit microsecond clock, which
         * wraps every 71 minutes; this one cannot be fooled by a wrap. */
        if (now64() - cal_good_at > CC_CAL_MAX_AGE_US) {
            cc110l_cal_expire();
        }

        /* Everything below can wait for a quiet moment. The flash work in
         * it takes frames (#22), so the moment is asked for again after
         * each step that may write or read the flash. Flash work waits for
         * flash_quiet(): no transaction set up at all. While work that may
         * program or erase is due (factory reset, pairing trigger from the
         * button or the server, which may create a network, counter
         * reservation, an update operation),
         * the coordinator is held: it starts nothing new, so it is idle by
         * the end of the transaction in flight, however full its queue.
         * The hold is dropped on the same pass as the work, before the
         * next csl_step(). */
        flash_due |= (bits & NOTE_FLASH) != 0;
        pair_due |= (bits & NOTE_PAIR) != 0;
        reset_due |= (bits & NOTE_RESET) != 0;
        csl_hold(reset_due || pair_due || pair_remote_pending()
                 || network_service_due((uint32_t)now) || handoff_pending());
        if (flash_due && flash_quiet()) {
            flash_due = false;
            flash_check();
        }
        if (reset_due && flash_quiet()) {
            factory_reset(); /* restarts the MCU; returns only without flash */
            reset_due = false;
            pair_due  = false;
        }
        if (pair_due && flash_quiet()) {
            pair_due = false;
            pairing_trigger((uint32_t)now);
        }
        if (pair_remote_pending() && flash_quiet()) {
            pairing_remote((uint32_t)xTaskGetTickCount());
        }
        if (flash_quiet()) {
            network_service((uint32_t)now); /* writes only if network_service_due() */
        }
        if (handoff_pending() && flash_quiet()) {
            update_op_run();
        }
        csl_hold(reset_due || pair_due || pair_remote_pending()
                 || network_service_due((uint32_t)now) || handoff_pending());
        if (quiet()) {
            if ((int32_t)(now - next_health) >= 0) {
                next_health = now + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
                health_check();
            } else if (gdo_irq_on && now64() - cal_at >= (uint64_t)RECAL_PERIOD_MS * 1000u) {
                radio_recalibrate();
            }
            if ((int32_t)(now - next_stats) >= 0) {
                next_stats = now + pdMS_TO_TICKS(STATS_PERIOD_MS);
                wake_flush();
                log_minute();
            }
        }

        /* The pairing LED (#19, owner spec 2026-11-17, amended 2026-11-18;
         * src/pair_led.h): slow flash while the window waits, quick flash
         * during an exchange, lit for 2 s after a pairing, off once the
         * window has closed (after its length, or by the button or the
         * server; a pairing does not close it). On the tick clock at this loop's
         * pace (at most 100 ms late); at its 49.7-day wrap one phase is
         * cut short. */
        uint32_t t       = (uint32_t)xTaskGetTickCount();
        bool     open    = link_pair_active(t);
        bool     engaged = open && link_pair_engaged(t);
        board_led(LED_PAIRING, pair_led_level(&pled, open, engaged, pair_stats()->paired, t));
        /* Their log lines wait for a quiet moment, as everything that logs
         * in this loop: an exchange starts on a frame of the target, and the
         * next frame's Enhanced ACK must not wait behind the log mutex. */
        if (!packet_arriving() && !csl_busy()) {
            const struct pair_stats *ps = pair_stats();
            if (engaged && !was_engaged) {
                LOG("pair: target found: pairing (LED lit)");
            } else if (!engaged && was_engaged) {
                LOG("pair: exchange over%s; exchanges %lu: paired %lu, failed %lu, no progress %lu",
                    open ? ", window still open (LED flashing)" : "",
                    (unsigned long)ps->exchanges,
                    (unsigned long)ps->paired,
                    (unsigned long)ps->ex_failed,
                    (unsigned long)ps->ex_idle);
            }
            if (window_was_open && !open) {
                LOG("pair: window closed; paired so far %lu, d/pair sent %lu delivered %lu",
                    (unsigned long)ps->paired,
                    (unsigned long)ps->dpair_sent,
                    (unsigned long)ps->dpair_delivered);
            }
            was_engaged     = engaged;
            window_was_open = open;
        }

        /* Retire a frame of ours that is over, so its airtime never looks
         * current again after the 32-bit clock wraps (src/cc110l.c). */
        (void)cc110l_tx_expected();

        /* Last radio access of every pass: a TX state that is not ours,
         * caused by any access above, is stopped now, not one pass later. */
        if (!tx_watch()) {
            radio_reinit();
        }
    }
}
