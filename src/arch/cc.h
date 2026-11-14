#ifndef ARCH_CC_H
#define ARCH_CC_H

/* lwIP compiler and platform glue (#13): GCC, Cortex-M4, little endian
 * (lwip/arch.h defaults), newlib-nano headers. */

#include "log.h"

/* lwIP's own diagnostics (LWIP_DEBUG is off, so only asserts remain). */
#define LWIP_PLATFORM_DIAG(x) \
    do {                      \
        log_printf x;         \
    } while (0)
#define LWIP_PLATFORM_ASSERT(x) fw_panic(x, __FILE__, __LINE__)

#endif
