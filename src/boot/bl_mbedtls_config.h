/* mbedTLS configuration for the bootloader (#24): SHA-256 only, the small
 * variant as in the application (src/tado_mbedtls_config.h). */
#ifndef BL_MBEDTLS_CONFIG_H
#define BL_MBEDTLS_CONFIG_H

#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER

#endif
