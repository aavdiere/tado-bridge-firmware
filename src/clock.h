#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* Clock tree (#7, docs/hardware.md "Clock tree"). Target: 26 MHz HSE crystal
 * -> PLL -> SYSCLK = HCLK = 96 MHz, APB1 = 48 MHz, APB2 = 96 MHz,
 * PLL48CK = 48 MHz. Falls back to an HSI-fed PLL at the same 96 MHz, and as
 * a last resort stays on the 16 MHz HSI. clock_init() never hangs. */

enum clock_source {
    CLOCK_SRC_HSI,     /* 16 MHz HSI, no PLL: both PLL attempts failed */
    CLOCK_SRC_PLL_HSE, /* normal case: 26 MHz crystal -> PLL */
    CLOCK_SRC_PLL_HSI, /* fallback: HSI -> PLL, same frequencies, +-1% or worse */
};

enum clock_fault {
    CLOCK_OK,
    CLOCK_FAULT_NOT_TRIED,
    CLOCK_FAULT_HSE_TIMEOUT, /* HSERDY did not set */
    CLOCK_FAULT_PLL_LOCK,    /* PLLRDY did not set */
    CLOCK_FAULT_VOS,         /* PWR_CSR.VOSRDY did not set */
    CLOCK_FAULT_FLASH_WS,    /* FLASH_ACR.LATENCY read back wrong */
    CLOCK_FAULT_SWITCH,      /* RCC_CFGR.SWS did not report the PLL */
};

struct clock_status {
    enum clock_source source;
    enum clock_fault  hse_fault;    /* why the HSE PLL is not in use; CLOCK_OK if it is */
    enum clock_fault  hsi_fault;    /* HSI PLL fallback result; NOT_TRIED if HSE worked */
    uint32_t          hse_ready_ms; /* ms from HSEON to HSERDY (the timeout if it never set) */
};

/* Core clock (HCLK) in Hz: 16 MHz at reset, 96 MHz after clock_init(). */
extern uint32_t clock_hz;
/* Bus clocks for peripheral drivers. SPI1 runs from APB2, SPI2 from APB1.
 * Timers on an APB with prescaler > 1 run at twice that APB clock
 * (rm0383.txt:4760-4763). */
extern uint32_t            clock_apb1_hz;
extern uint32_t            clock_apb2_hz;
extern struct clock_status clock_status;

/* Call once from main() after the startup delay, before anything that
 * depends on clock_hz. Also updates libopencm3's rcc_ahb/apb1/apb2_frequency.
 * Does not enable the clock security system and does not touch MCO pins. */
void clock_init(void);

const char *clock_source_name(enum clock_source src);
const char *clock_fault_name(enum clock_fault fault);

/* Busy-wait using SysTick polling (no interrupt). Valid for any clock_hz. */
void delay_ms(uint32_t ms);

#endif
