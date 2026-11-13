/* Clock tree for the IB01 (#7). Register sequence follows RM0383 Rev 4
 * (docs/datasheets/rm0383.txt) and the limits of the STM32F411CE datasheet
 * (docs/datasheets/stm32f411ce.txt). libopencm3's rcc_clock_setup_pll() is
 * not used because it waits forever for HSERDY and PLLRDY. */
#include "clock.h"
#include "log.h"

#include <stdbool.h>

#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/rcc.h>

/* --- Frequency plan ------------------------------------------------------
 * f_VCO = f_in / PLLM * PLLN, SYSCLK = f_VCO / PLLP, PLL48CK = f_VCO / PLLQ
 * (rm0383.txt:5232-5234).
 *
 * 96 MHz rather than 100 MHz: VCO 192 MHz gives SYSCLK 96 MHz with PLLP = 2
 * and exactly 48 MHz on PLL48CK with PLLQ = 4. 100 MHz (VCO 200 MHz) is also
 * reachable but its PLL48CK would be 50 MHz, APB1 would sit exactly at its
 * 50 MHz limit, and it needs 4 wait states below 2.7 V where 96 MHz needs 3
 * (rm0383.txt:2067-2068). The same PLLN/P/Q work from the 16 MHz HSI with
 * PLLM = 8, so the fallback runs at the same frequencies. */
#define HSE_HZ    26000000u /* crystal, docs/hardware.md (Components) */
#define HSI_HZ    16000000u /* stm32f411ce.txt:4968 */
#define PLLM_HSE  13u       /* 26 MHz / 13 = 2 MHz */
#define PLLM_HSI  8u        /* 16 MHz / 8 = 2 MHz */
#define PLLN      96u
#define PLLP      2u
#define PLLQ      4u
#define VCO_IN_HZ 2000000u
#define VCO_HZ    (VCO_IN_HZ * PLLN) /* 192 MHz */
#define SYSCLK_HZ (VCO_HZ / PLLP)    /* 96 MHz */
#define PLL48_HZ  (VCO_HZ / PLLQ)    /* 48 MHz */
#define APB1_HZ   (SYSCLK_HZ / 2)    /* 48 MHz, PPRE1 = /2 */
#define APB2_HZ   SYSCLK_HZ          /* 96 MHz, PPRE2 = /1 */
/* 90 < HCLK <= 100 MHz at 2.7-3.6 V, and 72 < HCLK <= 96 MHz at 2.4-2.7 V:
 * both need 3 WS (rm0383.txt:2067). Bench VDD is 3.20 V (hardware.md P0). */
#define FLASH_WS 3u

/* PLLM: 2..63, VCO input 1..2 MHz, 2 MHz recommended (rm0383.txt:5313-5316);
 * datasheet fPLL_IN 0.95..2.10 MHz (stm32f411ce.txt:5091). */
_Static_assert(HSE_HZ % PLLM_HSE == 0 && HSE_HZ / PLLM_HSE == VCO_IN_HZ, "HSE PLLM");
_Static_assert(HSI_HZ % PLLM_HSI == 0 && HSI_HZ / PLLM_HSI == VCO_IN_HZ, "HSI PLLM");
_Static_assert(PLLM_HSE >= 2 && PLLM_HSE <= 63 && PLLM_HSI >= 2 && PLLM_HSI <= 63, "PLLM range");
_Static_assert(VCO_IN_HZ >= 1000000u && VCO_IN_HZ <= 2000000u, "VCO input 1-2 MHz");
/* PLLN: 50..432 (rm0383.txt:5293), VCO output 100..432 MHz (rm0383.txt:5290-5291,
 * stm32f411ce.txt:5096). */
_Static_assert(PLLN >= 50 && PLLN <= 432, "PLLN range");
_Static_assert(VCO_HZ >= 100000000u && VCO_HZ <= 432000000u, "VCO output 100-432 MHz");
/* PLLP: 2, 4, 6 or 8; output <= 100 MHz (rm0383.txt:5280-5285); datasheet
 * fPLL_OUT 24..100 MHz (stm32f411ce.txt:5092). */
_Static_assert(PLLP == 2 || PLLP == 4 || PLLP == 6 || PLLP == 8, "PLLP value");
_Static_assert(SYSCLK_HZ >= 24000000u && SYSCLK_HZ <= 100000000u, "SYSCLK 24-100 MHz");
/* PLLQ: 2..15 (rm0383.txt:5253), USB needs 48 MHz (rm0383.txt:5251),
 * fPLL48_OUT <= 75 MHz (stm32f411ce.txt:5094). */
_Static_assert(PLLQ >= 2 && PLLQ <= 15, "PLLQ range");
_Static_assert(PLL48_HZ == 48000000u, "PLL48CK 48 MHz");
/* fHCLK <= 100 MHz in scale 1, fPCLK1 <= 50 MHz, fPCLK2 <= 100 MHz
 * (stm32f411ce.txt:3660-3670, rm0383.txt:5421,5431). Scale 1 needed above
 * 84 MHz (rm0383.txt:4254-4263). */
_Static_assert(SYSCLK_HZ > 84000000u, "scale 1 is needed and set");
_Static_assert(APB1_HZ <= 50000000u, "APB1 <= 50 MHz");
_Static_assert(APB2_HZ <= 100000000u, "APB2 <= 100 MHz");

/* --- Timeouts --------------------------------------------------------------
 * tSU(HSE) is 2 ms typical with no maximum given (stm32f411ce.txt:4859-4862);
 * 100 ms is 50x that. tLOCK is at most 300 us (stm32f411ce.txt:5097-5099).
 * RM0383 gives no time for VOSRDY or the clock switch; 5 ms is generous. */
#define HSE_TIMEOUT_MS    100u
#define PLL_TIMEOUT_MS    5u
#define VOS_TIMEOUT_MS    5u
#define SWITCH_TIMEOUT_MS 5u

/* RCC_PLLCFGR reserved bits 31:28, 23, 21:18 and 15 must be kept at their
 * reset value (rm0383.txt:5242,5247,5261,5267; reset value 0x24003010,
 * rm0383.txt:5229). */
#define PLLCFGR_RESERVED 0xF0BC8000u

#define CFGR_SWS(v) (((v) >> RCC_CFGR_SWS_SHIFT) & RCC_CFGR_SWS_MASK)

uint32_t            clock_hz      = HSI_HZ; /* HSI at reset (rm0383.txt:2073) */
uint32_t            clock_apb1_hz = HSI_HZ;
uint32_t            clock_apb2_hz = HSI_HZ;
struct clock_status clock_status  = {
    .source    = CLOCK_SRC_HSI,
    .hse_fault = CLOCK_FAULT_NOT_TRIED,
    .hsi_fault = CLOCK_FAULT_NOT_TRIED,
};

/* Poll until (*reg & mask) == want, at most timeout_ms. SysTick counts HCLK,
 * so the timeout is right while clock_hz matches the running clock. */
static bool wait_bits(volatile uint32_t *reg,
                      uint32_t           mask,
                      uint32_t           want,
                      uint32_t           timeout_ms,
                      uint32_t          *elapsed_ms) {
    uint32_t ms = 0;
    bool     ok = false;

    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(clock_hz / 1000 - 1);
    systick_clear();
    systick_counter_enable();
    for (;;) {
        if ((*reg & mask) == want) {
            ok = true;
            break;
        }
        if (systick_get_countflag() && ++ms >= timeout_ms) {
            ok = (*reg & mask) == want;
            break;
        }
    }
    systick_counter_disable();
    if (elapsed_ms) {
        *elapsed_ms = ms;
    }
    return ok;
}

static void pll_off(void) {
    RCC_CR &= ~RCC_CR_PLLON;
    (void)wait_bits(&RCC_CR, RCC_CR_PLLRDY, 0, PLL_TIMEOUT_MS, 0);
}

static void set_prescalers(uint32_t ppre1) {
    rcc_set_hpre(RCC_CFGR_HPRE_DIV_NONE);
    rcc_set_ppre2(RCC_CFGR_PPRE_DIV_NONE);
    rcc_set_ppre1(ppre1);
}

/* Configure, lock and switch to the PLL. Entered running on HSI with the PLL
 * off and the regulator scale already set. On failure, returns with SYSCLK
 * on HSI, the PLL off and prescalers at /1. Flash latency may be left at
 * 3 WS, which is valid (only slower) at 16 MHz. */
static enum clock_fault pll_start(bool from_hse) {
    uint32_t pllm = from_hse ? PLLM_HSE : PLLM_HSI;
    uint32_t cfg  = RCC_PLLCFGR & PLLCFGR_RESERVED;

    /* PLL fields are writable only with the PLL off (rm0383.txt:5250,5279,5288,5311-5312). */
    cfg |= (from_hse ? RCC_PLLCFGR_PLLSRC : 0)
           | ((PLLQ & RCC_PLLCFGR_PLLQ_MASK) << RCC_PLLCFGR_PLLQ_SHIFT)
           | (((PLLP / 2 - 1) & RCC_PLLCFGR_PLLP_MASK) << RCC_PLLCFGR_PLLP_SHIFT)
           | ((PLLN & RCC_PLLCFGR_PLLN_MASK) << RCC_PLLCFGR_PLLN_SHIFT)
           | ((pllm & RCC_PLLCFGR_PLLM_MASK) << RCC_PLLCFGR_PLLM_SHIFT);
    RCC_PLLCFGR = cfg; /* word write: PLLN needs half-word or word (rm0383.txt:5288-5289) */

    RCC_CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC_CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY, PLL_TIMEOUT_MS, 0)) {
        pll_off();
        return CLOCK_FAULT_PLL_LOCK;
    }

    /* VOS takes effect once the PLL is on (rm0383.txt:3561-3562). Do not run
     * above 84 MHz until the regulator reports scale 1 ready (rm0383.txt:4363). */
    if (!wait_bits(&PWR_CSR, PWR_CSR_VOSRDY, PWR_CSR_VOSRDY, VOS_TIMEOUT_MS, 0)) {
        pll_off();
        return CLOCK_FAULT_VOS;
    }

    /* Raise flash latency first and read it back (rm0383.txt:2078-2082).
     * Prefetch is allowed above 2.1 V (rm0383.txt:2051). ACR bits:
     * DCEN 10, ICEN 9, PRFTEN 8, LATENCY 3:0 (rm0383.txt:2809-2819). */
    FLASH_ACR = (FLASH_ACR & ~(FLASH_ACR_LATENCY_MASK << FLASH_ACR_LATENCY_SHIFT))
                | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN
                | (FLASH_WS << FLASH_ACR_LATENCY_SHIFT);
    if (((FLASH_ACR >> FLASH_ACR_LATENCY_SHIFT) & FLASH_ACR_LATENCY_MASK) != FLASH_WS) {
        pll_off();
        return CLOCK_FAULT_FLASH_WS;
    }

    /* APB1 /2 before the switch so it never exceeds 50 MHz (rm0383.txt:5431). */
    set_prescalers(RCC_CFGR_PPRE_DIV_2);
    rcc_set_sysclk_source(RCC_CFGR_SW_PLL);
    if (!wait_bits(&RCC_CFGR,
                   RCC_CFGR_SWS_MASK << RCC_CFGR_SWS_SHIFT,
                   RCC_CFGR_SWS_PLL << RCC_CFGR_SWS_SHIFT,
                   SWITCH_TIMEOUT_MS,
                   0)) {
        rcc_set_sysclk_source(RCC_CFGR_SW_HSI);
        (void)wait_bits(&RCC_CFGR,
                        RCC_CFGR_SWS_MASK << RCC_CFGR_SWS_SHIFT,
                        RCC_CFGR_SWS_HSI << RCC_CFGR_SWS_SHIFT,
                        SWITCH_TIMEOUT_MS,
                        0);
        set_prescalers(RCC_CFGR_PPRE_DIV_NONE);
        pll_off();
        return CLOCK_FAULT_SWITCH;
    }
    return CLOCK_OK;
}

void clock_init(void) {
    /* After reset SYSCLK is HSI with the PLL off (rm0383.txt:2073). Enforce
     * it anyway, so the PLL and VOS can be written (rm0383.txt:4258). HSI
     * starts in at most 4 us (stm32f411ce.txt:4979). */
    RCC_CR |= RCC_CR_HSION;
    (void)wait_bits(&RCC_CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY, 1, 0);
    if (CFGR_SWS(RCC_CFGR) != RCC_CFGR_SWS_HSI) {
        rcc_set_sysclk_source(RCC_CFGR_SW_HSI);
        (void)wait_bits(&RCC_CFGR,
                        RCC_CFGR_SWS_MASK << RCC_CFGR_SWS_SHIFT,
                        RCC_CFGR_SWS_HSI << RCC_CFGR_SWS_SHIFT,
                        SWITCH_TIMEOUT_MS,
                        0);
    }
    set_prescalers(RCC_CFGR_PPRE_DIV_NONE);
    pll_off();

    /* Scale 1 for HCLK above 84 MHz; VOS is writable only with the PLL off
     * (rm0383.txt:4254-4263). PWR needs its bus clock first (rm0383.txt:6012). */
    rcc_periph_clock_enable(RCC_PWR);
    pwr_set_vos_scale(PWR_SCALE1);

    /* HSE in crystal mode: HSEBYP = 0, writable only while HSE is off
     * (rm0383.txt:5183-5186, crystal per docs/hardware.md). The clock
     * security system stays off (CSSON = 0): on an HSE failure it would raise
     * an NMI that must clear CSSC or repeat forever (rm0383.txt:4951-4955).
     * What the PLL does if HSE is lost mid-run is not documented; the plan
     * is that the LSI-clocked watchdog (#8) resets a stalled core and the
     * next boot takes the HSI fallback below [I]. */
    if (!(RCC_CR & RCC_CR_HSEON)) {
        RCC_CR &= ~RCC_CR_HSEBYP;
    }
    RCC_CR |= RCC_CR_HSEON;
    if (!wait_bits(
            &RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY, HSE_TIMEOUT_MS, &clock_status.hse_ready_ms)) {
        clock_status.hse_fault = CLOCK_FAULT_HSE_TIMEOUT;
    } else {
        clock_status.hse_fault = pll_start(true);
    }

    if (clock_status.hse_fault == CLOCK_OK) {
        clock_status.source = CLOCK_SRC_PLL_HSE;
    } else {
        /* PLLSRC is writable with the PLL off (rm0383.txt:5263-5264). */
        RCC_CR &= ~RCC_CR_HSEON;
        clock_status.hsi_fault = pll_start(false);
        clock_status.source =
            clock_status.hsi_fault == CLOCK_OK ? CLOCK_SRC_PLL_HSI : CLOCK_SRC_HSI;
    }

    /* HSI stays on: it costs about 80 uA at most (stm32f411ce.txt:4982). */
    if (clock_status.source == CLOCK_SRC_HSI) {
        clock_hz      = HSI_HZ;
        clock_apb1_hz = HSI_HZ;
        clock_apb2_hz = HSI_HZ;
    } else {
        clock_hz      = SYSCLK_HZ;
        clock_apb1_hz = APB1_HZ;
        clock_apb2_hz = APB2_HZ;
    }
    rcc_ahb_frequency  = clock_hz;
    rcc_apb1_frequency = clock_apb1_hz;
    rcc_apb2_frequency = clock_apb2_hz;
}

const char *clock_source_name(enum clock_source src) {
    switch (src) {
        case CLOCK_SRC_PLL_HSE:
            return "HSE 26 MHz crystal + PLL";
        case CLOCK_SRC_PLL_HSI:
            return "HSI + PLL (fallback)";
        case CLOCK_SRC_HSI:
            return "HSI 16 MHz, no PLL (fallback)";
    }
    return "?";
}

const char *clock_fault_name(enum clock_fault fault) {
    switch (fault) {
        case CLOCK_OK:
            return "ok";
        case CLOCK_FAULT_NOT_TRIED:
            return "not tried";
        case CLOCK_FAULT_HSE_TIMEOUT:
            return "HSE did not start";
        case CLOCK_FAULT_PLL_LOCK:
            return "PLL did not lock";
        case CLOCK_FAULT_VOS:
            return "regulator scale 1 not ready";
        case CLOCK_FAULT_FLASH_WS:
            return "flash latency not accepted";
        case CLOCK_FAULT_SWITCH:
            return "SYSCLK switch to PLL failed";
    }
    return "?";
}

void delay_ms(uint32_t ms) {
    /* Once FreeRTOS owns SysTick (interrupt enabled), polling it would stop the
     * tick. Tasks must use vTaskDelay; treat any use here as a bug. */
    if ((STK_CSR & STK_CSR_TICKINT)) {
        fw_panic("delay_ms after the scheduler started", __FILE__, __LINE__);
    }
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(clock_hz / 1000 - 1); /* 95999 at 96 MHz, fits 24 bits */
    systick_clear();
    systick_counter_enable();
    while (ms--) {
        while (!systick_get_countflag()) {
        }
    }
    systick_counter_disable();
}
