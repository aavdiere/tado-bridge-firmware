/* TEMPORARY test-frame sender (#11): frame and schedule. See testtx.h.
 * Revert after the test. */
#include "testtx.h"

#include <string.h>

/* .bss: zero at every boot, so never armed after a reset or a panic. */
volatile uint32_t testtx_arm;

/* EUI-64 02:00:00:00:00:00:7e:57, in wire order. Bit 1 of the first byte
 * set: locally administered, so no manufacturer assigns it; not tado's
 * 00 1b c5 prefix (wire tail c5 1b 00), so it is no tado device's. */
const uint8_t testtx_dst[8] = {0x57, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

/* Fixed dummy bytes where a sealed frame has its ciphertext and MIC. Not
 * encrypted with anything: a receiver with a key fails the MIC. */
static const char dummy_body[] = "TEST FRAME NOT TADO."; /* 20 bytes, no NUL sent */
#define DUMMY_BODY_LEN (sizeof dummy_body - 1u)
static const uint8_t dummy_mic[4] = {0x5A, 0x5A, 0x5A, 0x5A};

_Static_assert(3u + 8u + 5u + DUMMY_BODY_LEN + sizeof dummy_mic == TESTTX_LEN, "test frame layout");
_Static_assert(TESTTX_LEN <= 64u, "at most 64 bytes");
_Static_assert((uint64_t)TESTTX_MAX_FRAMES *TESTTX_PERIOD_US >= TESTTX_MAX_RUN_US,
               "the time limit ends a run no later than the frame limit at full rate");

void testtx_build(uint8_t out[TESTTX_LEN], const uint8_t own[8], uint8_t seq) {
    out[0] = 0x69;
    out[1] = 0xEC;
    out[2] = seq;
    memcpy(out + 3, testtx_dst, 8);
    memcpy(out + 11, own, 5);
    memcpy(out + 16, dummy_body, DUMMY_BODY_LEN);
    memcpy(out + 16 + DUMMY_BODY_LEN, dummy_mic, sizeof dummy_mic);
}

enum testtx_step testtx_poll(struct testtx *t, uint64_t now_us, bool armed) {
    if (t->done) {
        return TESTTX_IDLE;
    }
    if (!t->running) {
        if (!armed) {
            return TESTTX_IDLE;
        }
        t->running  = true;
        t->start_us = now_us;
        t->next_us  = now_us;
        return TESTTX_START;
    }
    enum testtx_step stop = TESTTX_IDLE;
    if (!armed) {
        stop = TESTTX_STOP_DISARM;
    } else if (t->sent >= TESTTX_MAX_FRAMES) {
        stop = TESTTX_STOP_COUNT;
    } else if (now_us - t->start_us >= TESTTX_MAX_RUN_US) {
        stop = TESTTX_STOP_TIME;
    }
    if (stop != TESTTX_IDLE) {
        t->running = false;
        t->done    = true;
        return stop;
    }
    if (now_us < t->next_us) {
        return TESTTX_IDLE;
    }
    /* From this slot's own time, so two slots are never closer than 5 s. */
    t->next_us = now_us + TESTTX_PERIOD_US;
    t->slots++;
    return TESTTX_SLOT;
}

void testtx_sent(struct testtx *t) {
    t->sent++;
}
