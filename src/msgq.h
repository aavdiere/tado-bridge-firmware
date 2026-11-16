/* Message queue between two tasks (#17): one writer, one reader, no lock and
 * no allocation. The radio task and the net task pass relayed messages
 * through two of these, so neither waits for the other (docs/design.md
 * "Tasks"). Storage is a static byte buffer given at init; each message is
 * stored with a 2-byte length.
 *
 * Safe with exactly one task calling msgq_put() and one calling
 * msgq_peek()/msgq_pop(): each side writes only its own index, and a 32-bit
 * index load or store is atomic on the Cortex-M4. Hardware independent. */
#ifndef MSGQ_H
#define MSGQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct msgq {
    uint8_t          *buf;
    uint32_t          cap;
    volatile uint32_t head; /* next byte to read; reader only */
    volatile uint32_t tail; /* next byte to write; writer only */
};

void msgq_init(struct msgq *q, uint8_t *buf, size_t cap);

/* Largest message that can be put now (0 if not even an empty one fits). */
size_t msgq_room(const struct msgq *q);

/* Writer: store the message a | b (b may be NULL with blen 0). False, and
 * nothing stored, if it does not fit or is longer than 65535 bytes. */
bool msgq_put(struct msgq *q, const void *a, size_t alen, const void *b, size_t blen);

/* Reader: copy the oldest message to out and return its length; 0 if the
 * queue is empty. A message longer than cap is dropped and 0 returned. The
 * message stays queued until msgq_pop(). */
size_t msgq_peek(struct msgq *q, uint8_t *out, size_t cap);
void   msgq_pop(struct msgq *q);

#endif
