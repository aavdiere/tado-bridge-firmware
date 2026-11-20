/* Frequency offset of our own network's frames, and the drift warning
 * (#11). See freqoff.h. */
#include "freqoff.h"

static void acc_clear(struct freqoff_acc *a) {
    a->n   = 0;
    a->sum = 0;
    a->min = INT8_MAX;
    a->max = INT8_MIN;
}

static void acc_add(struct freqoff_acc *a, int fe) {
    if (a->n >= FREQOFF_MAX_FRAMES) {
        return;
    }
    a->n++;
    a->sum += fe;
    a->min = fe < a->min ? fe : a->min;
    a->max = fe > a->max ? fe : a->max;
}

void freqoff_init(struct freqoff *s) {
    acc_clear(&s->minute);
    acc_clear(&s->window);
    s->warned = false;
}

void freqoff_add(struct freqoff *s, int fe) {
    if (fe < INT8_MIN || fe > INT8_MAX) {
        return; /* not a FREQEST value */
    }
    acc_add(&s->minute, fe);
    acc_add(&s->window, fe);
}

int32_t freqoff_mean_hz(int32_t sum, uint32_t n) {
    if (n == 0) {
        return 0;
    }
    /* On the magnitude, unsigned: half away from zero, and no signed 64-bit
     * division in the image (the unsigned one is linked anyway). */
    uint64_t a   = sum < 0 ? (uint64_t)-(int64_t)sum : (uint64_t)sum;
    uint64_t den = (uint64_t)n * FREQOFF_UNIT_HZ_DEN;
    int32_t  m   = (int32_t)((a * FREQOFF_UNIT_HZ_NUM * 2u + den) / (2u * den));
    return sum < 0 ? -m : m;
}

bool freqoff_over_limit(int32_t sum, uint32_t n) {
    if (n == 0) {
        return false;
    }
    int64_t a = sum < 0 ? -(int64_t)sum : (int64_t)sum;
    return a * FREQOFF_UNIT_HZ_NUM > (int64_t)FREQOFF_LIMIT_HZ * FREQOFF_UNIT_HZ_DEN * n;
}

void freqoff_report(struct freqoff *s, struct freqoff_report *r) {
    r->minute_n       = s->minute.n;
    r->minute_mean_hz = freqoff_mean_hz(s->minute.sum, s->minute.n);
    r->minute_min     = s->minute.n ? s->minute.min : 0;
    r->minute_max     = s->minute.n ? s->minute.max : 0;
    acc_clear(&s->minute);

    r->window_n       = s->window.n;
    r->window_mean_hz = freqoff_mean_hz(s->window.sum, s->window.n);
    if (s->window.n < FREQOFF_MIN_FRAMES) {
        r->verdict = FREQOFF_PENDING;
        return;
    }
    if (freqoff_over_limit(s->window.sum, s->window.n)) {
        r->verdict = FREQOFF_WARN;
        s->warned  = true;
    } else {
        r->verdict = s->warned ? FREQOFF_BACK : FREQOFF_WITHIN;
        s->warned  = false;
    }
    acc_clear(&s->window);
}
