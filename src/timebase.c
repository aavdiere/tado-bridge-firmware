/* 1 MHz free-running counter on TIM2. TIM2 is a 32-bit timer
 * (rm0383.txt:17128) on APB1. Its kernel clock is APB1 x2 when the APB1
 * prescaler is not 1, else APB1 (rm0383.txt:4758-4763): 96 MHz on the PLL
 * paths (APB1 48 MHz, /2), 16 MHz on the HSI-only fallback (APB1 /1). */
#include "timebase.h"

#include "clock.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>

void timebase_init(void) {
    uint32_t timclk = (clock_apb1_hz == clock_hz) ? clock_apb1_hz : 2u * clock_apb1_hz;

    rcc_periph_clock_enable(RCC_TIM2);
    TIM_CR1(TIM2) = 0;
    TIM_PSC(TIM2) = timclk / 1000000u - 1u; /* 95 at 96 MHz, 15 at 16 MHz */
    TIM_ARR(TIM2) = 0xFFFFFFFFu;
    TIM_EGR(TIM2) = TIM_EGR_UG; /* load PSC now, not at the first overflow */
    TIM_SR(TIM2)  = 0;
    TIM_CR1(TIM2) = TIM_CR1_CEN;
}

uint32_t timebase_us(void) {
    return TIM_CNT(TIM2);
}

void delay_us(uint32_t us) {
    uint32_t start = timebase_us();
    /* +1: the counter may tick right after start was read. */
    while ((uint32_t)(timebase_us() - start) < us + 1u) {
    }
}
