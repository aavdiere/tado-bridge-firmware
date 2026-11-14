#ifndef PA3_ADC_H
#define PA3_ADC_H

#include <stdbool.h>
#include <stdint.h>

/* Raw ADC1 readings of the unconnected pin PA3 (package pin 13, ADC1_IN3,
 * docs/datasheets/stm32f411ce.txt:2288), the entropy source of
 * docs/design.md "Entropy (#26)". Touches only PA3 (analog mode, no pull)
 * and ADC1; never PA13/PA14 or PB6/PB7. Single owner: the net task. */

/* Sampling time selection, ADC_SMPRx SMPx[2:0]: 0 = 3 cycles ... 7 = 480
 * cycles (docs/datasheets/rm0383.txt:12515-12527). */
enum pa3_smp {
    PA3_SMP_3,
    PA3_SMP_15,
    PA3_SMP_28,
    PA3_SMP_56,
    PA3_SMP_84,
    PA3_SMP_112,
    PA3_SMP_144,
    PA3_SMP_480,
};

/* Powers ADC1 up for single software-started 12-bit conversions of channel 3
 * with the given sampling time. ADCCLK = PCLK2 / 4 = 24 MHz. */
void pa3_adc_on(enum pa3_smp smp);

/* One conversion, 0..4095. Returns false if EOC does not come within 1 ms.
 * A conversion takes at most 492 ADCCLK cycles, 20.5 µs at 24 MHz; the rest
 * covers preemption, which made 1 in 10^5 reads miss a 50 µs limit on the
 * bench [V]. */
bool pa3_adc_read(uint16_t *value);

/* Powers ADC1 down (a few µA, rm0383.txt:11503-11504) and stops its clock. */
void pa3_adc_off(void);

#endif
