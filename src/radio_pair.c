/* Radio task, pairing (#19, #40): the Enhanced Beacon that answers the
 * target's Beacon Request, and opening and closing a pairing window for the
 * pair button and the server's request. Part of the radio task: how pairing
 * runs, at the top of src/radio.c; the state it shares, src/radio_internal.h. */
#include "radio.h"

#include "radio_internal.h"

#include "cc110l.h"
#include "ccm.h"
#include "csl.h"
#include "link.h"
#include "log.h"
#include "mac.h"
#include "network.h"
#include "pair.h"
#include "pair_remote.h"
#include "phy.h"
#include "provision.h"
#include "rng.h"
#include "timebase.h"

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/platform_util.h"

#include <libopencm3/stm32/gpio.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

/* --- Pairing: the beacon answer (#19) -------------------------------------------
 * The target's Beacon Request has just been taken (frame_done()) and its
 * Enhanced Beacon built (link_beacon()). Deadline: the beacon's preamble
 * starts 2.4-7.4 ms after the request ended (pair.h). The request's end is
 * gdo0_fall_us, stamped by the GDO0 interrupt within microseconds of the
 * edge, later by at most one SPI1 critical section (about 0.1 ms), which
 * only moves the beacon later. Steps:
 *   1. The stamp must be the request's own end (own_end: exactly one GDO0
 *      edge since its length byte was read, so this task took the request
 *      while it was arriving). Otherwise the stamp may belong to a later
 *      packet: no beacon.
 *   2. Wait, spinning, until PAIR_BEACON_TX_AT_US after the stamp (about
 *      3.7 ms minus the time this task needed to get here: the pump, the
 *      CCM check of the request and building the beacon, well under
 *      0.5 ms [I]). Any GDO0 edge (a sync word), GDO0 high, a frame of ours
 *      on the air, a coordinator transaction holding the air or starting
 *      its listen less than PAIR_BEACON_QUIET_US (15 ms) from the decision
 *      (the beacon must not shorten that listen), or an RSSI at or above
 *      LBT_RSSI_BUSY_DBM at the end makes the channel busy: no beacon. The
 *      commercial bridge listens 3.968 ms here (freedo
 *      cc1101_bridge.ino:228-230).
 *   3. Too late (past PAIR_BEACON_TX_LATEST_US, e.g. this task was blocked
 *      by a flash program): no beacon.
 *   4. cc110l_tx_answer(): channel read-back, transmit gate (pairing flag,
 *      test channel, answers this request), airtime, SIDLE/SFRX/SFTX, 33
 *      FIFO bytes, STX: about 0.15 ms [I]. Then IDLE to TX 75.2 us
 *      (docs/datasheets/cc110l.txt:2542) to the first preamble bit.
 * So the preamble starts about 3.9-4.0 ms after the request ended, 1.5 ms
 * after the earliest and 3.4 ms before the latest the device expects [I].
 * The device asks again 8 s later when no beacon comes. Logged after the
 * beacon is on the air: STX time after the request's end. PB8 (pairing LED)
 * goes off here, a few microseconds of GPIO write, and rises right after
 * the STX strobe, whatever the LED showed before (a flash phase, or lit
 * during an exchange): about 3.3 ms dark, too short to see. A beacon not
 * sent leaves it off until the end of this task loop pass, which sets it
 * as the window says. */
void beacon_answer(uint32_t fall_us, bool own_end) {
    size_t         len      = 0;
    const uint8_t *answered = NULL;
    const uint8_t *b        = link_beacon(&len, &answered);
    if (!b) {
        return;
    }
    board_led(LED_PAIRING, false);
    uint32_t edges = gdo0_edges;
    uint32_t el    = timebase_us() - fall_us;
    bool     busy  = false;
    while (own_end && el < PAIR_BEACON_TX_AT_US) {
        if (gdo0_edges != edges || gpio_get(GPIOA, BOARD_GDO0_PIN) || cc110l_tx_expected()
            || !csl_quiet_for(PAIR_BEACON_QUIET_US)) {
            busy = true;
            break;
        }
        uint32_t left = PAIR_BEACON_TX_AT_US - el;
        delay_us(left > 100u ? 100u : left);
        el = timebase_us() - fall_us;
    }
    if (own_end && !busy) {
        uint8_t raw = 0;
        busy = gdo0_edges != edges || gpio_get(GPIOA, BOARD_GDO0_PIN)
               || !csl_quiet_for(PAIR_BEACON_QUIET_US) || !cc110l_read_status_stable(CC_RSSI, &raw)
               || rssi_x2(raw) >= 2 * LBT_RSSI_BUSY_DBM;
        el   = timebase_us() - fall_us;
    }
    uint32_t stx = 0;
    bool     ok  = false;
    if (!own_end) {
        COUNT(beacon_no_stamp);
    } else if (busy) {
        COUNT(beacon_busy);
    } else if (el > PAIR_BEACON_TX_LATEST_US) {
        COUNT(beacon_late);
    } else if ((ok = send_frame(b, len, answered, TXGATE_PAIRING, true, &stx))) {
        COUNT(beacon_sent);
    } else {
        COUNT(beacon_fail);
    }
    uint8_t seq = b[2];
    link_beacon_done(ok, (uint32_t)xTaskGetTickCount());
    if (ok) {
        LOG("pair: beacon seq %02x to the target sent: STX %lu us after the request ended "
            "(preamble about %lu us; window %u-%u us)",
            seq,
            (unsigned long)(stx - fall_us),
            (unsigned long)(stx - fall_us + PHY_IDLE_TO_TX_US),
            PAIR_BEACON_START_MIN_US,
            PAIR_BEACON_START_MAX_US);
    } else {
        LOG("pair: beacon seq %02x to the target NOT sent: %s (%lu us after the request ended)",
            seq,
            !own_end                        ? "request end not seen live"
            : busy                          ? "channel busy"
            : el > PAIR_BEACON_TX_LATEST_US ? "too late"
                                            : "refused or failed",
            (unsigned long)el);
    }
}

/* Opening a pairing window (#19), for the pair button and for the
 * server's remote trigger (#40) alike: for the device named in the build,
 * for window_ms (at most PAIR_WINDOW_MS, src/pair.h). Runs at
 * flash_quiet() (no packet arriving, no transaction set up), never inside
 * a window: creating a network blocks this task for up to about 0.8 s
 * (fw-reviewer items 1-3 of #19). A window therefore needs, all of them: a
 * target named in config/local.cmake (PAIR_TARGET_EUI64), the transmitter
 * built in (RADIO_TX_ALLOWED, the CMake option), the flash read at boot,
 * an identity, the radio up with PLL lock, no frame of the last window
 * still queued, and the owner's hand on the button or the server's
 * request. Without a target the code below is not even compiled: the build
 * then holds no path to network_create() or link_pair_open(). Returns
 * PAIR_REMOTE_OPENED or why not (src/pair_remote.h); logs only the
 * creation of a network. */

/* The one test for a target named in the build: radio_pair_target_named()
 * for the boot banner (src/main.c) and the session (src/session.c), and
 * the compile-time switch of pairing_open(). */
#ifdef PROVISION_PAIR_EUI64
#define PAIR_TARGET_NAMED 1
#else
#define PAIR_TARGET_NAMED 0
#endif

bool radio_pair_target_named(void) {
    return PAIR_TARGET_NAMED;
}

static enum pair_remote_rc pairing_open(uint32_t window_ms) {
#if !PAIR_TARGET_NAMED
    (void)window_ms;
    return PAIR_REMOTE_NO_TARGET;
#else
    static const uint8_t eui[MAC_EUI_LEN] = PROVISION_PAIR_EUI64;
    uint8_t              target[MAC_EUI_LEN];
    for (unsigned i = 0; i < MAC_EUI_LEN; i++) {
        target[i] = eui[MAC_EUI_LEN - 1u - i]; /* wire order, as link_start() */
    }
    if (!RADIO_TX_ALLOWED) {
        return PAIR_REMOTE_RX_ONLY;
    }
    if (!flash_ready || !link_own()) {
        return !flash_ready ? PAIR_REMOTE_NO_FLASH : PAIR_REMOTE_NO_IDENTITY;
    }
    /* The target must not be the bridge itself (its EUI-64 from the
     * identity record, #21); checked before any network is created. */
    if (memcmp(target, link_own(), MAC_EUI_LEN) == 0) {
        return PAIR_REMOTE_SELF_TARGET;
    }
    if (!gdo_irq_on || !cc110l_cal_ok()) {
        return PAIR_REMOTE_RADIO;
    }
    if (link_pair_frames_queued()) {
        return PAIR_REMOTE_QUEUED;
    }
    uint8_t key[CCM_KEY_LEN], rnd[PAIR_RAND_LEN];
    bool    ok;
    if (network_active()) {
        ok = network_key(key);
    } else if (network_absent()) {
        LOG("pair: no network yet: creating one on channel %u before the window opens",
            RADIO_CHANNEL);
        ok = network_create(RADIO_CHANNEL, key);
    } else if (network_replaceable(NULL)) {
        /* A record written by an image for another channel (stage P7.1,
         * src/network.h): this pair request replaces it. */
        LOG("pair: the network record is for another channel: a new network on channel %u "
            "replaces it before the window opens",
            RADIO_CHANNEL);
        ok = network_create(RADIO_CHANNEL, key);
    } else {
        return PAIR_REMOTE_NETWORK;
    }
    ok = ok && rng_random(NULL, rnd, sizeof rnd) == 0
         && link_pair_open(target, key, rnd, (uint32_t)xTaskGetTickCount(), window_ms);
    mbedtls_platform_zeroize(key, sizeof key);
    mbedtls_platform_zeroize(rnd, sizeof rnd);
    return ok ? PAIR_REMOTE_OPENED : PAIR_REMOTE_OPEN_FAILED;
#endif
}

/* The pair button (#19): open a window of PAIR_WINDOW_MS, or close the one
 * that is open (whoever opened it). */
void pairing_trigger(uint32_t now_ms) {
    if (link_pair_active(now_ms)) {
        link_pair_close();
        LOG("pair: window closed by the pair button");
        return;
    }
    enum pair_remote_rc rc = pairing_open(PAIR_WINDOW_MS);
    if (rc == PAIR_REMOTE_OPENED) {
        LOG("pair: window open for %u s on channel %u, for the device named in the build only; "
            "LED flashing until it is found",
            PAIR_WINDOW_MS / 1000u,
            RADIO_CHANNEL);
    } else if (rc == PAIR_REMOTE_QUEUED) {
        LOG("pair: frames of the last window still queued: no window; hold the button again in "
            "a few seconds");
    } else if (rc == PAIR_REMOTE_OPEN_FAILED) {
        LOG("pair: %s", pair_remote_text(rc));
    } else {
        LOG("pair: %s: no window", pair_remote_text(rc));
    }
}

/* The server's remote trigger (#40, src/pair_remote.h), posted by the net
 * task (src/session.c): duration 0 closes the open window, whoever opened
 * it; a duration above 0 opens one through pairing_open() above, with
 * every guard of the button, or leaves an open window as it is (its
 * length, its counters and an exchange in progress unchanged). At
 * flash_quiet(), as the button. The outcome goes back to the net task,
 * which answers the request and writes its one log line; nothing is logged
 * here except a network's creation. */
void pairing_remote(uint32_t now_ms) {
    bool     open;
    uint32_t window_ms, secs = 0;
    if (!pair_remote_claim(&open, &window_ms)) {
        return;
    }
    enum pair_remote_rc rc;
    if (!open) {
        rc = PAIR_REMOTE_WAS_CLOSED;
        if (link_pair_active(now_ms)) {
            link_pair_close();
            rc = PAIR_REMOTE_CLOSED;
        }
    } else if (link_pair_active(now_ms)) {
        rc   = PAIR_REMOTE_WAS_OPEN;
        secs = link_pair_left_ms(now_ms) / 1000u;
    } else {
        rc   = pairing_open(window_ms);
        secs = rc == PAIR_REMOTE_OPENED ? window_ms / 1000u : 0u;
    }
    pair_remote_finish(rc, secs);
}

void radio_request_remote_pairing(void) {
    if (radio_self) {
        xTaskNotify(radio_self, NOTE_PAIR_REMOTE, eSetBits);
    }
}
