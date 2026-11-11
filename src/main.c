/* Placeholder: startup delay, blink PB0 (router LED, active high), RTT hello.
 * Clock tree is #7, FreeRTOS/LEDs/button/watchdog are #8. */
#include "clock.h"
#include "log.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#define LED_PORT GPIOB
#define LED_PIN GPIO0 /* router LED, drive high = on (docs/hardware.md) */

int main(void) {
    /* Debugger attach safety: nothing else runs before this. */
    delay_ms(2000);

    clock_init();
    log_init();
    LOG("tado-bridge skeleton, core %lu Hz", (unsigned long)clock_hz);

    rcc_periph_clock_enable(RCC_GPIOB);
    gpio_mode_setup(LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_PIN);

    for (unsigned n = 0;; n++) {
        gpio_toggle(LED_PORT, LED_PIN);
        LOG("tick %u", n);
        delay_ms(500);
    }
}
