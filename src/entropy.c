/* PA3 entropy source: health tests and SHA-256 conditioning (#14).
 * Design: docs/design.md "Entropy (#26)"; measurement and the values below:
 * design.md "Entropy: measurement" and tools/entropy/pa3_noise.py.
 * Health tests follow NIST SP 800-90B section 4.4 (repetition count test,
 * adaptive proportion test, cutoffs from the formulas there). */
#include "entropy.h"

#include "pa3_adc.h"
#include "timebase.h"

#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#include <string.h>

/* Sampling time: 3 ADCCLK cycles. Every setting from 3 to 480 cycles gave
 * the same spread (sd 26 LSB); 3 cycles had the highest lowest estimate
 * (0.87 bit per reading) and is the fastest, about 1 µs per reading [V]
 * (design.md "Entropy: measurement"). */
#define SMP PA3_SMP_3

/* Readings right after power-up climb from about 560 towards the pin's level
 * of about 910 within the first 10-20 as the sample-and-hold capacitor
 * charges [V]; the first 64 are discarded. */
#define SETTLE_SAMPLES 64u

/* Credited min-entropy per 12-bit reading, in 1/1000 bit: 0.1 bit, about a
 * ninth of the lowest SP 800-90B estimate at this setting (0.87, t-tuple) and
 * a third of the lowest over all settings (0.34 at 15 cycles). The readings
 * are mostly a deterministic, partly periodic process with little noise
 * on top, so the credit stays far below the estimates [V]/[I]. */
#define H_CREDIT_MBIT 100u

/* Each 256-bit SHA-256 output is computed over readings credited with
 * 512 bits, twice its size: 5120 readings, about 5 ms of sampling. */
#define BLOCK_CREDIT_BITS 512u
#define BLOCK_SAMPLES     ((BLOCK_CREDIT_BITS * 1000u + H_CREDIT_MBIT - 1u) / H_CREDIT_MBIT)
_Static_assert(BLOCK_SAMPLES == 5120u, "credit and block size as documented");

/* Health-test cutoffs (SP 800-90B 4.4) for H = 0.5 bit per reading, below
 * the 0.87 assessed at this setting and above the 0.1 credited, so that a
 * source that loses most of its entropy fails, while the measured source
 * stays far from the cutoffs: longest run of one value 12 and APT maximum 50
 * of 512 in 10^6 readings [V]. False-positive probability alpha = 2^-30 per
 * reading (4.4 allows 2^-20 to 2^-40). From tools/entropy/pa3_noise.py
 * --h 0.5: RCT C = 1 + ceil(30 / H) = 61 (4.4.1); APT C = 1 +
 * CRITBINOM(512, 2^-H, 1 - alpha) = 422 (4.4.2). */
#define RCT_CUTOFF 61u
#define APT_WINDOW 512u
#define APT_CUTOFF 422u

#define STARTUP_SAMPLES 1024u /* SP 800-90B 4.3: at least 1024 */
/* The 1024 start-up readings must spread over at least 16 LSB; measured
 * 102-129 [V]. */
#define MIN_RANGE    16u
#define MAX_TIMEOUTS 8u /* consecutive reads without EOC: ADC dead */

struct ht_state {
    uint16_t rct_value, rct_count;
    uint16_t apt_value, apt_count, apt_index;
    uint16_t rct_max, apt_max;
    uint32_t samples, timeouts;
};
static struct ht_state ht;

static bool started; /* start-up test passed and no failure since */

static void ht_reset(void) {
    ht = (struct ht_state){.rct_value = 0xFFFFu}; /* no 12-bit reading equals it */
}

/* SP 800-90B 4.4.1 and 4.4.2 on one reading. False on failure. */
static bool ht_feed(uint16_t v) {
    if (v == ht.rct_value) {
        ht.rct_count++;
    } else {
        ht.rct_value = v;
        ht.rct_count = 1;
    }
    if (ht.rct_count > ht.rct_max) {
        ht.rct_max = ht.rct_count;
    }

    if (ht.apt_index == 0u) {
        ht.apt_value = v;
        ht.apt_count = 1;
    } else if (v == ht.apt_value) {
        ht.apt_count++;
    }
    if (ht.apt_count > ht.apt_max) {
        ht.apt_max = ht.apt_count;
    }
    if (++ht.apt_index == APT_WINDOW) {
        ht.apt_index = 0;
    }
    return ht.rct_count < RCT_CUTOFF && ht.apt_count < APT_CUTOFF;
}

static bool read_one(uint16_t *v) {
    for (unsigned i = 0; i < MAX_TIMEOUTS; i++) {
        if (pa3_adc_read(v)) {
            ht.samples++;
            return true;
        }
        ht.timeouts++;
    }
    return false;
}

/* Powers the ADC up, discards the settling readings, restarts the tests. */
static bool source_on(void) {
    pa3_adc_on(SMP);
    ht_reset();
    for (unsigned i = 0; i < SETTLE_SAMPLES; i++) {
        uint16_t v;
        if (!read_one(&v)) {
            return false;
        }
    }
    ht.samples = 0;
    return true;
}

bool entropy_startup(struct entropy_report *r) {
    uint16_t    lo = 0xFFFFu, hi = 0;
    const char *fail = NULL;
    started          = false;
    if (!source_on()) {
        fail = "failed: ADC conversions do not complete";
    }
    for (unsigned i = 0; fail == NULL && i < STARTUP_SAMPLES; i++) {
        uint16_t v;
        if (!read_one(&v)) {
            fail = "failed: ADC conversions do not complete";
        } else if (!ht_feed(v)) {
            fail = ht.rct_count >= RCT_CUTOFF ? "failed: repetition count test"
                                              : "failed: adaptive proportion test";
        } else {
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
    }
    pa3_adc_off();
    if (hi < lo) {
        lo = hi = 0; /* no reading */
    }
    if (fail == NULL && (uint16_t)(hi - lo) < MIN_RANGE) {
        fail = "failed: readings barely vary";
    }
    *r = (struct entropy_report){
        .samples  = ht.samples,
        .timeouts = ht.timeouts,
        .min      = lo,
        .max      = hi,
        .rct_max  = ht.rct_max,
        .apt_max  = ht.apt_max,
        .fail     = fail,
    };
    started = (fail == NULL);
    return started;
}

int entropy_get(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    static uint32_t        block_no;
    mbedtls_sha256_context sha;
    unsigned char          digest[32];
    uint8_t                raw[64]; /* 32 readings, little-endian */
    bool                   ok = started && source_on();

    mbedtls_sha256_init(&sha);
    for (size_t done = 0; ok && done < len;) {
        /* Domain tag, block counter and the 1 MHz count (timing jitter):
         * uncredited, they only keep two blocks from hashing equal input. */
        uint32_t hdr[3] = {0x50413301u /* "PA3", version 1 */, block_no++, timebase_us()};
        ok = mbedtls_sha256_starts(&sha, 0) == 0
             && mbedtls_sha256_update(&sha, (const unsigned char *)hdr, sizeof hdr) == 0;
        for (unsigned i = 0; ok && i < BLOCK_SAMPLES;) {
            unsigned n = 0;
            for (; ok && n < sizeof raw / 2u && i < BLOCK_SAMPLES; n++, i++) {
                uint16_t v       = 0;
                ok               = read_one(&v) && ht_feed(v);
                raw[2u * n]      = (uint8_t)v;
                raw[2u * n + 1u] = (uint8_t)(v >> 8);
            }
            ok = ok && mbedtls_sha256_update(&sha, raw, 2u * n) == 0;
        }
        uint32_t t = timebase_us();
        ok         = ok && mbedtls_sha256_update(&sha, (const unsigned char *)&t, sizeof t) == 0
                     && mbedtls_sha256_finish(&sha, digest) == 0;
        if (ok) {
            size_t n = len - done < sizeof digest ? len - done : sizeof digest;
            memcpy(out + done, digest, n);
            done += n;
        }
    }
    pa3_adc_off();
    mbedtls_sha256_free(&sha);
    mbedtls_platform_zeroize(digest, sizeof digest);
    mbedtls_platform_zeroize(raw, sizeof raw);
    if (!ok) {
        started = false; /* latched until entropy_startup() passes again */
        mbedtls_platform_zeroize(out, len);
        return -1;
    }
    return 0;
}
