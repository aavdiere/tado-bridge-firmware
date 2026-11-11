/* Placeholder: startup delay, blink PB0 (router LED, active high), RTT hello.
 * Clock tree (#7) is in clock.c; FreeRTOS/LEDs/button/watchdog are #8. */
#include "clock.h"
#include "log.h"

#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/rcc.h>

#define LED_PORT GPIOB
#define LED_PIN GPIO0 /* router LED, drive high = on (docs/hardware.md) */

int main(void) {
    /* Debugger attach safety: nothing else runs before this. */
    delay_ms(2000);

    clock_init();
    log_init();
    LOG("tado-bridge skeleton");
    LOG("clock: SYSCLK %lu Hz, APB1 %lu Hz, APB2 %lu Hz, from %s",
        (unsigned long)clock_hz,
        (unsigned long)clock_apb1_hz,
        (unsigned long)clock_apb2_hz,
        clock_source_name(clock_status.source));
    if (clock_status.source != CLOCK_SRC_PLL_HSE) {
        LOG("clock: WARNING HSE PLL not used: %s (HSE wait %lu ms); HSI PLL: %s",
            clock_fault_name(clock_status.hse_fault),
            (unsigned long)clock_status.hse_ready_ms,
            clock_fault_name(clock_status.hsi_fault));
    } else {
        LOG("clock: HSE ready after %lu ms", (unsigned long)clock_status.hse_ready_ms);
    }
    LOG("clock: RCC_CR %08lx PLLCFGR %08lx CFGR %08lx FLASH_ACR %08lx PWR_CR %08lx",
        (unsigned long)RCC_CR,
        (unsigned long)RCC_PLLCFGR,
        (unsigned long)RCC_CFGR,
        (unsigned long)FLASH_ACR,
        (unsigned long)PWR_CR);

    rcc_periph_clock_enable(RCC_GPIOB);
    gpio_mode_setup(LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_PIN);

    for (unsigned n = 0;; n++) {
        gpio_toggle(LED_PORT, LED_PIN);
        LOG("tick %u", n);
        delay_ms(500);
    }
}
