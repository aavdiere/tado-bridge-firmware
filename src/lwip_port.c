/* lwIP NO_SYS port (#13): time base and random numbers. See src/lwipopts.h. */
#include "lwip/arch.h"
#include "lwip/sys.h"

#include "timebase.h"

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

/* PLACEHOLDER (#26): not cryptographic. lwIP uses it for the DHCP
 * transaction id, the DNS query id and source port (LWIP_DNS_SECURE), and
 * the first TCP local port, which are guess-resistance measures, not
 * secrets. xorshift32 (Marsaglia 2003), seeded from the 96-bit unique device
 * ID (base 0x1FFF7A10, docs/datasheets/rm0383.txt:46698-46711) so two bridges
 * differ, with the 1 MHz TIM2 count mixed in at every call so network timing
 * adds variation between boots. P4 replaces this with the DRBG seeded from
 * the PA3 entropy pool (docs/design.md "Entropy"). */
#define UID_BASE 0x1FFF7A10u

uint32_t lwip_port_rand(void) {
    static uint32_t s;
    if (s == 0u) {
        const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
        s                            = uid[0] ^ (uid[1] * 0x9E3779B9u) ^ (uid[2] * 0x85EBCA6Bu);
    }
    s ^= timebase_us();
    if (s == 0u) {
        s = 0x2545F491u; /* xorshift must not start from 0 */
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
