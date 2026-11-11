#ifndef RTT_H
#define RTT_H

#include <stddef.h>

/* Minimal SEGGER RTT compatible up-channel 0 (target to host) plus an unused
 * 16-byte down channel. Non-blocking: if the buffer is full (no debugger
 * attached or host too slow) output is dropped. */
void rtt_init(void);
size_t rtt_write(const void *data, size_t len);

#endif
