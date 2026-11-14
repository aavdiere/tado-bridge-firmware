/* lwIP NO_SYS port (#13): time base. Random numbers come from src/rng.c
 * (LWIP_RAND in src/lwipopts.h). */
#include "lwip/arch.h"
#include "lwip/sys.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>

/* Milliseconds for lwIP's timeouts: the FreeRTOS tick runs at 1 kHz
 * (src/FreeRTOSConfig.h). Wraps after 49.7 days; lwIP compares times
 * modulo 2^32 (TIME_LESS_THAN in core/timeouts.c). */
_Static_assert(configTICK_RATE_HZ == 1000, "sys_now() returns ticks as milliseconds");
u32_t sys_now(void) {
    return (u32_t)xTaskGetTickCount();
}
