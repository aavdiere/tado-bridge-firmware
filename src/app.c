/* Tasks (#8, #9, #10, #12, #13). Priorities follow docs/design.md "Tasks": radio
 * high, net medium, housekeeping low. */
#include "app.h"

#include "board.h"
#include "button.h"
#include "log.h"
#include "net.h"
#include "radio.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>

#define PRIO_HOUSEKEEPING (tskIDLE_PRIORITY + 1)
#define PRIO_NET          (tskIDLE_PRIORITY + 2)
#define PRIO_RADIO        (tskIDLE_PRIORITY + 3)

#define STACK_WORDS 512 /* 2 KB each; LOG() needs about 1 KB of it */
/* 3.5 KB. Deepest path (#19): the pair button creates a network, whose key
 * draw may reseed the DRBG: radio_task_run 232 + network_create 88 +
 * rng_random 24 + mbedtls_ctr_drbg_random(_with_add) 104 + reseed 416 +
 * block_cipher_df 832 + AES 124 bytes = about 1.8 KB by -fstack-usage
 * (without LTO), plus an exception frame of up to 104 bytes with the FPU:
 * about 1.9 KB, against about 1.1 KB on the receive and coordinator paths
 * (#17). 896 words leave about 1.6 KB. To be measured on the bench
 * ("alive:" line, after a window opened on a bridge without a network). */
#define RADIO_STACK_WORDS 896
#define NET_STACK_WORDS   1536 /* 6 KB: lwIP input down to the driver (#13), then TLS (#15) */

static TaskHandle_t h_radio, h_net, h_hk;

static void radio_task(void *arg) {
    (void)arg;
    radio_task_run(); /* src/radio.c; owns SPI1, checks in with the watchdog */
}

static void net_task(void *arg) {
    (void)arg;
    net_task_run(); /* src/net.c; owns SPI2, watchdog check-in */
}

#define POLL_MS         10
#define KICK_MS         500   /* watchdog window (docs/build.md P1) */
#define PAIR_HOLD_MS    1000  /* the pair button opens a pairing window (#19) */
#define FACTORY_HOLD_MS 10000 /* the factory reset button erases the network (P6) */

/* Debounced press and release lines, as before the button module. */
static void button_log(const char *name, unsigned ev) {
    if (ev & BUTTON_PRESS) {
        LOG("button: %s pressed", name);
    }
    if (ev & BUTTON_RELEASE) {
        LOG("button: %s released", name);
    }
}

static void housekeeping_task(void *arg) {
    (void)arg;
    struct button reset, pair;
    uint32_t      loops     = 0;
    TickType_t    next_kick = xTaskGetTickCount() + pdMS_TO_TICKS(KICK_MS);
    /* The factory reset needs a release first: held from power-on (or PB10
     * stuck low) it never fires (src/button.h). */
    button_init(&reset, true);
    button_init(&pair, false);

    for (TickType_t wake = xTaskGetTickCount();; loops++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

        /* Holding the factory reset button FACTORY_HOLD_MS erases the
         * network record and the counter log, keeps the identity, and
         * restarts the bridge (radio task, which owns SPI1). Once per
         * press. */
        unsigned ev = button_poll(&reset, board_reset_button_raw(), FACTORY_HOLD_MS / POLL_MS);
        button_log("factory reset (PB10)", ev);
        if (ev & BUTTON_HOLD) {
            LOG("button: factory reset held %u s: factory reset requested", FACTORY_HOLD_MS / 1000);
            radio_request_factory_reset();
        }
        /* The router LED shows Ethernet link and DHCP (net task, #13); the
         * pairing LED shows a pairing window and its exchanges (radio task,
         * #19). Holding the pair button PAIR_HOLD_MS opens a window, or
         * closes the open one; once per press. */
        ev = button_poll(&pair, board_pair_button_raw(), PAIR_HOLD_MS / POLL_MS);
        button_log("pair (PC15)", ev);
        if (ev & BUTTON_HOLD) {
            LOG("button: pair held %u ms: pairing window requested", PAIR_HOLD_MS);
            radio_request_pairing();
        }

        /* Every 500 ms by the clock, not by loop count: after the net task's
         * P-256 steps have held this task off for a few hundred ms (#15),
         * vTaskDelayUntil() catches up in a burst, and counted windows would
         * then close a few ms apart with no check-ins in the second one. */
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_kick) >= 0) {
            next_kick = now + pdMS_TO_TICKS(KICK_MS);
            watchdog_checkin(WDG_HOUSEKEEPING);
            uint32_t missing = watchdog_kick_if_alive();
            if (missing) {
                LOG("watchdog: not kicked, missing check-in mask %lx", (unsigned long)missing);
            }
        }
        if (loops % 1000 == 999) { /* every 10 s */
            /* #10: flash JEDEC ID on the shared SPI1 bus while the radio
             * receives. The radio task owns the bus and runs it. */
            radio_request_flash_check();
            LOG("alive: heap free %u min %u, stack words never used radio/net/hk %u/%u/%u",
                (unsigned)xPortGetFreeHeapSize(),
                (unsigned)xPortGetMinimumEverFreeHeapSize(),
                (unsigned)uxTaskGetStackHighWaterMark(h_radio),
                (unsigned)uxTaskGetStackHighWaterMark(h_net),
                (unsigned)uxTaskGetStackHighWaterMark(h_hk));
        }
    }
}

void app_create_tasks(void) {
    BaseType_t ok = xTaskCreate(radio_task, "radio", RADIO_STACK_WORDS, NULL, PRIO_RADIO, &h_radio);
    ok &= xTaskCreate(net_task, "net", NET_STACK_WORDS, NULL, PRIO_NET, &h_net);
    ok &= xTaskCreate(housekeeping_task, "housekeep", STACK_WORDS, NULL, PRIO_HOUSEKEEPING, &h_hk);
    configASSERT(ok == pdPASS);
}
