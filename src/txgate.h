/* Transmit gate (#17): the rules every transmission has to pass, as pure
 * functions, so they can be tested on the host and enforced at the lowest
 * layer that keys the transmitter (src/cc110l.c: the only TX FIFO write and
 * the only STX strobe both call them).
 *
 * Rules (CLAUDE.md "Hard rules", docs/test-bench.md "The other tado
 * network"):
 *   1. Never transmit on the protected channel, the owner's stock network
 *      (channel 26, 868.3237 MHz), nor within 5 channels (about 1 MHz) of it.
 *      The carrier is computed from the register values that set it, so a
 *      changed base frequency, channel spacing or offset trim cannot move a
 *      "channel 16" onto it.
 *   2. Transmit only on the test channel (channel 16, 866.3242 MHz) while
 *      the project is on the bench.
 *   3. An Enhanced ACK is sent only as the exact answer to a unicast Data
 *      frame addressed to our own EUI-64.
 *   4. Every other frame carries our own EUI-64 as its source, and only the
 *      frame classes a coordinator sends are let through.
 *   5. Pairing frames (the Enhanced Beacon, and anything flagged as part of
 *      pairing) go out on the test channel only, and a beacon only as the
 *      answer to a Beacon Request. */
#ifndef TXGATE_H
#define TXGATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Channel plan: protected channel, owner decision of 2026-11-11
 * (docs/test-bench.md "Channel scan result"); test channel 16, owner
 * decision of 2026-11-17 (was 10: the spare device scans 11, 16, 21 and 26
 * for a network and never asked on 10; docs/README.md "Project
 * decisions"). Channel 16 is 10 channels (2.0 MHz) from channel 26. */
#define TXGATE_PROTECTED_CHANNEL 26u
#define TXGATE_TEST_CHANNEL      16u
#define TXGATE_MAX_CHANNEL       49u
#define TXGATE_MIN_CHANNEL_DIST  5u       /* docs/test-bench.md rule 2 */
#define TXGATE_MIN_DIST_HZ       999000ll /* 5 channels of 199.951 kHz, rounded down */
/* Largest frequency offset trim (FSCTRL0) in units of 1.587 kHz: 32 units are
 * 50.8 kHz, 59 ppm at 865 MHz, more than any crystal correction needs
 * (docs/open-questions.md row 7). Stock writes 0. */
#define TXGATE_MAX_TRIM 32

/* The CC110L registers that set the carrier (docs/datasheets/cc110l.txt:
 * 3700-3736, 3869-3880). */
struct txgate_radio {
    uint8_t freq2, freq1, freq0; /* 0x0D-0x0F */
    uint8_t channr;              /* 0x0A */
    uint8_t mdmcfg1, mdmcfg0;    /* 0x13, 0x14: channel spacing exponent and mantissa */
    uint8_t fsctrl0;             /* 0x0C: frequency offset trim, signed */
};

enum txgate_verdict {
    TXGATE_OK = 0,
    TXGATE_OFF_PLAN,         /* base frequency, spacing or trim is not the tado channel plan */
    TXGATE_BAD_CHANNEL,      /* channel above 49 */
    TXGATE_PROTECTED,        /* on or too near the protected channel */
    TXGATE_NOT_TEST_CHANNEL, /* rule 2, or a pairing frame elsewhere */
    TXGATE_NO_IDENTITY,      /* own EUI-64 not set */
    TXGATE_BAD_LENGTH,
    TXGATE_BAD_CLASS,    /* a frame class the bridge never sends */
    TXGATE_NOT_OUR_SRC,  /* source address is not ours */
    TXGATE_NO_ANSWER,    /* an ACK or beacon without the frame it answers */
    TXGATE_NOT_FOR_US,   /* the answered frame is not addressed to us */
    TXGATE_WRONG_ANSWER, /* the ACK or beacon does not match the answered frame */
    TXGATE_MALFORMED,
};

/* For log lines. */
const char *txgate_reason(enum txgate_verdict v);

/* Carrier frequency these registers program, in Hz, for a 26 MHz crystal. */
int64_t txgate_carrier_hz(const struct txgate_radio *r);

/* Rules 1 and 2. Checked when a frame is loaded and again at the STX
 * strobe. */
enum txgate_verdict txgate_check_channel(const struct txgate_radio *r);

/* The frame is part of pairing (rule 5). The caller sets it for frames sent
 * under the pairing key; the gate cannot see the key. */
#define TXGATE_PAIRING 0x01u

/* All rules for one frame (without the radio's CRC). answered is the first
 * 16 bytes of the received frame that an Enhanced ACK or an Enhanced Beacon
 * answers, NULL for every other frame. own is our wire EUI-64. */
enum txgate_verdict txgate_check_frame(const struct txgate_radio *r,
                                       const uint8_t              own[8],
                                       const uint8_t             *frame,
                                       size_t                     len,
                                       const uint8_t             *answered,
                                       unsigned                   flags);

#endif
