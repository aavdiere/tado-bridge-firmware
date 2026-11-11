# Build, flash and log

- Clone with `git submodule update --init` (libopencm3 at `vendor/libopencm3`, pinned to `10cfbd76`, the commit `stm-template` uses). `git config core.hooksPath .githooks` once per clone.
- `make` builds `build/tado-bridge.elf` (Release, `-Os -flto`, newlib-nano) and builds libopencm3 for STM32F4 on first use. `make size` prints `arm-none-eabi-size`. `BUILD_TYPE=Debug BUILD_DIR=build-debug make` gives a debug build.
- Device `STM32F411CE`, linker script `cmake/stm32f411ce.ld` (512 KB flash, 128 KB RAM).
- `make flash` or `tools/flash.sh [elf]`: `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program <elf> verify reset exit"`. No `st-flash`, no connect-under-reset (no NRST on the test points).
- RTT: `tools/rtt.sh` starts OpenOCD with an RTT server on TCP 9090; connect with `nc localhost 9090`. The firmware has its own minimal RTT block (`src/rtt.c`, up channel 0, 1 KB, non-blocking, ID `SEGGER RTT`), found by scanning RAM.
- Startup: `main()` busy-waits 2 s first so a debugger can attach. Then `clock_init()` (`src/clock.c`, #7) sets 96 MHz from the 26 MHz crystal, with an HSI fallback, and the boot log reports the source and the RCC/flash registers (`hardware.md`, Clock tree).
- VS Code: `.vscode/launch.json` has Cortex-Debug launch and attach configs for OpenOCD with RTT.
