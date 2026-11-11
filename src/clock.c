#include "clock.h"

#include <libopencm3/cm3/systick.h>

uint32_t clock_hz = 16000000; /* HSI at reset */

/* TODO(#7): 26 MHz crystal to ~96-100 MHz PLL. Update clock_hz here. */
void clock_init(void) {
}

void delay_ms(uint32_t ms) {
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(clock_hz / 1000 - 1);
    systick_clear();
    systick_counter_enable();
    while (ms--) {
        while (!systick_get_countflag()) {
        }
    }
    systick_counter_disable();
}
