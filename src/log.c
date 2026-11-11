#include "log.h"

#include "rtt.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <libopencm3/cm3/scb.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
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

void fw_panic(const char *what, const char *file, int line) {
    __asm volatile("cpsid i" ::: "memory");
    panicking = 1;
    LOG("PANIC: %s at %s:%d; waiting for the watchdog", what, file, line);
    for (;;) {
    }
}

/* Called from the fault entry stubs in faults.c with the stacked frame
 * (r0 r1 r2 r3 r12 lr pc xpsr). Interrupts off, no mutex. */
void fw_fault_report(const char *name, const uint32_t *frame) {
    __asm volatile("cpsid i" ::: "memory");
    panicking = 1;
    LOG("FAULT %s: PC %08lx LR %08lx xPSR %08lx", name, (unsigned long)frame[6],
        (unsigned long)frame[5], (unsigned long)frame[7]);
    LOG("FAULT %s: CFSR %08lx HFSR %08lx MMFAR %08lx BFAR %08lx; waiting for the watchdog", name,
        (unsigned long)SCB_CFSR, (unsigned long)SCB_HFSR, (unsigned long)SCB_MMFAR,
        (unsigned long)SCB_BFAR);
    for (;;) {
    }
}
