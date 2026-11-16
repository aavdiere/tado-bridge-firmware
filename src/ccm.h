/* CCM* as the tado 868 MHz link uses it (#17): AES-128, 13-byte nonce,
 * 2-byte length field, 4-byte MIC (B0 flags 0x49). Ported from freedo
 * (Apache-2.0, see ccm.c). Hardware independent; the AES block cipher is
 * mbedTLS's (vendor/mbedtls/library/aes.c).
 *
 * No heap: the key schedule lives in the caller's struct ccm_key. mbedTLS's
 * own CCM module is not used, because its cipher layer allocates the AES
 * context on the heap (vendor/mbedtls/library/cipher.c, mbedtls_cipher_setup)
 * and the radio side keeps off the FreeRTOS heap (docs/design.md "RAM
 * measured (#16, #17)"). */
#ifndef CCM_H
#define CCM_H

#include "mbedtls/aes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CCM_KEY_LEN   16u
#define CCM_NONCE_LEN 13u
#define CCM_MIC_LEN   4u
#define CCM_AAD_MAX   30u /* two CBC-MAC blocks with the 2-byte length prefix */

struct ccm_key {
    mbedtls_aes_context aes;
    bool                set;
};

void ccm_set_key(struct ccm_key *k, const uint8_t key[CCM_KEY_LEN]);
void ccm_clear_key(struct ccm_key *k);

/* Encrypts data in place and writes the MIC over (nonce, aad, plaintext).
 * aad_len <= CCM_AAD_MAX, len <= 65535. False if the key is not set or a
 * length is out of range; data is then untouched. */
bool ccm_seal(const struct ccm_key *k,
              const uint8_t         nonce[CCM_NONCE_LEN],
              const uint8_t        *aad,
              size_t                aad_len,
              uint8_t              *data,
              size_t                len,
              uint8_t               mic[CCM_MIC_LEN]);

/* Decrypts data in place and checks the MIC. False on a mismatch; data then
 * holds the (unauthenticated) decryption and must not be used. */
bool ccm_open(const struct ccm_key *k,
              const uint8_t         nonce[CCM_NONCE_LEN],
              const uint8_t        *aad,
              size_t                aad_len,
              uint8_t              *data,
              size_t                len,
              const uint8_t         mic[CCM_MIC_LEN]);

#endif
