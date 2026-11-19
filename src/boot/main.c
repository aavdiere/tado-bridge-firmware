/* Bootloader (#24): docs/design.md "Implementation: bootloader and boot
 * state (#24)". Written once over SWD at 0x08000000 and never updated over
 * the network. Runs on the 16 MHz HSI, with no interrupts, no RTOS and no
 * logging; decides with bootctl_run() (src/boot/bootctl.c), then starts the
 * application at 0x08010000 or halts with an LED code.
 *
 * No attach delay is needed for the debugger: OpenOCD halts the core at the
 * reset vector after a reset (`reset halt`), before any of this runs, and
 * nothing here touches the SWD pins or enters a low-power mode
 * (docs/design.md "Recovery over SWD"). */
#include "bl_hw.h"
#include "bootctl.h"

int main(void) {
    bl_hw_init();
    bl_iwdg_start();
    bool                  ext_ok = bl_flash_init();
    struct bootctl_report rep;
    if (bootctl_run(ext_ok, &rep) == BOOTCTL_JUMP) {
        bl_jump();
    }
    bl_halt(BL_LED_NO_APP);
}
