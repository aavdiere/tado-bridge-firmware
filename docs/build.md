# Build, flash and log

- Clone with `git submodule update --init` (libopencm3 at `vendor/libopencm3`, pinned to `10cfbd76`, the commit `stm-template` uses; FreeRTOS-Kernel at `vendor/FreeRTOS-Kernel`, pinned to tag `V11.1.0`). `git config core.hooksPath .githooks` once per clone.
- `make` builds `build/tado-bridge.elf` (Release, `-Os -flto`, newlib-nano) and builds libopencm3 for STM32F4 on first use. `make size` prints `arm-none-eabi-size`. `BUILD_TYPE=Debug BUILD_DIR=build-debug make` gives a debug build.
- Device `STM32F411CE`, linker script `cmake/stm32f411ce.ld` (512 KB flash, 128 KB RAM).
- `make flash` or `tools/flash.sh [elf]`: `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program <elf> verify reset exit"`. No `st-flash`, no connect-under-reset (no NRST on the test points).
- RTT: `tools/rtt.sh` starts OpenOCD with an RTT server on TCP 19021; connect with `nc localhost 19021`, or without netcat: `bash -c 'exec 3<>/dev/tcp/127.0.0.1/19021; cat <&3'`. `tools/rtt.sh` can be started at any time, also during the firmware's 2 s startup delay: OpenOCD 0.12 searches for the control block only once per `rtt start`, so the script retries every 200 ms for up to 10 s (verified on the bench 2026-11-11 **[V]**: two misses during the delay, then the full boot log). **RTT host notes [V] (2026-11-12):** the 1 KB up buffer fills while no host is attached and later output is dropped (non-blocking). An `ncat` client must use `--recv-only` when stdin is not a terminal, else it closes immediately on stdin EOF. Port 9090 is taken by another service on the bench PC; override with `RTT_PORT`. The firmware has its own minimal RTT block (`src/rtt.c`, up channel 0, 1 KB, non-blocking, ID `SEGGER RTT`), found by scanning RAM.
- Startup: `main()` first drives the two SPI1 chip selects high (`board_cs_early()`, PA4 and PB9 only, #28), then busy-waits 2 s so a debugger can attach. Then `clock_init()` (`src/clock.c`, #7) sets 96 MHz from the 26 MHz crystal, with an HSI fallback, and the boot log reports the source and the RCC/flash registers (`hardware.md`, Clock tree).
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

**Faults.** `src/faults.c` replaces libopencm3's silent handlers for HardFault, MemManage, BusFault and UsageFault; `faults_init()` enables the last three in `SCB_SHCSR`. A naked stub picks MSP or PSP from EXC_RETURN and `fw_fault_report()` (interrupts off, no mutex) logs two lines, then spins until the IWDG resets: `FAULT <name>: PC xxxxxxxx LR xxxxxxxx xPSR xxxxxxxx` and `FAULT <name>: CFSR .. HFSR .. MMFAR .. BFAR ..; waiting for the watchdog`. The log line buffer is 192 bytes, and `-fmacro-prefix-map` makes `__FILE__` repo-relative (no home path in the `.bin`). The IWDG start waits (bounded, about 50 ms) for `IWDG_SR` PVU/RVU to clear before the reload key, so the first window uses reload 1500; on timeout it logs a warning. The newlib heap ends at `_heap_limit` (top of RAM minus 4 KB main stack, `cmake/stm32f411ce.ld`).

**Start `tools/rtt.sh` before testing [V]:** the 1 KB RTT up buffer fills after about 2 minutes with no host attached and then drops new lines (seen on the bench 2026-11-11: early button presses were lost and one heartbeat line was truncated).

**Bench test, expected result after `make flash` and `make rtt`:**

1. The log shows `reset: RCC_CSR ...` (after a flash: `SOFTWARE PIN`, because OpenOCD resets with SYSRESETREQ **[V]**, 2026-11-11; `POR/PDR` after a power cycle), the clock lines, `watchdog: IWDG started`.
2. Power LED (PB1) blinks at 1 Hz and cloud LED (PB5) at 1 Hz. Both continue indefinitely.
3. Press the factory reset button: router LED (PB0) lights while held, RTT prints `button: factory reset (PB10) pressed`/`released`. Press the pair button: pairing LED (PB8) lights, same in RTT.
4. Halting in OpenOCD (`halt`, then `resume`) must not reset the board.

The P1 watchdog test used a temporary hang path that was removed after it passed (results in `prototype-plan.md` "P1 result").

Size (`make size`): text 12108, data 116, bss 34192 bytes (46416 total; 26% of the 128 KB RAM). bss is mostly the 32 KB FreeRTOS heap, so about 93 KB of RAM is left, before stacks beyond the three tasks (the main stack, which interrupts use, comes out of that).

## P2: radio receive-only and flash read-only on SPI1 (#9, #10)

Code: `src/spi1.c`, `src/cc110l.c`, `src/radio.c`, `src/mx25r.c`, `src/timebase.c`; settings and datasheet facts in `hardware.md` ("SPI1: radio and flash") and `radio-runtime.md` ("P2 receive-only implementation"). **This image cannot transmit** (transmit guard in `src/cc110l.c`) and never writes or erases the external flash. It listens on the stock channel 26, which `test-bench.md` rule 3 allows. Needs a `fw-reviewer` verdict before flashing.

The radio task now owns SPI1 and blinks no placeholder LED; the cloud LED (PB5) toggles on each CRC-OK frame instead. Housekeeping asks the radio task for a flash JEDEC ID check every 10 s.

**Bench test, expected result after `make flash` and `make rtt`** (power-cycle first, so the CC110L starts from power-on defaults, `test-bench.md`):

1. Boot log as in P1, then `spi1: SCK 6000000 Hz, mode 0`, `flash: JEDEC ID c22815 (MX25R1635F), status register ..`. Note the status register value; bit 0 (WIP) should be 0.
2. `radio: PARTNUM .. VERSION ..` (datasheet: `00 17`), `radio: calibrated, FSCAL3 .. FSCAL2 .. FSCAL1 ..` without "PLL NOT locked", `radio: RX on channel 26 (868323668 Hz), receive only`.
3. A 60 s line `radio: 60 s: freqest in packet N, none N (stale sample dropped N), sync rssi differs N` with in-packet equal to the frame count and none 0.
3b. Frames from the stock network: `rx: t .. len .. crc ok rssi .. dBm ...` lines and `rx: wake train ...` summaries; the cloud LED flickers with them. The SDR scan saw about 84 CRC-valid frames in 30 s on this channel (`test-bench.md`).
4. Every 60 s a `radio: 60 s: ...` and a `flash: 60 s: JEDEC ID ok 6, mismatch 0 ...` line. Flash mismatches must stay 0 with radio traffic running.
5. Every 10 s the `alive:` line from P1; the watchdog must not fire (`reset:` shows no `IWDG` after a long run).
6. Optional with the SDR: confirm the bridge transmits nothing on 863-873 MHz while it runs.

Record PARTNUM/VERSION, the frame and CRC counts, the RSSI range and the `FREQEST` spread in `radio-runtime.md` and `hardware.md` (offset measurement for #7). After the test, power-cycle the board again so the radio leaves our configuration.

Size (`make size`, Release): text 19664, data 116, bss 34492 bytes.

**Pre-flash review follow-ups (#9, #28).** Bench result of the first P2 image: flash ID `c22815`, PARTNUM `00` / VERSION `17`, 210 of 211 frames CRC OK in 60 s, no errors **[V]** (2026-11-11). The follow-ups from the `fw-reviewer` review, built but **not yet run on hardware**:

- TX-state watch on every path, not only the steady-state loop: after each bring-up step, on every bring-up failure (check, `SIDLE`, check), every 100 ms of the 10 s retry wait, and as the last radio access of every task loop pass. Status byte STATE `111` (TXFIFO_UNDERFLOW) now counts as a TX state too, next to TX and FSTXON (`datasheets/cc110l.txt:1381-1405`); MARCSTATE `0x10` (TXRX_SWITCH) joins `0x12-0x16` (`cc110l.txt:4462-4495`). Details in `src/radio.c` (`tx_watch`).
- Chip selects high before the 2 s startup delay (`hardware.md`, SPI1).
- GDO interrupt storm guard and the wider 10 s health check (`hardware.md`, SPI1).
- A read-back mismatch of the reserved registers `0x20`, `0x29-0x2B` is tagged `(reserved)` in the log line.
- `RADIO_TX_ALLOWED` builds will need `RADIO_RX_CHANNEL != RADIO_PROTECTED_CHANNEL` (`_Static_assert`, inactive while `RADIO_TX_ALLOWED` is 0, which still `#error`s).
- CMake: re-running `cmake` on an existing build directory used to append the linker flags to their own cache entry again, and `ld` then refused the linker script given twice ("appears multiple times"). The toolchain file now sets `CMAKE_EXE_LINKER_FLAGS` instead of appending, and the linker script is a `target_link_options(-T...)` in `cmake/CMakeLists.txt` (CMake 3.13 or newer); the cached flags stay identical across re-runs **[V]** (build PC, CMake 4.4.5).

Size after the follow-ups (`make size`, Release): text 20680, data 116, bss 34512 bytes. After the in-packet `FREQEST` change (#11): text 21404, data 116, bss 34568 bytes.
