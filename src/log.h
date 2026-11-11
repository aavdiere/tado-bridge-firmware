#ifndef LOG_H
#define LOG_H

#include <stdint.h>

/* printf-like logging over RTT channel 0. No float support (nano printf).
 * Thread-safe once log_lock_init() has run and the scheduler is running
 * (a mutex serialises callers; do not LOG from interrupts). */
void log_init(void);
/* Call after log_init() and before vTaskStartScheduler(); needs the FreeRTOS heap. */
void log_lock_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Fatal error: interrupts off, log over RTT without locking, then spin until
 * the watchdog resets the board. Never returns. */
__attribute__((noreturn)) void fw_panic(const char *what, const char *file, int line);

/* Fault report: log stacked PC/LR/xPSR and the fault status registers, then spin. */
__attribute__((noreturn, used, externally_visible)) void fw_fault_report(const char *name, const uint32_t *frame);

#define LOG(fmt, ...) log_printf(fmt "\r\n", ##__VA_ARGS__)

#endif
