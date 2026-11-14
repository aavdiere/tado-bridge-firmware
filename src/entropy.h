#ifndef ENTROPY_H
#define ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Entropy source (#14, docs/design.md "Entropy (#26)"): raw ADC readings of
 * the unconnected pin PA3 (src/pa3_adc.c), health-tested as in NIST SP
 * 800-90B section 4.4 and conditioned with SHA-256. Measured noise and the
 * credited entropy: docs/design.md "Entropy: measurement".
 *
 * Fails closed: until entropy_startup() has passed, and after any health-test
 * failure, entropy_get() returns no output. Net task only (it owns ADC1);
 * no locking. */

struct entropy_report {
    uint32_t    samples;  /* readings taken by the start-up test */
    uint32_t    timeouts; /* reads without EOC in time (preemption or a dead ADC) */
    uint16_t    min, max; /* range of the start-up readings */
    uint16_t    rct_max;  /* longest run of one value seen */
    uint16_t    apt_max;  /* most repeats of a window's first value in one window */
    const char *fail;     /* NULL if passed, else which test failed */
};

/* Start-up test (SP 800-90B 4.3): powers ADC1, discards the settling
 * readings, runs the repetition count and adaptive proportion tests on 1024
 * consecutive readings and checks that they spread over a minimum range
 * (design.md: refuse readings that are stuck or barely vary). Clears an
 * earlier failure if it passes. Takes about 2 ms. */
bool entropy_startup(struct entropy_report *r);

/* Conditioned entropy, f_entropy for mbedtls_ctr_drbg_seed() (and usable as
 * lwIP's ALTCP_MBEDTLS_RNG_FN): fills out[0..len) with SHA-256 outputs, each
 * over readings credited with at least 512 bits of min-entropy, twice the
 * 256-bit output. ctx is unused. Returns 0, or -1 with out cleared if the
 * start-up test has not passed or a health test fails during collection
 * (the failure latches until entropy_startup() passes again). */
int entropy_get(void *ctx, unsigned char *out, size_t len);

#endif
