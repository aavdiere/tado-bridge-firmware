#ifndef BL_HW_H
#define BL_HW_H

/* Bootloader hardware (#24), src/boot/bl_hw.c. The ports of bootctl.h
 * (internal flash, IWDG) and store.h (external flash, boot state sectors
 * only) are implemented there too. */
#include <stdbool.h>
#include <stdint.h>

/* Power LED codes of the bootloader, after the application's 1-4
 * (docs/hardware.md "LED roles"). */
#define BL_LED_NO_APP 5u /* no valid application and none to put in: SWD recovery */
#define BL_LED_GUARD  6u /* a write outside the bootloader's limits was refused: a bug */

/* IWDG: prescaler /256, reload 4095: 22.3 s at the LSI's fastest 47 kHz,
 * 32.8 s at 32 kHz, 61.7 s at 17 kHz (stm32f411ce.txt:5028,
 * rm0383.txt:23216). Longer than any one step of the bootloader (a 128 KB
 * sector erase takes up to 2 s, stm32f411ce.txt:5320-5330) and than the
 * application's start before its own watchdog_start() (2 s delay plus the
 * clock and peripheral set-up). A candidate that hangs before that still
 * resets, so its start is counted. */
#define BL_IWDG_RELOAD 0xFFFu

/* HSI clock as after reset, SysTick, pins, SPI1. */
void bl_hw_init(void);
/* Start the IWDG (frozen while the debugger halts the core). */
void bl_iwdg_start(void);
/* Release the external flash from deep power-down, wait for an unfinished
 * cycle, check the JEDEC ID. False: do not use the external flash. */
bool bl_flash_init(void);
/* Start the application at FWIMG_APP_BASE: VTOR, MSP, reset vector. */
__attribute__((noreturn)) void bl_jump(void);
/* Show LED code forever, kicking the IWDG. */
__attribute__((noreturn)) void bl_halt(unsigned code);

#endif
