#include "log.h"

#include "board.h"
#include "rtt.h"
#include "timebase.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <libopencm3/cm3/scb.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static SemaphoreHandle_t lock;
static volatile int      panicking;

void log_init(void) {
    rtt_init();
}

void log_lock_init(void) {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
}

void log_printf(const char *fmt, ...) {
    char line[192];
    bool use_lock = lock && !panicking && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (use_lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 1) {
        n = sizeof line - 1;
    }
    if (n > 0) {
        rtt_write(line, (size_t)n);
    }
    if (use_lock) {
        xSemaphoreGive(lock);
    }
}

/* Panic path: flash the power LED code, then let the IWDG reset the board.
 * Runs with interrupts off and no scheduler: it waits on the free-running TIM2
 * counter (timebase_us) and writes the LED pin by register. The IWDG is kicked
 * for the first PANIC_KICK_CYCLES flash cycles (about 10 s at most) so the
 * code can be read, then not kicked: the board resets 2-5.6 s later (the IWDG
 * timeout), so a panic never hangs for good and the reset cause logs IWDG. */
#define PANIC_FLASH_ON_MS  150u
#define PANIC_FLASH_OFF_MS 250u
#define PANIC_CYCLE_MS     2500u
#define PANIC_KICK_CYCLES  4u

static void panic_wait_ms(uint32_t ms, bool kick) {
    uint32_t t0 = timebase_us();
    while ((uint32_t)(timebase_us() - t0) < ms * 1000u) {
        if (kick) {
            watchdog_kick();
        }
    }
}

static void panic_flash(unsigned code) {
    board_power_led_force();
    for (unsigned cycle = 0;; cycle++) {
        bool kick = cycle < PANIC_KICK_CYCLES;
        for (unsigned i = 0; i < code; i++) {
            board_led(LED_POWER, true);
            panic_wait_ms(PANIC_FLASH_ON_MS, kick);
            board_led(LED_POWER, false);
            panic_wait_ms(PANIC_FLASH_OFF_MS, kick);
        }
        panic_wait_ms(PANIC_CYCLE_MS - code * (PANIC_FLASH_ON_MS + PANIC_FLASH_OFF_MS), kick);
    }
}

void fw_panic(const char *what, const char *file, int line) {
    __asm volatile("cpsid i" ::: "memory");
    panicking = 1;
    LOG("PANIC: %s at %s:%d; flashing the fault code, then the watchdog resets", what, file, line);
    panic_flash(FAULT_PANIC);
}

/* Called from the fault entry stubs in faults.c with the stacked frame
 * (r0 r1 r2 r3 r12 lr pc xpsr). Interrupts off, no mutex. */
void fw_fault_report(const char *name, const uint32_t *frame) {
    __asm volatile("cpsid i" ::: "memory");
    panicking = 1;
    LOG("FAULT %s: PC %08lx LR %08lx xPSR %08lx",
        name,
        (unsigned long)frame[6],
        (unsigned long)frame[5],
        (unsigned long)frame[7]);
    LOG("FAULT %s: CFSR %08lx HFSR %08lx MMFAR %08lx BFAR %08lx; flashing the fault code, then the "
        "watchdog resets",
        name,
        (unsigned long)SCB_CFSR,
        (unsigned long)SCB_HFSR,
        (unsigned long)SCB_MMFAR,
        (unsigned long)SCB_BFAR);
    panic_flash(FAULT_CPU);
}
