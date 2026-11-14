/* ADC1 channel 3 on PA3 for the entropy pool (#14, docs/design.md "Entropy").
 * Register-level; citations are to docs/datasheets/rm0383.txt (RM) and
 * docs/datasheets/stm32f411ce.txt (DS). */
#include "pa3_adc.h"

#include "timebase.h"

#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#define PA3_PIN     GPIO3
#define PA3_CHANNEL 3u /* ADC1_IN3, DS:2288 */

/* ADCCLK = PCLK2 / 4: 96 MHz / 4 = 24 MHz (clock.c: APB2 = SYSCLK). The
 * maximum is 36 MHz for VDDA 2.4-3.6 V (DS:6506); the bench rail is 3.28 V
 * (docs/test-bench.md). /2 would give 48 MHz, too fast. */
#define ADCPRE_DIV4 (1u << 16) /* ADC_CCR ADCPRE[1:0] = 01, RM:12812-12817 */
#define ADCPRE_MASK (3u << 16)

#define TSTAB_US 10u /* power-up time tSTAB max 3 µs (DS:6541) */
#define EOC_WAIT_US                                                                \
    1000u /* 480 + 12 cycles take 20.5 µs at 24 MHz (RM:11872-11878); the rest is \
           * room for preemption by the radio task and interrupts */

void pa3_adc_on(enum pa3_smp smp) {
    /* PA3 analog (MODER = 11, RM:8089), no pull (PUPDR = 00): high impedance,
     * nothing driven (RM:7921-7928). gpio_mode_setup() changes PA3's bits
     * only. GPIOA's clock is already on (board_init). */
    gpio_mode_setup(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, PA3_PIN);

    rcc_periph_clock_enable(RCC_ADC1); /* RCC_APB2ENR ADC1EN, RM:6145 */
    ADC_CR2(ADC1) = 0;                 /* ADON = 0 while configuring */
    ADC_CCR       = (ADC_CCR & ~ADCPRE_MASK) | ADCPRE_DIV4;
    /* CR1: RES = 00 (12 bit, RM:12247), SCAN = 0 (RM:12297), no interrupts. */
    ADC_CR1(ADC1) = 0;
    /* SMP3[2:0] in ADC_SMPR2 bits 11:9 (RM:12503, 12515-12527). */
    ADC_SMPR2(ADC1) = (uint32_t)smp << (3u * PA3_CHANNEL);
    ADC_SQR1(ADC1)  = 0;           /* L[3:0] = 0: one conversion (RM:12616) */
    ADC_SQR3(ADC1)  = PA3_CHANNEL; /* SQ1[4:0] = 3 (RM:12694) */
    ADC_SR(ADC1)    = 0;
    /* CR2: CONT = 0 single (RM:12454), ALIGN = 0 right (RM:12427), EOCS = 0
     * (RM:12431), no external trigger. ADON wakes it up (RM:11500-11502). */
    ADC_CR2(ADC1) = ADC_CR2_ADON;
    delay_us(TSTAB_US);
}

bool pa3_adc_read(uint16_t *value) {
    ADC_CR2(ADC1) |= ADC_CR2_SWSTART; /* RM:12355-12360, needs ADON = 1 */
    uint32_t t0 = timebase_us();
    while (!(ADC_SR(ADC1) & ADC_SR_EOC)) { /* RM:12207-12210 */
        if (timebase_us() - t0 > EOC_WAIT_US) {
            return false;
        }
    }
    *value = (uint16_t)(ADC_DR(ADC1) & 0x0FFFu); /* reading DR clears EOC */
    return true;
}

void pa3_adc_off(void) {
    ADC_CR2(ADC1) = 0; /* ADON = 0: power down (RM:11503-11504) */
    rcc_periph_clock_disable(RCC_ADC1);
}
