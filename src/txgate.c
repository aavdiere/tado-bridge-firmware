/* Transmit gate (#17). Rules and their sources: src/txgate.h. */
#include "txgate.h"

#include "mac.h"

#include <string.h>

/* The tado channel plan as the stock register table programs it
 * (docs/radio.md "Stock CC110L register table"): FREQ = 0x213276
 * (863.125 MHz), CHANSPC_E = 2, CHANSPC_M = 0xF8 (199.951 kHz). */
#define FXOSC_HZ       26000000ll
#define PLAN_FREQ_WORD 0x213276l
#define PLAN_CHANSPC_E 2u
#define PLAN_CHANSPC_M 0xF8u

/* Carrier in units of fXOSC / 2^18 (99.2 Hz), which keeps every term whole:
 *   carrier = fXOSC / 2^16 x FREQ                           (cc110l.txt:3729-3736)
 *           + CHAN x fXOSC / 2^18 x (256 + CHANSPC_M) x 2^CHANSPC_E  (:3869-3880)
 *           + fXOSC / 2^14 x FREQOFF                         (:3713-3722) */
static int64_t carrier_units(const struct txgate_radio *r) {
    int64_t freq  = (int64_t)r->freq2 << 16 | (int64_t)r->freq1 << 8 | r->freq0;
    int64_t space = (int64_t)(256 + r->mdmcfg0) << (r->mdmcfg1 & 3u);
    return freq * 4 + (int64_t)r->channr * space + (int64_t)(int8_t)r->fsctrl0 * 16;
}

int64_t txgate_carrier_hz(const struct txgate_radio *r) {
    return carrier_units(r) * FXOSC_HZ / 262144;
}

static int64_t protected_hz(void) {
    static const struct txgate_radio p = {
        .freq2   = (uint8_t)(PLAN_FREQ_WORD >> 16),
        .freq1   = (uint8_t)(PLAN_FREQ_WORD >> 8),
        .freq0   = (uint8_t)PLAN_FREQ_WORD,
        .channr  = TXGATE_PROTECTED_CHANNEL,
        .mdmcfg1 = PLAN_CHANSPC_E,
        .mdmcfg0 = PLAN_CHANSPC_M,
    };
    return txgate_carrier_hz(&p);
}

enum txgate_verdict txgate_check_channel(const struct txgate_radio *r) {
    /* The distance in Hz first: it holds whatever the other registers say. */
    int64_t d = txgate_carrier_hz(r) - protected_hz();
    if (d < 0) {
        d = -d;
    }
    if (d < TXGATE_MIN_DIST_HZ) {
        return TXGATE_PROTECTED;
    }
    long freq = (long)r->freq2 << 16 | (long)r->freq1 << 8 | r->freq0;
    int  trim = (int8_t)r->fsctrl0;
    if (freq != PLAN_FREQ_WORD || (r->mdmcfg1 & 3u) != PLAN_CHANSPC_E
        || r->mdmcfg0 != PLAN_CHANSPC_M || trim > TXGATE_MAX_TRIM || trim < -TXGATE_MAX_TRIM) {
        return TXGATE_OFF_PLAN;
    }
    if (r->channr > TXGATE_MAX_CHANNEL) {
        return TXGATE_BAD_CHANNEL;
    }
    unsigned cd = r->channr > TXGATE_PROTECTED_CHANNEL ? r->channr - TXGATE_PROTECTED_CHANNEL
                                                       : TXGATE_PROTECTED_CHANNEL - r->channr;
    if (cd < TXGATE_MIN_CHANNEL_DIST) {
        return TXGATE_PROTECTED;
    }
    if (r->channr != TXGATE_TEST_CHANNEL) {
        return TXGATE_NOT_TEST_CHANNEL;
    }
    return TXGATE_OK;
}

static bool eui_set(const uint8_t own[8]) {
    static const uint8_t zero[8];
    return own && memcmp(own, zero, 8) != 0 && memcmp(own + 5, mac_eui_suffix, 3) == 0;
}

enum txgate_verdict txgate_check_frame(const struct txgate_radio *r,
                                       const uint8_t              own[8],
                                       const uint8_t             *f,
                                       size_t                     len,
                                       const uint8_t             *answered,
                                       unsigned                   flags) {
    enum txgate_verdict v = txgate_check_channel(r);
    if (v != TXGATE_OK) {
        return v;
    }
    if (!eui_set(own)) {
        return TXGATE_NO_IDENTITY;
    }
    if (!f || len < 2 || len > MAC_FRAME_MAX) {
        return TXGATE_BAD_LENGTH;
    }
    /* Rule 5. With rule 2 in force the channel check above already implies
     * it; it stays separate so that lifting rule 2 later cannot lift it. */
    bool on_test = r->channr == TXGATE_TEST_CHANNEL;
    if ((flags & TXGATE_PAIRING) && !on_test) {
        return TXGATE_NOT_TEST_CHANNEL;
    }

    switch (mac_classify(f, len)) {
        case MAC_WAKEUP: {
            struct mac_wakeup w;
            uint8_t           again[MAC_WAKE_LEN];
            if (len != MAC_WAKE_LEN || !mac_parse_wakeup(f, len, &w)) {
                return TXGATE_BAD_LENGTH;
            }
            mac_build_wakeup(again, w.seq, w.dst_short, w.rendezvous);
            return memcmp(again, f, MAC_WAKE_LEN) == 0 ? TXGATE_OK : TXGATE_MALFORMED;
        }

        case MAC_DATA:
        case MAC_DATA_PENDING:
            if (len < MAC_HDR_LEN + MAC_SHIM_LEN + MAC_FCS_LEN + MAC_MIC_LEN) {
                return TXGATE_BAD_LENGTH;
            }
            if (memcmp(f + 11, own, 5) != 0) {
                return TXGATE_NOT_OUR_SRC;
            }
            if (memcmp(f + 3, own, 8) == 0) {
                return TXGATE_MALFORMED; /* a frame to ourselves */
            }
            return TXGATE_OK;

        case MAC_BROADCAST:
            if (len < MAC_HDR_LEN + 2u + MAC_FCS_LEN + MAC_MIC_LEN) {
                return TXGATE_BAD_LENGTH;
            }
            if (f[3] != 0xFF || f[4] != 0xFF) {
                return TXGATE_MALFORMED;
            }
            return memcmp(f + 5, own, 8) == 0 ? TXGATE_OK : TXGATE_NOT_OUR_SRC;

        case MAC_ENH_ACK: {
            uint8_t want[MAC_ACK_LEN];
            if (len != MAC_ACK_LEN) {
                return TXGATE_BAD_LENGTH;
            }
            if (!answered) {
                return TXGATE_NO_ANSWER;
            }
            /* Rebuilt from the answered frame: refused unless that frame is
             * a unicast Data frame addressed to us, and the ACK is its
             * answer byte for byte (only the CSL phase is the caller's). */
            if (!mac_ack_for_data(want, answered, own, (uint16_t)(f[21] | f[22] << 8))) {
                return TXGATE_NOT_FOR_US;
            }
            return memcmp(want, f, MAC_ACK_LEN) == 0 ? TXGATE_OK : TXGATE_WRONG_ANSWER;
        }

        case MAC_BEACON:
            /* 18 ec seq | PAN(2) | dst(8) | src(8) | ..., answering
             * 0b e8 seq | ff ff | ff ff | src(8) | ... (freedo tado/beacon.go:
             * 17-27). Pairing only; the caller (#19) also has to check that
             * the joiner is the device named for the window. */
            if (len != 32u) {
                return TXGATE_BAD_LENGTH;
            }
            if (!(flags & TXGATE_PAIRING) || !on_test) {
                return TXGATE_NOT_TEST_CHANNEL;
            }
            if (!answered) {
                return TXGATE_NO_ANSWER;
            }
            if (mac_classify(answered, MAC_HDR_LEN) != MAC_BEACON_REQUEST) {
                return TXGATE_WRONG_ANSWER;
            }
            if (memcmp(f + 13, own, 8) != 0) {
                return TXGATE_NOT_OUR_SRC;
            }
            if (memcmp(f + 5, answered + 7, 8) != 0 || memcmp(answered + 7, own, 8) == 0) {
                return TXGATE_WRONG_ANSWER;
            }
            return TXGATE_OK;

        default:
            return TXGATE_BAD_CLASS;
    }
}

const char *txgate_reason(enum txgate_verdict v) {
    static const char *const s[] = {
        [TXGATE_OK]               = "ok",
        [TXGATE_OFF_PLAN]         = "frequency registers off the channel plan",
        [TXGATE_BAD_CHANNEL]      = "channel above 49",
        [TXGATE_PROTECTED]        = "on or near the protected channel",
        [TXGATE_NOT_TEST_CHANNEL] = "not on the test channel",
        [TXGATE_NO_IDENTITY]      = "own EUI-64 not set",
        [TXGATE_BAD_LENGTH]       = "bad frame length",
        [TXGATE_BAD_CLASS]        = "frame class the bridge does not send",
        [TXGATE_NOT_OUR_SRC]      = "source address is not ours",
        [TXGATE_NO_ANSWER]        = "answer without the frame it answers",
        [TXGATE_NOT_FOR_US]       = "answered frame is not addressed to us",
        [TXGATE_WRONG_ANSWER]     = "does not match the answered frame",
        [TXGATE_MALFORMED]        = "malformed frame",
    };
    return (unsigned)v < sizeof s / sizeof s[0] && s[v] ? s[v] : "?";
}
