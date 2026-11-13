#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

/* Free-running 1 MHz counter on TIM2 (32-bit, wraps after about 71.6 min).
 * Used for bounded busy-waits in drivers and for radio timestamps. Touches no
 * pin. Call after clock_init(). Safe from tasks and interrupts. */
void     timebase_init(void);
uint32_t timebase_us(void);
/* Busy-wait at least us microseconds. For short driver waits only. */
void delay_us(uint32_t us);

#endif
