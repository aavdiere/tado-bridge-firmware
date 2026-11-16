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

/* Fault codes: number of short flashes of the power LED (docs/hardware.md,
 * "LED roles"). */
enum fault_code { FAULT_PANIC = 1, FAULT_CPU = 2, FAULT_ENTROPY = 3 };

/* Fatal error: interrupts off, log over RTT without locking, flash the power
 * LED FAULT_PANIC (log.c, panic_flash), then reset through the watchdog.
 * Never returns. */
__attribute__((noreturn)) void fw_panic(const char *what, const char *file, int line);

/* Fault report: log stacked PC/LR/xPSR and the fault status registers, then flash FAULT_CPU and
 * reset as fw_panic. */
__attribute__((noreturn, used, externally_visible)) void fw_fault_report(const char     *name,
                                                                         const uint32_t *frame);

#define LOG(fmt, ...) log_printf(fmt "\r\n", ##__VA_ARGS__)

#endif
