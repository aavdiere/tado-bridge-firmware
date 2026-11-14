/* mbedTLS 3.6 configuration (vendor/mbedtls, MBEDTLS_CONFIG_FILE in
 * cmake/CMakeLists.txt). Named so that it cannot be confused with mbedTLS's
 * own include/mbedtls/mbedtls_config.h.
 *
 * #14 needs only SHA-256 (entropy conditioning) and CTR_DRBG with AES-256
 * (the DRBG), docs/design.md "Entropy". P4 (#15) adds TLS 1.2 with
 * ECDHE-ECDSA-AES128-GCM-SHA256 here (design.md "TLS settings"). */
#ifndef TADO_MBEDTLS_CONFIG_H
#define TADO_MBEDTLS_CONFIG_H

/* No operating system entropy: the only source is the PA3 pool, given to
 * CTR_DRBG directly (src/entropy.c, src/rng.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES /* tables in flash, not RAM */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER /* about 1 KB less flash, slower; the pool hashes little */

/* 48 bytes of conditioned entropy per (re)seed, 1.5 x the 256-bit security
 * strength, so CTR_DRBG asks for no separate nonce (good_nonce_len(),
 * vendor/mbedtls/library/ctr_drbg.c). */
#define MBEDTLS_CTR_DRBG_ENTROPY_LEN 48

#endif
