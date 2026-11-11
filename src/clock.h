#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* Core clock in Hz. 16 MHz HSI until the clock tree (#7) replaces it. */
extern uint32_t clock_hz;

/* Hook for the clock tree (#7). Called once from main() after the startup
 * delay. The stub keeps the 16 MHz HSI. */
void clock_init(void);

/* Busy-wait using SysTick polling (no interrupt). Valid for any clock_hz. */
void delay_ms(uint32_t ms);

#endif
