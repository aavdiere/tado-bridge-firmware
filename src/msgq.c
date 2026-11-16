/* Single-writer, single-reader message queue (#17). See msgq.h. */
#include "msgq.h"

#define LEN_BYTES 2u

void msgq_init(struct msgq *q, uint8_t *buf, size_t cap) {
    q->buf  = buf;
    q->cap  = (uint32_t)cap;
    q->head = 0;
    q->tail = 0;
}

/* Bytes stored; one byte of the buffer always stays free so that a full
 * queue differs from an empty one. */
static uint32_t used(const struct msgq *q, uint32_t head, uint32_t tail) {
    return tail >= head ? tail - head : q->cap - head + tail;
}

size_t msgq_room(const struct msgq *q) {
    uint32_t space = q->cap - 1u - used(q, q->head, q->tail);
    return space > LEN_BYTES ? space - LEN_BYTES : 0u;
}

static uint32_t put_bytes(struct msgq *q, uint32_t at, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        q->buf[at] = p[i];
        at         = at + 1u == q->cap ? 0u : at + 1u;
    }
    return at;
}

bool msgq_put(struct msgq *q, const void *a, size_t alen, const void *b, size_t blen) {
    size_t   len   = alen + blen;
    uint32_t tail  = q->tail;
    uint32_t space = q->cap - 1u - used(q, q->head, tail);
    if (len > 0xFFFFu || len + LEN_BYTES > space) {
        return false;
    }
    uint8_t hdr[LEN_BYTES] = {(uint8_t)(len >> 8), (uint8_t)len};
    tail                   = put_bytes(q, tail, hdr, LEN_BYTES);
    tail                   = put_bytes(q, tail, a, alen);
    tail                   = put_bytes(q, tail, b, blen);
    q->tail                = tail; /* the message becomes visible here, complete */
    return true;
}

static uint8_t at(const struct msgq *q, uint32_t head, uint32_t off) {
    uint32_t i = head + off;
    return q->buf[i >= q->cap ? i - q->cap : i];
}

size_t msgq_peek(struct msgq *q, uint8_t *out, size_t cap) {
    uint32_t head = q->head;
    if (head == q->tail) {
        return 0;
    }
    size_t len = (size_t)at(q, head, 0) << 8 | at(q, head, 1);
    if (len > cap) {
        msgq_pop(q);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = at(q, head, (uint32_t)(LEN_BYTES + i));
    }
    return len;
}

void msgq_pop(struct msgq *q) {
    uint32_t head = q->head;
    if (head == q->tail) {
        return;
    }
    uint32_t len = (uint32_t)at(q, head, 0) << 8 | at(q, head, 1);
    uint32_t nxt = head + LEN_BYTES + len;
    q->head      = nxt >= q->cap ? nxt - q->cap : nxt;
}
