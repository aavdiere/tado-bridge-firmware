#ifndef RNG_H
#define RNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Random numbers (#14): mbedTLS CTR_DRBG (AES-256, NIST SP 800-90A) seeded
 * from the PA3 entropy source (src/entropy.h), docs/design.md "Entropy".
 * Used for lwIP's LWIP_RAND, and by P4 for TLS (mbedtls_ssl_conf_rng with
 * rng_random) and WebSocket masks.
 *
 * Any task, after the scheduler has started: one mutex serialises the DRBG
 * (#17). rng_init() and every call that can reseed belong to the net task,
 * which owns the ADC of the entropy source; a reseed takes about 45 ms.
 * The radio task uses rng_try_bytes() only, which never waits and never
 * reseeds. Network-key generation (#19, #37) needs full-strength output at
 * a moment without radio deadlines: call rng_bytes() from the net task, or
 * accept the wait. */

/* Start-up health test, then the first seed. Logs the result. False if the
 * test or the seed failed: no random numbers are available then. */
bool rng_init(void);

/* f_rng for mbedTLS: 0 on success; non-zero if rng_init() has not succeeded
 * or a reseed failed its health tests (out is then not random). */
int rng_random(void *ctx, unsigned char *out, size_t len);

/* Always random, or never returns: fw_panic() when rng_random() fails, so no
 * caller ever gets a weak value (fail closed). */
void     rng_bytes(void *out, size_t len);
uint32_t rng_u32(void);

/* Random bytes without waiting: false if the DRBG is not seeded yet, another
 * task holds it, a reseed is due, or len is above 1024. For callers with
 * deadlines that can try again later (the radio task's backoff seed). */
bool rng_try_bytes(void *out, size_t len);

#endif
