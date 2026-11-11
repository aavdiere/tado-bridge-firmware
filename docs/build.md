# Build, flash and log

- Clone with `git submodule update --init` (libopencm3 at `vendor/libopencm3`, pinned to `10cfbd76`, the commit `stm-template` uses; FreeRTOS-Kernel at `vendor/FreeRTOS-Kernel`, pinned to tag `V11.1.0`). `git config core.hooksPath .githooks` once per clone.
- `make` builds `build/tado-bridge.elf` (Release, `-Os -flto`, newlib-nano) and builds libopencm3 for STM32F4 on first use. `make size` prints `arm-none-eabi-size`. `BUILD_TYPE=Debug BUILD_DIR=build-debug make` gives a debug build.
- Device `STM32F411CE`, linker script `cmake/stm32f411ce.ld` (512 KB flash, 128 KB RAM).
- `make flash` or `tools/flash.sh [elf]`: `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program <elf> verify reset exit"`. No `st-flash`, no connect-under-reset (no NRST on the test points).
- RTT: `tools/rtt.sh` starts OpenOCD with an RTT server on TCP 9090; connect with `nc localhost 9090`. The firmware has its own minimal RTT block (`src/rtt.c`, up channel 0, 1 KB, non-blocking, ID `SEGGER RTT`), found by scanning RAM.
- Startup: `main()` busy-waits 2 s first so a debugger can attach. Then `clock_init()` (`src/clock.c`, #7) sets 96 MHz from the 26 MHz crystal, with an HSI fallback, and the boot log reports the source and the RCC/flash registers (`hardware.md`, Clock tree).
- VS Code: `.vscode/launch.json` has Cortex-Debug launch and attach configs for OpenOCD with RTT.

## P1: FreeRTOS, LEDs, buttons, watchdog (#8)

**RTOS.** FreeRTOS V11.1.0, port `GCC/ARM_CM4F`, `heap_4`, config in `src/FreeRTOSConfig.h`. Tick 1 kHz, preemptive, stack overflow check 2, `configASSERT` and the malloc-failed and stack-overflow hooks log over RTT (`PANIC: ...`), then spin with interrupts off until the watchdog resets the board. SVC, PendSV and SysTick are mapped to libopencm3's `sv_call_handler`, `pend_sv_handler` and `sys_tick_handler` by `#define`s in the config. FreeRTOS heap: **32 KB, provisional** (`configTOTAL_HEAP_SIZE`, for the #17 RAM budget; TLS in P4 will need a revisit). Interrupts that call `...FromISR` functions (GDO0/GDO2, P2) need a numeric priority of 5 or more.

**SysTick.** `delay_ms()` polls SysTick and is only for the 2 s startup delay and `clock_init()`, before `vTaskStartScheduler()`. After that SysTick belongs to FreeRTOS; `delay_ms()` then calls `fw_panic()`. Tasks use `vTaskDelay`.

**Heap and malloc.** The firmware code does not call `malloc`; all allocation goes through FreeRTOS `heap_4`. `_sbrk` in `src/syscalls.c` stays because newlib-nano's printf internals link `_malloc_r`; that heap is the RAM between `end` and the stack limit and is unused in practice **[I]** (not proven; `log_printf` is serialised by a mutex, so newlib's lock hooks, which are no-ops, are not a concern). Newlib's reentrancy (`errno` per task) is off.

**Tasks** (`src/app.c`; priorities from `design.md`, higher number = higher priority):

| Task | Priority | Stack | P1 job |
| :--- | --: | --: | :--- |
| `radio` | 3 | 2 KB | empty; cloud LED (PB5) blinks at 1 Hz from it, as a placeholder |
| `net` | 2 | 2 KB | empty; **heartbeat**: power LED (PB1) blinks at 1 Hz |
| `housekeep` | 1 | 2 KB | polls both buttons every 10 ms (3 samples = 30 ms debounce), drives LEDs, logs button events, kicks the watchdog, logs heap and stack headroom every 10 s |

**LEDs and buttons** (pins from `hardware.md`): router LED PB0 mirrors the factory reset button (PB10, active low, pull-up); pairing LED PB8 mirrors the pair button (PC15, active high, pull-down). RTT logs `button: ... pressed/released`. The factory reset button does nothing destructive.

**Watchdog.** IWDG on the LSI (17-47 kHz, typical 32 kHz), prescaler /64, reload 1500: **timeout 2.0 s to 5.6 s, 3.0 s typical** (`datasheets/stm32f411ce.txt:5028`). Started in `main()` after the startup delay, `clock_init()` and task creation, right before the scheduler. Only `housekeep` kicks it, every 500 ms, and only if the radio, net and housekeeping tasks each checked in (`watchdog_checkin()`, an atomic flag per task) during that window. Otherwise it logs `watchdog: not kicked, missing check-in mask N` and the board resets 2-6 s later. The reset cause (`IWDG`, `PIN`, `POR/PDR`, `SOFTWARE`, `BOR`...) is logged from `RCC_CSR` at every boot and the flags are cleared (`RMVF`). `main()` sets `DBGMCU_APB1_FZ.DBG_IWDG_STOP` (bit 12, `rm0383.txt:46197`) so the watchdog stops counting while OpenOCD halts the core; the register survives system resets but not a power cycle, and the firmware sets it on every boot. This freeze only applies to a halted core: a running core that has hung is still reset.

**Bench test, expected result after `make flash` and `make rtt`:**

1. The log shows `reset: RCC_CSR ...` (after a flash: `PIN`, or `POR/PDR` after a power cycle), the clock lines, `watchdog: IWDG started`.
2. Power LED (PB1) blinks at 1 Hz and cloud LED (PB5) at 1 Hz. Both continue indefinitely.
3. Press the factory reset button: router LED (PB0) lights while held, RTT prints `button: factory reset (PB10) pressed`/`released`. Press the pair button: pairing LED (PB8) lights, same in RTT.
4. **Deliberate hang** (compiled in by default for P1; `make clean; make HANG_TEST=0` removes it; the setting is cached in the build directory until the next `make clean`):
   - Hold the **factory reset button for more than 5 s**: RTT prints `HANG TEST: net task stops checking in`, the power LED freezes, the next watchdog check logs `not kicked, missing check-in mask 2`, and the board resets 2-6 s later (RTT then shows `reset: ... IWDG`). Tests the check-in logic with all tasks still scheduling.
   - Hold **factory reset and pair together for more than 5 s**: the radio task spins at a higher priority than housekeeping, which starves; all LEDs stop and the IWDG resets the board. Tests a real hang.
   - The test fires once per boot. Release the buttons before the reset to avoid retriggering on the next boot (the hold timer restarts from 0 at boot, so a button still held gives another 5 s).
5. Halting in OpenOCD (`halt`, then `resume`) must not reset the board.

Size with `HANG_TEST=1` (`make size`): text 12108, data 116, bss 34192 bytes (46416 total; 26% of the 128 KB RAM). bss is mostly the 32 KB FreeRTOS heap, so about 93 KB of RAM is left, before stacks beyond the three tasks (the main stack, which interrupts use, comes out of that).
