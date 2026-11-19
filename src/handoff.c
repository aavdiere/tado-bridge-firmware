/* Hand-over of one update flash operation with a time limit (#24). See
 * handoff.h. */
#include "handoff.h"

#include "fwupd.h"

#include <stddef.h>

/* The slot: 0 empty; the operation's address (posted); the address with
 * bit 0 set (claimed). struct fwupd_op holds uint32_t fields, so its
 * address is a multiple of 4 and bit 0 is free. Compare-and-swap is
 * LDREX/STREX on the Cortex-M4 (single core: an exception between the two
 * clears the exclusive monitor, so the STREX fails and the swap retries);
 * on the host, the same builtins. */
#define CLAIMED ((uintptr_t)1u)

_Static_assert(_Alignof(struct fwupd_op) >= 2, "bit 0 of an operation's address is free");

static uintptr_t slot;

static bool cas(uintptr_t want, uintptr_t to) {
    return __atomic_compare_exchange_n(&slot, &want, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static bool done(const struct fwupd_op *op) {
    return __atomic_load_n(&op->done, __ATOMIC_ACQUIRE);
}

bool handoff_run(struct fwupd_op *op, uint32_t wait_ms) {
    op->ok   = false;
    op->done = false;
    if (!cas(0, (uintptr_t)op)) {
        return false;
    }
    handoff_port_wake();
    uint32_t t0 = handoff_port_ms();
    while (!done(op)) {
        uint32_t waited = handoff_port_ms() - t0;
        if (waited >= wait_ms) {
            if (cas((uintptr_t)op, 0)) {
                return false; /* never claimed: withdrawn, it will not run */
            }
            /* Claimed: it runs now and is bounded; wait for its end. */
            handoff_port_sleep(1000u);
        } else {
            uint32_t left = wait_ms - waited;
            handoff_port_sleep(left < 1000u ? left : 1000u);
        }
    }
    return op->ok;
}

bool handoff_pending(void) {
    return __atomic_load_n(&slot, __ATOMIC_ACQUIRE) != 0;
}

struct fwupd_op *handoff_claim(void) {
    uintptr_t v = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
    if (v == 0 || (v & CLAIMED) != 0 || !cas(v, v | CLAIMED)) {
        return NULL;
    }
    return (struct fwupd_op *)v;
}

void handoff_finish(struct fwupd_op *op) {
    /* The slot first: once done is seen, the poster may post again. */
    __atomic_store_n(&slot, (uintptr_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&op->done, true, __ATOMIC_RELEASE);
}
