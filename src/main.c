/* Startup delay, clock, RTT, LEDs and buttons, SPI1 for radio and flash,
 * FreeRTOS tasks, independent watchdog (#6, #7, #8, #9, #10). SPI2, the
 * ENC28J60 and lwIP are set up by the net task (#12, #13). */
#include "app.h"
#include "board.h"
#include "clock.h"
#include "faults.h"
#include "log.h"
#include "spi1.h"
#include "timebase.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/rcc.h>

int main(void) {
    /* Radio CSn and flash CS# high before the delay (#28): otherwise PA4 and
     * PB9 float as inputs for 2 s and either device may see itself selected.
     * Two pins only; see board_cs_early(). */
    board_cs_early();

    /* Debugger attach safety: nothing else runs before this. Polls SysTick,
     * which FreeRTOS takes over at vTaskStartScheduler(); delay_ms() must not
     * be used after that. */
    delay_ms(2000);

    clock_init();
    timebase_init();
    faults_init();
    log_init();
    LOG("tado-bridge P4 #14: radio receive-only, flash read-only, Ethernet with lwIP (DHCP, DNS), "
        "PA3 entropy and CTR_DRBG");
    watchdog_log_reset_cause();
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

    board_init(); /* starts with the radio and flash chip selects high (#28) */
    log_lock_init();
    spi1_init();
    app_create_tasks();

    /* Watchdog last, right before the scheduler: ~2-6 s timeout, kicked only by
     * the housekeeping task. */
    watchdog_start();
    LOG("watchdog: IWDG started, /%d reload %d (2.0-5.6 s), frozen while halted",
        WDG_PRESCALER_DIV,
        WDG_RELOAD);
    vTaskStartScheduler();

    fw_panic("vTaskStartScheduler returned", __FILE__, __LINE__);
}
