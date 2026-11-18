/* mbedTLS configuration for the host tests (#17): only the AES block cipher
 * that src/ccm.c uses and SHA-256 for the image checks (#24, src/fwimg.c),
 * built from the same vendor/mbedtls sources as the firmware. The firmware's own configuration is src/tado_mbedtls_config.h. */
#ifndef MBEDTLS_HOST_CONFIG_H
#define MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_SHA256_C

#endif
