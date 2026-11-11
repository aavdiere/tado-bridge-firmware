#include "watchdog.h"

#include "log.h"

#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>

#define DBGMCU_APB1_FZ      (*(volatile uint32_t *)0xE0042008u) /* rm0383.txt:46197 */
#define DBG_IWDG_STOP       (1u << 12)

static volatile uint32_t alive;

void watchdog_log_reset_cause(void) {
    uint32_t csr = RCC_CSR;
    LOG("reset: RCC_CSR %08lx:%s%s%s%s%s%s%s", (unsigned long)csr,
        (csr & RCC_CSR_IWDGRSTF) ? " IWDG" : "", (csr & RCC_CSR_WWDGRSTF) ? " WWDG" : "",
        (csr & RCC_CSR_SFTRSTF) ? " SOFTWARE" : "", (csr & RCC_CSR_PORRSTF) ? " POR/PDR" : "",
        (csr & RCC_CSR_PINRSTF) ? " PIN" : "", (csr & RCC_CSR_BORRSTF) ? " BOR" : "",
        (csr & RCC_CSR_LPWRRSTF) ? " LOWPOWER" : "");
    RCC_CSR |= RCC_CSR_RMVF;
}

void watchdog_start(void) {
    DBGMCU_APB1_FZ |= DBG_IWDG_STOP;
    IWDG_KR = IWDG_KR_START;
    IWDG_KR = IWDG_KR_UNLOCK;
    while (IWDG_SR & IWDG_SR_PVU) {
    }
    IWDG_PR = IWDG_PR_DIV64;
    while (IWDG_SR & IWDG_SR_RVU) {
    }
    IWDG_RLR = WDG_RELOAD;
    IWDG_KR = IWDG_KR_RESET;
}

void watchdog_checkin(enum wdg_task who) {
    __atomic_fetch_or(&alive, (uint32_t)who, __ATOMIC_RELAXED);
}

uint32_t watchdog_kick_if_alive(void) {
    uint32_t seen = __atomic_exchange_n(&alive, 0, __ATOMIC_RELAXED);
    uint32_t missing = WDG_ALL & ~seen;
    if (missing == 0) {
        IWDG_KR = IWDG_KR_RESET;
    }
    return missing;
}
