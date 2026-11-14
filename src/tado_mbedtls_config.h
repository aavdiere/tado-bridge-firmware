/* mbedTLS 3.6 configuration (vendor/mbedtls, MBEDTLS_CONFIG_FILE in
 * cmake/CMakeLists.txt). Named so that it cannot be confused with mbedTLS's
 * own include/mbedtls/mbedtls_config.h.
 *
 * Two users:
 * - #14: SHA-256 (entropy conditioning) and CTR_DRBG with AES-256 (the
 *   DRBG), docs/design.md "Entropy".
 * - #15: a TLS 1.2 client with exactly one cipher suite,
 *   ECDHE-ECDSA-AES128-GCM-SHA256 on P-256, that verifies the server's
 *   certificate chain against one trust anchor and its subjectAltName
 *   (docs/design.md "TLS settings"). Plus SHA-1 and Base64 for the WebSocket
 *   handshake (RFC 6455 4.1, src/ws.c).
 * Everything else stays off: no TLS 1.3, no PSA, no RSA, no CBC, no
 * renegotiation, no session tickets, no debug strings. */
#ifndef TADO_MBEDTLS_CONFIG_H
#define TADO_MBEDTLS_CONFIG_H

#include <stddef.h>

/* --- Platform ------------------------------------------------------------- */
/* No operating system entropy: the only source is the PA3 pool, given to
 * CTR_DRBG directly (src/entropy.c, src/rng.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
/* Cortex-M4 multiply-accumulate in bignum (bn_mul.h, UMAAL path). */
#define MBEDTLS_HAVE_ASM
/* No MBEDTLS_HAVE_TIME: the bridge has no clock until the server sends the
 * time, and that needs TLS first. So certificate validity dates are not
 * checked (x509_crt.c checks them only with MBEDTLS_HAVE_TIME_DATE). The
 * bench certificates run to 2046 (docs/test-bench.md "Bench certificate"). */

/* All allocations from the FreeRTOS heap (src/tls.c), not newlib malloc:
 * docs/build.md P1 "Heap and malloc". */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO   tls_free
void *tls_calloc(size_t n, size_t size);
void  tls_free(void *p);

/* --- Symmetric, hashes, DRBG ---------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES /* tables in flash, not RAM */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER /* about 1 KB less flash, slower; TLS hashes little */
#define MBEDTLS_SHA1_C         /* Sec-WebSocket-Accept only (RFC 6455 4.2.2) */
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C

/* 48 bytes of conditioned entropy per (re)seed, 1.5 x the 256-bit security
 * strength, so CTR_DRBG asks for no separate nonce (good_nonce_len(),
 * vendor/mbedtls/library/ctr_drbg.c). */
#define MBEDTLS_CTR_DRBG_ENTROPY_LEN 48

/* --- Public key: P-256 only ----------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM /* fast reduction modulo the P-256 prime */
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C /* required by MBEDTLS_ECDSA_C (check_config.h) */
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* --- X.509: parse the trust anchor and the server chain -------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C /* the trust anchor is built in as PEM text */
#define MBEDTLS_BASE64_C    /* PEM, and the WebSocket key and accept values */

/* --- TLS 1.2 client, one suite --------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_CIPHERSUITES           MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET /* RFC 7627; tanoclo's Node server offers it */

/* Record buffers: 4 KB in, 2 KB out instead of 16 KB each (design.md "TLS
 * settings"). The client also asks for max_fragment_length (RFC 6066 4,
 * src/tls.c), so the server must keep its records small. mbedTLS
 * advertises at most the smaller of the two buffers
 * (vendor/mbedtls/library/ssl_misc.h:334-341, ssl_tls.c:3033-3038), so the
 * request is 2048, which leaves the 4 KB receive buffer half used.
 * tanoclo's Node TLS server acknowledges it: `openssl s_client -maxfraglen
 * 2048` (and 4096) gets "max fragment length" back in its ServerHello
 * (2026-11-14, bench). */
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_IN_CONTENT_LEN  4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

/* Not defined on purpose:
 * MBEDTLS_SSL_KEEP_PEER_CERTIFICATE: the server certificate is freed after
 *   the handshake; only its hash would be kept, and nothing needs it.
 * MBEDTLS_SSL_SERVER_NAME_INDICATION: the bench server is an IP address,
 *   and tanoclo's port 988 picks its certificate by client address, not SNI
 *   (tanoclo/ws-server/lib/proxy-manager.js:136-144).
 * MBEDTLS_SSL_SESSION_TICKETS: would let a reconnect skip the P-256 work;
 *   not needed yet (docs/design.md "TLS settings"). */

#endif
