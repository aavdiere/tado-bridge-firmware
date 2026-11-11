#ifndef LOG_H
#define LOG_H

/* printf-like logging over RTT channel 0. No float support (nano printf). */
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(fmt, ...) log_printf(fmt "\r\n", ##__VA_ARGS__)

#endif
