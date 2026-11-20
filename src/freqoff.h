#ifndef FREQOFF_H
#define FREQOFF_H

/* Frequency offset of our own network's frames, and the drift warning (#11,
 * owner decision 2026-11-20: no per-unit trim, a run-time warning instead).
 * Hardware independent (tests/host/test_freqoff.c); the radio task feeds it
 * and logs (src/radio.c frame_done() and log_minute()).
 *
 * Input: the in-packet FREQEST sample (signed units of fXOSC / 2^14 =
 * 203125 / 128 Hz = 1586.9 Hz, datasheets/cc110l.txt:4429-4434) of frames
 * that had a good CRC and that the link layer accepted from our network
 * (link_rx_from_network(), src/link.h). Nothing here sets FSCTRL0 or
 * changes what is sent: it only reports.
 *
 * Two accumulators:
 *   minute  the frames since the last 60 s report: count, mean, min..max,
 *           printed in every report.
 *   window  the frames since the last verdict. At a report, a window with
 *           at least FREQOFF_MIN_FRAMES frames gets a verdict and starts
 *           again; a smaller one is carried into the next minute, so a
 *           network with a single device (1-6 frames a minute on the bench)
 *           still gets one. Crystal drift is slow (temperature, ageing), so
 *           a window spread over a few minutes is still one reading.
 *
 * Verdict, at most one per report: |mean| above FREQOFF_LIMIT_HZ warns, in
 * every report while it stays above; the first verdict at or below the
 * limit after a warning gives one "back within limit". No hysteresis: a
 * mean that sits at the limit may alternate between the two, one line a
 * minute at most.
 *
 * FREQOFF_LIMIT_HZ = 15 kHz, derived in docs/radio-runtime.md
 * ("Frequency offset warning (#11)"): below the +-17.3 kHz at which the
 * Carson bandwidth of our GFSK (2 x 25.39 kHz + 49.99 kBaud = 100.8 kHz)
 * reaches the edge of the 135.4 kHz channel filter, half the FOC_LIMIT of
 * +-BW/4 = +-33.9 kHz (FOCCFG = 0x16, cc110l.txt:4045-4051), and above the
 * worst of the bench: -6 units (-9.5 kHz) on stock senders, +1..+3 units
 * on our own network [V]. 15 kHz is 9.45 units and 17.3 ppm at 866.3 MHz.
 *
 * FREQOFF_MIN_FRAMES = 4: one device gives FREQEST within one unit from
 * frame to frame on the bench [V], so four frames already fix the mean to
 * well under a unit; fewer would let one odd frame decide.
 *
 * The mean mixes every device heard: a shift of the bridge's own crystal
 * moves all of them alike, which is what this watches; one device far off
 * among several shows in min..max of the minute line before it moves the
 * mean. */

#include <stdbool.h>
#include <stdint.h>

#define FREQOFF_UNIT_HZ_NUM 203125 /* fXOSC / 2^14 = 203125 / 128 Hz */
#define FREQOFF_UNIT_HZ_DEN 128
#define FREQOFF_LIMIT_HZ    15000
#define FREQOFF_MIN_FRAMES  4u
/* Frames per accumulator at most: more are not counted. A minute holds
 * fewer than 60 s / 1.6 ms = 37500 frames (at least 10 bytes on the air at
 * 160 us each), so the cap is not reached in practice; it keeps |sum| below
 * 1.3e7. */
#define FREQOFF_MAX_FRAMES 100000u

struct freqoff_acc {
    uint32_t n;
    int32_t  sum; /* units */
    int      min, max;
};

struct freqoff {
    struct freqoff_acc minute, window;
    bool               warned; /* the last verdict warned */
};

enum freqoff_verdict {
    FREQOFF_PENDING, /* window below FREQOFF_MIN_FRAMES: carried on */
    FREQOFF_WITHIN,  /* within the limit, nothing to log */
    FREQOFF_WARN,    /* above the limit: log the warning */
    FREQOFF_BACK,    /* within the limit after a warning: log it once */
};

struct freqoff_report {
    uint32_t             minute_n;
    int32_t              minute_mean_hz; /* 0 if minute_n is 0 */
    int                  minute_min, minute_max;
    enum freqoff_verdict verdict;
    uint32_t             window_n;       /* the window judged (PENDING: so far) */
    int32_t              window_mean_hz; /* 0 if window_n is 0 */
};

void freqoff_init(struct freqoff *s);
/* One accepted frame's FREQEST, -128..127 units. */
void freqoff_add(struct freqoff *s, int fe);
/* The 60 s report: fills r, starts a new minute and, after a verdict, a
 * new window. */
void freqoff_report(struct freqoff *s, struct freqoff_report *r);

/* Mean of sum units over n frames in Hz, rounded half away from zero; 0 if
 * n is 0. */
int32_t freqoff_mean_hz(int32_t sum, uint32_t n);
/* |sum / n| in Hz above FREQOFF_LIMIT_HZ, exactly (no rounding). */
bool freqoff_over_limit(int32_t sum, uint32_t n);

#endif
