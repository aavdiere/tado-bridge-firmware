/* mbedTLS configuration for the host tests (#17): only the AES block cipher
 * that src/ccm.c uses, built from the same vendor/mbedtls sources as the
 * firmware. The firmware's own configuration is src/tado_mbedtls_config.h. */
#ifndef MBEDTLS_HOST_CONFIG_H
#define MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES

#endif
