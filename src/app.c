/* P1 tasks (#8). Priorities follow docs/design.md "Tasks": radio high, net
 * medium, housekeeping low. The radio and net bodies are placeholders. */
#include "app.h"

#include "board.h"
#include "log.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>

/* Deliberate-hang test (docs/build.md). Build with HANG_TEST=0 to remove it.
 *   hold the factory reset button > 5 s:            net task stalls (stops checking in)
 *   hold reset + pair together > 5 s:               radio task busy-spins at higher priority
 * Either way the housekeeping task stops kicking or gets starved, and the IWDG
 * resets the board. The factory reset button does nothing else. */
#ifndef HANG_TEST
#define HANG_TEST 1
#endif
#define HANG_HOLD_MS 5000

#define PRIO_HOUSEKEEPING (tskIDLE_PRIORITY + 1)
#define PRIO_NET          (tskIDLE_PRIORITY + 2)
#define PRIO_RADIO        (tskIDLE_PRIORITY + 3)

#define STACK_WORDS 512 /* 2 KB each; LOG() needs about 1 KB of it */

static TaskHandle_t h_radio, h_net, h_hk;

enum hang { HANG_NONE, HANG_STALL_NET, HANG_SPIN_RADIO };
static volatile enum hang hang_request;

static void radio_task(void *arg) {
    (void)arg;
    bool cloud = false;
    for (unsigned n = 0;; n++) {
        if (hang_request == HANG_SPIN_RADIO) {
            LOG("HANG TEST: radio task spins, expect an IWDG reset");
            for (;;) {
            }
        }
        watchdog_checkin(WDG_RADIO);
        if (n % 10 == 0) { /* every 500 ms: cloud LED blinks from this task (placeholder) */
            cloud = !cloud;
            board_led(LED_CLOUD, cloud);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void net_task(void *arg) {
    (void)arg;
    bool power = false;
    for (unsigned n = 0;; n++) {
        if (hang_request == HANG_STALL_NET) {
            LOG("HANG TEST: net task stops checking in, expect an IWDG reset");
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000)); /* alive but silent */
            }
        }
        watchdog_checkin(WDG_NET);
        if (n % 10 == 0) { /* heartbeat: power LED toggles every 500 ms */
            power = !power;
            board_led(LED_POWER, power);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

struct button {
    const char *name;
    bool        state;   /* debounced, true = pressed */
    uint8_t     pending; /* consecutive samples that disagree with state */
    uint32_t    held_ms;
};

#define POLL_MS     10
#define DEBOUNCE_N  3 /* 30 ms */

static void button_update(struct button *b, bool raw) {
    if (raw == b->state) {
        b->pending = 0;
    } else if (++b->pending >= DEBOUNCE_N) {
        b->state = raw;
        b->pending = 0;
        b->held_ms = 0;
        LOG("button: %s %s", b->name, raw ? "pressed" : "released");
    }
    if (b->state) {
        b->held_ms += POLL_MS;
    }
}

static void housekeeping_task(void *arg) {
    (void)arg;
    struct button reset = {.name = "factory reset (PB10)"};
    struct button pair = {.name = "pair (PC15)"};
    uint32_t      loops = 0;

    for (TickType_t wake = xTaskGetTickCount();; loops++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

        button_update(&reset, board_reset_button_raw());
        button_update(&pair, board_pair_button_raw());
        board_led(LED_ROUTER, reset.state); /* router LED mirrors the reset button */
        board_led(LED_PAIRING, pair.state); /* pairing LED mirrors the pair button */

#if HANG_TEST
        if (hang_request == HANG_NONE && reset.held_ms > HANG_HOLD_MS) {
            hang_request = pair.state ? HANG_SPIN_RADIO : HANG_STALL_NET;
        }
#endif

        if (loops % 50 == 49) { /* every 500 ms */
            watchdog_checkin(WDG_HOUSEKEEPING);
            uint32_t missing = watchdog_kick_if_alive();
            if (missing) {
                LOG("watchdog: not kicked, missing check-in mask %lx", (unsigned long)missing);
            }
        }
        if (loops % 1000 == 999) { /* every 10 s */
            LOG("alive: heap free %u min %u, stack words never used radio/net/hk %u/%u/%u",
                (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
                (unsigned)uxTaskGetStackHighWaterMark(h_radio),
                (unsigned)uxTaskGetStackHighWaterMark(h_net),
                (unsigned)uxTaskGetStackHighWaterMark(h_hk));
        }
    }
}

void app_create_tasks(void) {
    BaseType_t ok = xTaskCreate(radio_task, "radio", STACK_WORDS, NULL, PRIO_RADIO, &h_radio);
    ok &= xTaskCreate(net_task, "net", STACK_WORDS, NULL, PRIO_NET, &h_net);
    ok &= xTaskCreate(housekeeping_task, "housekeep", STACK_WORDS, NULL, PRIO_HOUSEKEEPING, &h_hk);
    configASSERT(ok == pdPASS);
}
