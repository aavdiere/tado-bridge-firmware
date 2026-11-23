/* The power LED's fault code (docs/hardware.md, "LED roles"): `code` short
 * flashes, then dark, every 2.5 s. One pattern for the application's codes
 * (src/log.h enum fault_code: src/log.c, src/net.c, src/session.c) and the
 * bootloader's (src/boot/bl_hw.h BL_LED_*: src/boot/bl_hw.c). */
#ifndef FAULT_LED_H
#define FAULT_LED_H

#include <stdbool.h>
#include <stdint.h>

#define FAULT_LED_ON_MS    150u
#define FAULT_LED_OFF_MS   250u
#define FAULT_LED_FLASH_MS (FAULT_LED_ON_MS + FAULT_LED_OFF_MS)
#define FAULT_LED_CYCLE_MS 2500u
/* The largest code whose flashes leave a pause before the next cycle. */
#define FAULT_LED_CODE_MAX 6u
_Static_assert((FAULT_LED_CODE_MAX * FAULT_LED_FLASH_MS) < FAULT_LED_CYCLE_MS,
               "the flashes of every code fit the cycle");

/* The dark time after the flashes of code, to the end of the cycle. */
#define FAULT_LED_PAUSE_MS(code) (FAULT_LED_CYCLE_MS - (code) * FAULT_LED_FLASH_MS)

/* The LED's level t_ms into the pattern of code (any t_ms: the pattern
 * repeats every FAULT_LED_CYCLE_MS). */
static inline bool fault_led_on(unsigned code, uint32_t t_ms) {
    uint32_t t = t_ms % FAULT_LED_CYCLE_MS;
    return t / FAULT_LED_FLASH_MS < code && t % FAULT_LED_FLASH_MS < FAULT_LED_ON_MS;
}

#endif
