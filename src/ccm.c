/* CCM* for the tado link (#17).
 *
 * Ported from freedo, tado/ccm.go and the keystream and MIC helpers of
 * firmware/cc1101_bridge/cc1101_bridge.ino:883-921.
 * Copyright 2026 The freedo contributors. Licensed under the Apache License,
 * Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0. Changes for
 * this project: C, no allocation, one pass over the data.
 *
 *   B0  = 0x49 | nonce(13) | len(2 BE)          Adata = 1, M = 4, L = 2
 *   AAD = len(2 BE) | aad, zero padded to 16
 *   MIC = first 4 bytes of CBC-MAC(B0 | AAD | plaintext padded) XOR E(A0)
 *   A_i = 0x01 | nonce(13) | i(2 BE); the keystream starts at A1. */
#include "ccm.h"

#include <string.h>

#define B0_FLAGS 0x49u
#define A_FLAGS  0x01u

void ccm_set_key(struct ccm_key *k, const uint8_t key[CCM_KEY_LEN]) {
    mbedtls_aes_init(&k->aes);
    k->set = mbedtls_aes_setkey_enc(&k->aes, key, 128) == 0;
}

void ccm_clear_key(struct ccm_key *k) {
    mbedtls_aes_free(&k->aes); /* zeroizes the key schedule */
    k->set = false;
}

static void ecb(const struct ccm_key *k, const uint8_t in[16], uint8_t out[16]) {
    /* Cannot fail for a context with a key set (vendor/mbedtls/library/aes.c,
     * mbedtls_aes_crypt_ecb). mbedTLS takes a non-const context but does not
     * change it when encrypting. */
    (void)mbedtls_aes_crypt_ecb((mbedtls_aes_context *)&k->aes, MBEDTLS_AES_ENCRYPT, in, out);
}

static void counter_block(uint8_t a[16], const uint8_t nonce[CCM_NONCE_LEN], unsigned i) {
    a[0] = A_FLAGS;
    memcpy(a + 1, nonce, CCM_NONCE_LEN);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
}

/* XOR the keystream A1, A2, ... over data. */
static void
ctr_crypt(const struct ccm_key *k, const uint8_t nonce[CCM_NONCE_LEN], uint8_t *data, size_t len) {
    uint8_t a[16], s[16];
    for (size_t off = 0, i = 1; off < len; off += 16, i++) {
        counter_block(a, nonce, (unsigned)i);
        ecb(k, a, s);
        for (size_t j = 0; j < 16 && off + j < len; j++) {
            data[off + j] ^= s[j];
        }
    }
}

static void cbc_mac(const struct ccm_key *k,
                    const uint8_t         nonce[CCM_NONCE_LEN],
                    const uint8_t        *aad,
                    size_t                aad_len,
                    const uint8_t        *pt,
                    size_t                len,
                    uint8_t               mic[CCM_MIC_LEN]) {
    uint8_t x[16], b[16];

    b[0] = B0_FLAGS;
    memcpy(b + 1, nonce, CCM_NONCE_LEN);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    ecb(k, b, x);

    uint8_t a[2 + CCM_AAD_MAX] = {0};
    a[0]                       = (uint8_t)(aad_len >> 8);
    a[1]                       = (uint8_t)aad_len;
    memcpy(a + 2, aad, aad_len);
    for (size_t off = 0; off < 2 + aad_len; off += 16) {
        for (size_t j = 0; j < 16; j++) {
            b[j] = x[j] ^ a[off + j];
        }
        ecb(k, b, x);
    }
    for (size_t off = 0; off < len; off += 16) {
        for (size_t j = 0; j < 16; j++) {
            b[j] = x[j] ^ (off + j < len ? pt[off + j] : 0u);
        }
        ecb(k, b, x);
    }

    uint8_t s0[16];
    counter_block(b, nonce, 0);
    ecb(k, b, s0);
    for (size_t j = 0; j < CCM_MIC_LEN; j++) {
        mic[j] = x[j] ^ s0[j];
    }
}

static bool args_ok(const struct ccm_key *k, size_t aad_len, size_t len) {
    return k && k->set && aad_len <= CCM_AAD_MAX && len <= 0xFFFFu;
}

bool ccm_seal(const struct ccm_key *k,
              const uint8_t         nonce[CCM_NONCE_LEN],
              const uint8_t        *aad,
              size_t                aad_len,
              uint8_t              *data,
              size_t                len,
              uint8_t               mic[CCM_MIC_LEN]) {
    if (!args_ok(k, aad_len, len)) {
        return false;
    }
    cbc_mac(k, nonce, aad, aad_len, data, len, mic);
    ctr_crypt(k, nonce, data, len);
    return true;
}

bool ccm_open(const struct ccm_key *k,
              const uint8_t         nonce[CCM_NONCE_LEN],
              const uint8_t        *aad,
              size_t                aad_len,
              uint8_t              *data,
              size_t                len,
              const uint8_t         mic[CCM_MIC_LEN]) {
    if (!args_ok(k, aad_len, len)) {
        return false;
    }
    uint8_t want[CCM_MIC_LEN];
    ctr_crypt(k, nonce, data, len);
    cbc_mac(k, nonce, aad, aad_len, data, len, want);
    /* The MIC is not a secret; a plain compare is enough (freedo ccm.go:99). */
    return memcmp(want, mic, CCM_MIC_LEN) == 0;
}
