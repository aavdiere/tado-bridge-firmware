#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Independent watchdog (LSI, 17-47 kHz, 32 kHz typical:
 * docs/datasheets/stm32f411ce.txt:5028). Prescaler /64, reload 1500: timeout
 * 2.0 s (47 kHz) .. 3.0 s (32 kHz) .. 5.6 s (17 kHz). */
#define WDG_PRESCALER_DIV 64
#define WDG_RELOAD        1500

/* Task check-in bits, set by each task at least once per kick window. */
enum wdg_task { WDG_RADIO = 1u << 0, WDG_NET = 1u << 1, WDG_HOUSEKEEPING = 1u << 2 };
#define WDG_ALL (WDG_RADIO | WDG_NET | WDG_HOUSEKEEPING)

/* Logs the reset cause from RCC_CSR and clears the flags (RMVF). Call after
 * log_init(). */
void watchdog_log_reset_cause(void);

/* Sets DBGMCU_APB1_FZ.DBG_IWDG_STOP (so a halt in the debugger does not
 * reset the board) and starts the IWDG. It cannot be stopped afterwards. */
void watchdog_start(void);

/* Atomic; any task. */
void watchdog_checkin(enum wdg_task who);

/* Housekeeping only: kick the IWDG if every task in WDG_ALL checked in since
 * the last call, then clear the flags. Returns the missing bits (0 = kicked). */
uint32_t watchdog_kick_if_alive(void);

#endif
