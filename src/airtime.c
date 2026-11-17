/* Rolling airtime limit for the transmitter (#17). See airtime.h for the
 * regulatory basis and the window. */
#include "airtime.h"

#include <string.h>

void airtime_advance(struct airtime *a, uint32_t now_us) {
    /* Modulo 2^32: never more than the time that really passed. */
    uint32_t dt = now_us - a->last_us;
    a->last_us  = now_us;
    if ((uint64_t)a->into_us + dt < AIRTIME_BUCKET_US) {
        a->into_us += dt;
        return;
    }
    uint64_t t = (uint64_t)a->into_us + dt;
    if (t >= (uint64_t)AIRTIME_BUCKETS * AIRTIME_BUCKET_US) {
        /* Every bucket is older than the window. */
        memset(a->bucket, 0, sizeof a->bucket);
        a->used_us = 0;
        a->into_us = (uint32_t)(t % AIRTIME_BUCKET_US);
        return;
    }
    while (t >= AIRTIME_BUCKET_US) {
        t -= AIRTIME_BUCKET_US;
        a->cur = (uint8_t)((a->cur + 1u) % AIRTIME_BUCKETS);
        a->used_us -= a->bucket[a->cur]; /* the oldest minute leaves the window */
        a->bucket[a->cur] = 0;
    }
    a->into_us = (uint32_t)t;
}

bool airtime_charge(struct airtime *a, uint32_t now_us, uint32_t cost_us) {
    airtime_advance(a, now_us);
    if (cost_us > AIRTIME_BUDGET_US - a->used_us) { /* used_us <= budget always */
        return false;
    }
    a->bucket[a->cur] += cost_us;
    a->used_us += cost_us;
    return true;
}

uint32_t airtime_used_us(struct airtime *a, uint32_t now_us) {
    airtime_advance(a, now_us);
    return a->used_us;
}
