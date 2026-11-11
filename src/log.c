#include "log.h"

#include "rtt.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdarg.h>
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
    char line[128];
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
