/* Minimal RTT control block, layout compatible with SEGGER RTT (V7/V8):
 * 16-byte ID, two buffer counts, then up and down buffer descriptors.
 * Written from the public layout description; no SEGGER code used. */
#include "rtt.h"

#include <stdint.h>
#include <string.h>

#define UP_SIZE 1024
#define DOWN_SIZE 16

typedef struct {
    const char *name;
    char *buf;
    uint32_t size;
    volatile uint32_t wr_off;
    volatile uint32_t rd_off;
    uint32_t flags; /* 0 = skip when full */
} rtt_buf_t;

typedef struct {
    char id[16];
    int32_t max_up;
    int32_t max_down;
    rtt_buf_t up[1];
    rtt_buf_t down[1];
} rtt_cb_t;

static char up_buf[UP_SIZE];
static char down_buf[DOWN_SIZE];
__attribute__((used)) static rtt_cb_t cb;

void rtt_init(void) {
    cb.max_up = 1;
    cb.max_down = 1;
    cb.up[0] = (rtt_buf_t){"Terminal", up_buf, UP_SIZE, 0, 0, 0};
    cb.down[0] = (rtt_buf_t){"Terminal", down_buf, DOWN_SIZE, 0, 0, 0};
    /* ID last, so a host never sees a half-initialised block. */
    memcpy(cb.id, "SEGGER RTT\0\0\0\0\0\0", 16);
    __asm volatile("dmb" ::: "memory");
}

size_t rtt_write(const void *data, size_t len) {
    const char *p = data;
    rtt_buf_t *b = &cb.up[0];
    uint32_t wr = b->wr_off, rd = b->rd_off;
    uint32_t free = (rd > wr) ? rd - wr - 1 : UP_SIZE - 1 - wr + rd;
    if (len > free) {
        len = free; /* drop the tail, never block */
    }
    for (size_t i = 0; i < len; i++) {
        up_buf[wr] = p[i];
        if (++wr == UP_SIZE) {
            wr = 0;
        }
    }
    __asm volatile("dmb" ::: "memory");
    b->wr_off = wr;
    return len;
}
