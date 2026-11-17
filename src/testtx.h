/* TEMPORARY test-frame sender for the first over-the-air test on channel 10
 * (#11). Revert after the test, with the commit that added it.
 *
 * Sends fixed unicast Data frames (69 ec) from our address to a made-up
 * destination, so the SDR can see the bridge's own transmissions: carrier
 * offset (#7, #11), frame timing, output level. Nothing here is a working
 * protocol frame: the "sealed" part and the MIC are fixed dummy bytes, no
 * key is used, and the destination is a locally administered EUI-64 that
 * no tado device has (not 00 1b c5 ..., not ours).
 *
 * Armed only over SWD: testtx_arm must hold TESTTX_ARM_MAGIC. It is in .bss,
 * zeroed at every boot, so a reset or a panic stops it. A run ends by itself
 * after TESTTX_MAX_FRAMES frames or TESTTX_MAX_RUN_US, whichever comes
 * first, or as soon as testtx_arm no longer holds the magic word; it cannot
 * start again until the next reset.
 *
 * This file holds the hardware-independent part (frame and schedule, host
 * tested in tests/host/test_testtx.c); src/radio.c sends through the
 * coordinator's port (csl_port_listen_begin/busy, csl_port_send), so every
 * guard of the driver stays in force. */
#ifndef TESTTX_H
#define TESTTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TESTTX_ARM_MAGIC  0x54583130u /* ASCII "TX10" */
#define TESTTX_LEN        40u         /* MAC frame without the radio's length byte and CRC */
#define TESTTX_PERIOD_US  5000000u    /* one slot every 5 s at the fastest */
#define TESTTX_MAX_FRAMES 60u
#define TESTTX_MAX_RUN_US 300000000ull /* 5 minutes from the start line */

/* Written by the debugger only. */
extern volatile uint32_t testtx_arm;

/* The test frame: 69 ec seq | dst(8) | own[0:5] | 20 dummy bytes | 4 dummy
 * MIC bytes. own is our wire EUI-64. */
void testtx_build(uint8_t out[TESTTX_LEN], const uint8_t own[8], uint8_t seq);

/* The destination in wire order (EUI-64 02:00:00:00:00:00:7e:57 reversed). */
extern const uint8_t testtx_dst[8];

enum testtx_step {
    TESTTX_IDLE,        /* nothing to do now */
    TESTTX_START,       /* armed: the run starts now (log the start line) */
    TESTTX_SLOT,        /* send one frame now, after a listen */
    TESTTX_STOP_COUNT,  /* TESTTX_MAX_FRAMES sent */
    TESTTX_STOP_TIME,   /* TESTTX_MAX_RUN_US passed */
    TESTTX_STOP_DISARM, /* testtx_arm no longer holds the magic word */
};

struct testtx {
    bool     running, done;
    uint64_t start_us, next_us;
    unsigned slots, sent;
};

/* One look at the schedule. armed: testtx_arm == TESTTX_ARM_MAGIC now. A
 * STOP_* result is given once; from then on only IDLE. After SLOT the
 * caller reports with testtx_sent() whether the frame went out. */
enum testtx_step testtx_poll(struct testtx *t, uint64_t now_us, bool armed);
void             testtx_sent(struct testtx *t);

#endif
