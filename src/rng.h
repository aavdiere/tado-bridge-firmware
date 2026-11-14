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
 * Net task only, like the entropy source: no locking. A caller in another
 * task (P5/P6 key generation) needs a lock first. */

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

#endif
