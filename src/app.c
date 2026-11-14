/* Tasks (#8, #9, #10, #12, #13). Priorities follow docs/design.md "Tasks": radio
 * high, net medium, housekeeping low. */
#include "app.h"

#include "board.h"
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

#define STACK_WORDS     512  /* 2 KB each; LOG() needs about 1 KB of it */
#define NET_STACK_WORDS 1536 /* 6 KB: lwIP input down to the driver (#13), then TLS (#15) */

static TaskHandle_t h_radio, h_net, h_hk;

static void radio_task(void *arg) {
    (void)arg;
    radio_task_run(); /* src/radio.c; owns SPI1, checks in with the watchdog */
}

static void net_task(void *arg) {
    (void)arg;
    net_task_run(); /* src/net.c; owns SPI2, power LED heartbeat, watchdog check-in */
}

struct button {
    const char *name;
    bool        state;   /* debounced, true = pressed */
    uint8_t     pending; /* consecutive samples that disagree with state */
};

#define POLL_MS    10
#define DEBOUNCE_N 3   /* 30 ms */
#define KICK_MS    500 /* watchdog window (docs/build.md P1) */

static void button_update(struct button *b, bool raw) {
    if (raw == b->state) {
        b->pending = 0;
    } else if (++b->pending >= DEBOUNCE_N) {
        b->state   = raw;
        b->pending = 0;
        LOG("button: %s %s", b->name, raw ? "pressed" : "released");
    }
}

static void housekeeping_task(void *arg) {
    (void)arg;
    struct button reset     = {.name = "factory reset (PB10)"};
    struct button pair      = {.name = "pair (PC15)"};
    uint32_t      loops     = 0;
    TickType_t    next_kick = xTaskGetTickCount() + pdMS_TO_TICKS(KICK_MS);

    for (TickType_t wake = xTaskGetTickCount();; loops++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

        button_update(&reset, board_reset_button_raw());
        button_update(&pair, board_pair_button_raw());
        /* The router LED shows Ethernet link and DHCP (net task, #13). */
        board_led(LED_PAIRING, pair.state); /* pairing LED mirrors the pair button */

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
    BaseType_t ok = xTaskCreate(radio_task, "radio", STACK_WORDS, NULL, PRIO_RADIO, &h_radio);
    ok &= xTaskCreate(net_task, "net", NET_STACK_WORDS, NULL, PRIO_NET, &h_net);
    ok &= xTaskCreate(housekeeping_task, "housekeep", STACK_WORDS, NULL, PRIO_HOUSEKEEPING, &h_hk);
    configASSERT(ok == pdPASS);
}
