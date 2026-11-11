#ifndef LOG_H
#define LOG_H

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

#define LOG(fmt, ...) log_printf(fmt "\r\n", ##__VA_ARGS__)

#endif
