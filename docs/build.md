# Build, flash and log

- Clone with `git submodule update --init` (libopencm3 at `vendor/libopencm3`, pinned to `10cfbd76`, the commit `stm-template` uses; FreeRTOS-Kernel at `vendor/FreeRTOS-Kernel`, pinned to tag `V11.1.0`; lwIP at `vendor/lwip`, pinned to tag `STABLE-2_2_1_RELEASE`, commit `77dcd25a`). `git config core.hooksPath .githooks` once per clone: the pre-commit hook formats staged `src/` files with clang-format and runs `tools/check-no-firmware.sh`. `make format` formats all of `src/` (clang-format 23.1.4 on the bench PC; 23.1.5 since 2026-11-14, which left the existing files unchanged).
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
3. Frames from the stock network: `rx: t .. len .. crc ok rssi .. dBm ...` lines and `rx: wake train ...` summaries; the cloud LED flickers with them. The SDR scan saw about 84 CRC-valid frames in 30 s on this channel (`test-bench.md`).
4. Every 60 s a `flash: 60 s: JEDEC ID ok 6, mismatch 0 ...` line and these radio lines. Flash mismatches must stay 0 with radio traffic running. The radio line `radio: 60 s: freqest in packet N, none N (stale sample dropped N), sync rssi differs N` (`src/radio.c:936`) reports the in-packet `FREQEST` sampling (f1f6b3a). Expect in packet equal to the frame count, none 0, stale sample dropped 0 and sync rssi differs 0 **[D]**; the 2026-11-12 final image gave exactly that (`radio-runtime.md:97`) **[V]**. A non-zero none or stale count means a sample was missed or outlived its frame; non-zero sync rssi differs means the RSSI frozen at sync is not the appended RSSI byte.
5. Every 10 s the `alive:` line from P1; the watchdog must not fire (`reset:` shows no `IWDG` after a long run).
6. Optional with the SDR: confirm the bridge transmits nothing on 863-873 MHz while it runs.

Record PARTNUM/VERSION, the frame and CRC counts, the RSSI range and the `FREQEST` spread in `radio-runtime.md` and `hardware.md` (offset measurement for #7). After the test, power-cycle the board again so the radio leaves our configuration. The owner power-cycled the board after the 2026-11-12 runs **[V]** (owner report to the main session, no log).

Size (`make size`, Release): text 19664, data 116, bss 34492 bytes.

**Pre-flash review follow-ups (#9, #28).** Bench result of the first P2 image: flash ID `c22815`, PARTNUM `00` / VERSION `17`, 210 of 211 frames CRC OK in 60 s, no errors **[V]** (2026-11-11). The follow-ups from the `fw-reviewer` review, built but **not yet run on hardware**:

- TX-state watch on every path, not only the steady-state loop: after each bring-up step, on every bring-up failure (check, `SIDLE`, check), every 100 ms of the 10 s retry wait, and as the last radio access of every task loop pass. Status byte STATE `111` (TXFIFO_UNDERFLOW) now counts as a TX state too, next to TX and FSTXON (`datasheets/cc110l.txt:1381-1405`); MARCSTATE `0x10` (TXRX_SWITCH) joins `0x12-0x16` (`cc110l.txt:4462-4495`). Details in `src/radio.c` (`tx_watch`).
- Chip selects high before the 2 s startup delay (`hardware.md`, SPI1).
- GDO interrupt storm guard and the wider 10 s health check (`hardware.md`, SPI1).
- A read-back mismatch of the reserved registers `0x20`, `0x29-0x2B` is tagged `(reserved)` in the log line.
- `RADIO_TX_ALLOWED` builds will need `RADIO_RX_CHANNEL != RADIO_PROTECTED_CHANNEL` (`_Static_assert`, inactive while `RADIO_TX_ALLOWED` is 0, which still `#error`s).
- CMake: re-running `cmake` on an existing build directory used to append the linker flags to their own cache entry again, and `ld` then refused the linker script given twice ("appears multiple times"). The toolchain file now sets `CMAKE_EXE_LINKER_FLAGS` instead of appending, and the linker script is a `target_link_options(-T...)` in `cmake/CMakeLists.txt` (CMake 3.13 or newer); the cached flags stay identical across re-runs **[V]** (build PC, CMake 4.4.5).

Size after the follow-ups (`make size`, Release): text 20680, data 116, bss 34512 bytes. After the in-packet `FREQEST` change (#11): text 21404, data 116, bss 34568 bytes.

## P3: lwIP with DHCP, DNS and ping (#13)

Code: `src/net.c` (net task, netif glue, DHCP and DNS), `src/lwipopts.h` (options), `src/lwip_port.c` (`sys_now`, random numbers), `src/arch/cc.h` (asserts to `fw_panic`). lwIP 2.2.1 from `vendor/lwip`; `cmake/CMakeLists.txt` lists the core, IPv4 and Ethernet sources it compiles.

**Threading: `NO_SYS = 1` inside the net task [I].** `design.md` ("Tasks") puts ENC28J60, lwIP, TLS and WebSocket in one medium-priority net task that owns SPI2. With `NO_SYS`, that task is lwIP's only context: it reads frames from the chip, calls `netif->input`, and runs `sys_check_timeouts()` on every pass (at least every 50 ms; lwIP's shortest timer is TCP's 250 ms). There is no extra `tcpip_thread` and no lwIP locking, and nothing calls lwIP from an interrupt. P4 can run TLS over lwIP's raw TCP API in the same task (`altcp_tls` in lwIP has an mbedTLS port).

**Netif.** `eth_output()` writes the pbuf chain with `enc28j60_tx_begin/write/end` and skips lwIP's 2 bytes of `ETH_PAD_SIZE` padding. `rx_frames()` reads each good frame into a `PBUF_POOL` chain, frees it in the chip (`enc28j60_rx_end()`) and only then calls `netif->input`. That way a reply sent from inside `input`, such as an ARP or echo reply, never starts `tx_begin()` between `rx_begin()` and `rx_end()`, which the driver forbids (`src/enc28j60.h`). Frames with a bad receive status, runts and our own looped-back frames are dropped and counted. A failed pbuf allocation drops the frame and counts it as `no pbuf`. MTU 1500, MAC still the #12 placeholder. The PHY link drives `netif_set_link_up/down`. On link up, lwIP starts DHCP or re-requests the old lease (`dhcp_network_changed_link_up()`, `vendor/lwip/src/core/ipv4/dhcp.c`). A chip re-init sets the link down first.

**Protocols (`src/lwipopts.h`).** IPv4 only; ARP, ICMP (answers echo), UDP, TCP (for P4), DHCP with address conflict detection (`LWIP_DHCP_DOES_ACD_CHECK`), DNS with servers from DHCP. No IPv6, IGMP, AutoIP or raw API. No IP fragmentation or reassembly: TCP keeps to the MSS, and DNS and DHCP messages stay within one frame. Software checksums: the ENC28J60's DMA checksum is not used (errata #17, `datasheets/enc28j60-errata.txt:356-381`). DHCP host name `tado-bridge` (option 12).

**Server name.** `NET_SERVER_HOST` is a CMake cache value, default `10.42.0.1`, the bench server (`test-bench.md`, "Bench network"; `server-api.md`: server URL is a build value). After every new lease the net task resolves it with `dns_gethostbyname()` and logs `dns: <host> is <address>`. It retries every 10 s on failure. An address literal resolves without a query. P4 connects to the result.

**Random numbers.** Until P4 (#14), `LWIP_RAND()` was a xorshift32 placeholder seeded from the unique ID. It is now `rng_u32()`, from the CTR_DRBG seeded from the PA3 entropy pool before `lwip_init()` (`design.md`, "Entropy: implementation"; "P4: entropy pool and DRBG" below). lwIP uses it for the DHCP xid, the DNS id and source port, and the first TCP port.

**Router LED (PB0):** off without link, blinking at 2 Hz with link but no address, on with a DHCP lease (P3 "done when": the router LED shows link and DHCP).

**Log lines.** At boot: `ip: lwIP 2.2.1, DHCP as tado-bridge, server ...`. Then `ip: DHCP lease <ip>/<mask>, gateway ..., DNS ..., lease N s`, `dns: ...`, and `ip: address lost (DHCP state N)` if the lease goes. Every 60 s: the two `eth: 60 s:` counter lines (new: `no pbuf`, and `drop` for frames refused while the link is down or too long) and `ip: 60 s: address ..., server ... is ...`.

**Memory (Release, `make size`):** text 58844, data 124, bss 51508 bytes (before: 27728, 120, 34740). lwIP adds about 31 KB of flash. Its RAM is static: `ram_heap` (`MEM_SIZE` 6 KB) 6163 bytes, `PBUF_POOL` (6 buffers of 1516 bytes) 9195 bytes, other pools, PCBs, ARP and DNS tables under 1 KB: about 16.4 KB (`arm-none-eabi-nm -S`). The net task's stack is now 4 KB (1024 words) out of the FreeRTOS heap. Measured on the bench [V]: 766-768 words never used, so lwIP input and transmit with `LOG()` use about 1 KB. Heap free 23504-23512 bytes of 32768.

**Bench test, expected result after `make flash` and `make rtt`** (Ethernet to the laptop's `enp196s0f0`, NetworkManager shared mode, `test-bench.md`):

1. Boot log as in P2, then `ip: lwIP 2.2.1, DHCP as tado-bridge, server 10.42.0.1`, the `eth:` bring-up lines, `ip: DHCP lease 10.42.0.x/255.255.255.0, gateway 10.42.0.1, DNS 10.42.0.1, lease 3600 s`, `dns: 10.42.0.1 is 10.42.0.1 (no query needed)`. The router LED blinks briefly and then stays on.
2. Laptop side: `journalctl | grep dnsmasq-dhcp` shows DISCOVER, OFFER, REQUEST and ACK for `02:7a:d0:b1:00:01 tado-bridge`; dnsmasq then resolves `tado-bridge` to the lease. The lease file in `/var/lib/NetworkManager/` is root-only.
3. `ping <ip>` and `ping -s 1472 -M do <ip>` (full 1514-byte frames): no loss. A TCP connect to any port is refused at once (lwIP answers RST).
4. DNS through the DHCP server: build with `cmake -Bbuild -DNET_SERVER_HOST=<name> cmake/`, then `dns: <name> is <address> (N ms)`. Afterwards rebuild with `-DNET_SERVER_HOST=10.42.0.1` for the bench server.
5. The per-minute lines keep `bad 0`, `no pbuf 0`, `busy 0`, `reinit 0`.

## P4: entropy pool and DRBG (#14)

Code: `src/pa3_adc.c` (ADC1 channel 3 on PA3, register level), `src/entropy.c` (SP 800-90B start-up and continuous health tests, SHA-256 conditioning), `src/rng.c` (mbedTLS CTR_DRBG), `src/tado_mbedtls_config.h`. Design, measurement and the chosen values: `design.md`, "Entropy: measurement" and "Entropy: implementation". Analysis tool: `tools/entropy/pa3_noise.py`.

**mbedTLS 3.6.7 (LTS), `vendor/mbedtls` [I].** The design approved mbedTLS (`design.md`, "Software stack"). 3.6 is the long-term-support branch, and lwIP 2.2.1's `altcp_tls` port is written against the 2.x/3.x API (`mbedtls_ctr_drbg_*` and `mbedtls_entropy_*` in `vendor/lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c:71-72`), which 4.x no longer offers as public API (from memory, not checked here). Only `aes.c`, `ctr_drbg.c`, `platform_util.c` and `sha256.c` are compiled so far. The generated library files are in the release tag, so its `framework` submodule is not needed. P4's TLS work (#15) extends the config file and the source list.

**Log lines.** At boot, after the radio lines: `rng: PA3 start-up test passed: 1024 readings, range <lo>-<hi>, longest run N, APT max N of 512, N read timeouts` and `rng: CTR_DRBG (AES-256) seeded with 48 bytes of conditioned PA3 entropy in 44 ms`, then the lwIP lines as in P3. On failure: `rng: PA3 start-up test failed: <test>` and `rng: no random numbers; network stays off, retrying in 10 s`.

**Memory (Release, `make size`):** text 67868, data 124, bss 51856 bytes (P3: 58844, 124, 51508). mbedTLS adds about 9 KB of flash (AES tables in flash), and the DRBG context adds 348 bytes of bss. Net task stack: 493-500 of 1024 words never used (P3: 766), because seeding uses about 1 KB more stack **[V]**. Heap free 23504 bytes, unchanged.

**Bench test (2026-11-14) [V].** `make flash` (OpenOCD verify OK), `make rtt`: 11 boots, every start-up test passed and every seed took 43-45 ms. DHCP lease 10.42.0.43 from the laptop, `ping` 3 of 3. The per-minute counters were clean: `bad 0`, `no pbuf 0`, `reinit 0`.

**RTT after flashing a different image [V]:** if `tools/rtt.sh` starts during the 2 s startup delay, it can find an earlier image's RTT control block, which is still in RAM at that image's address, and print that image's stale log. The running firmware writes its block elsewhere, so nothing new appears. Start `make rtt` after the delay, or send `rtt stop` and `rtt start` on OpenOCD's Tcl or telnet port once the firmware runs.

## P4: TLS and WebSocket (#15)

Code: `src/tls.c`, `src/ws.c`, `src/coap.c`, `src/session.c`; mbedTLS options in `src/tado_mbedtls_config.h`. Design and the reasons for each choice: `design.md`, "TLS and WebSocket (#15)". The net task runs it after DHCP and the server lookup (`src/net.c`).

**Provisioning: `config/local.cmake` (gitignored).** Copy `config/local.cmake.example` and fill in `BRIDGE_SERIAL`, `BRIDGE_EUI64` and `SERVER_CA_PEM`. CMake writes them to `build/generated/provision.h`. It refuses a malformed serial or EUI-64, and a PEM file that holds a private key. Run `make clean` once after creating the file. Without it, the image builds and logs `session: no bridge identity built in ...`, and never connects. The serial and EUI-64 must never enter git (`CLAUDE.md`); `build/` and `config/local.cmake` are ignored. P6 replaces this with the identity record in external flash (`design.md`).
- **EUI-64:** the 8 bytes at config start + 50, in stored order (`tanoclo/patch_ib_firmware/extract_rf_key.js:71`). They start with tado's OUI `00 1b c5`.
- **Serial [I]:** the backup holds a u32 at the config marker + 8 (`extract_rf_key.js:58`). The serial is `IB` and that number in decimal, padded to 10 digits. Reasons: tado serials are `IB` plus 10 characters (`tanoclo/ws-server/test/ci_seed.sql:46`), a u32 has at most 10 decimal digits, and no ASCII `IB` serial appears anywhere in either dump (searched 2026-11-14). **The owner should compare this with the label on the bridge.** The bench test only needs the same string in tanoclo's database, so it works either way. A tanoclo import from the tado cloud would register the label's serial.
- **Trust anchor:** for the bench, `~/tanoclo-bench/certs/bench-ca.pem` (`test-bench.md`, "Bench certificate"). It is a public certificate, built in as PEM text.

**Server port:** CMake cache value `NET_SERVER_PORT`, default 988 (`tanoclo/ws-server/lib/config.js:47`). The certificate must name `NET_SERVER_HOST`.

**Registering the bridge in tanoclo (bench, done 2026-11-14).** tanoclo answers auth only for a serial in its `devices` table, and otherwise stays silent (`tanoclo/ws-server/lib/handlers.js:129-133`). auth/token also needs a `websocket_whitelist` entry for the serial or its home, unless that table is empty. It is empty on the bench, so every known device passes (`tanoclo/ws-server/lib/db-auth.js:45-52`). The bench database had no homes, users or devices, so one row was added, as `prototype-plan.md` P4 planned (`ci_seed.sql:39-67` does the same for its test bridge):

```sh
docker exec tanoclo-db mysql -utanoclo -p'<MYSQL_PASSWORD from tanoclo/docker-compose.yml>' tanoclo -e \
  "INSERT INTO devices (serial_no, device_type, home_id, current_fw_version, connection_state,
     in_pairing_mode, connection_state_timestamp)
   VALUES ('<BRIDGE_SERIAL>', 'IB01', 0, 'p4-bench', 0, 0, '2026-11-14T00:00:00Z');"
```

`home_id` 0 is a placeholder: no `homes` row exists. Before a real tanoclo setup, for example an import from the tado cloud, delete this row (`DELETE FROM devices WHERE serial_no = ...`) or reset the database (`docker compose ... down -v`). The import updates an existing row but not its `home_id`. For the test, tanoclo's `log_level` (table `server_settings`) was set to `debug`, which logs `AUTH OK` and `TIME_SYNC`, and set back to `info` afterwards. It is read when the server starts.

**Log lines.**
- Boot: `tls: trust anchor CN=..., P-256 yes`, then `session: bridge <serial>, server wss://10.42.0.1:988/hw/v2`.
- Each connection: `tls: TLSv1.2 TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256: handshake N ms (TCP N ms), N ms CPU in N steps, longest N ms; records 2048/2048; heap free N min N`, `session: WebSocket open ...`, `session: auth/key accepted ...`, `session: auth/token accepted ...: 8-byte session token, lifetime 1440 min`, `time: <UTC date> (unix N) from GET time`, `session: ready: ...`.
- Failures: `session: <reason> (in <phase>); retrying in N ms`. A missing database row shows as `no reply to auth/key in 10 s; is the serial a device in tanoclo's database?`.

**Memory (Release, `make size`):** text 131236, data 136, bss 56192 bytes (P4 #14: 67868, 124, 51856). TLS, X.509 and the WebSocket add about 63 KB of flash. The 4.3 KB of new bss are the WebSocket buffers (2062 + 1280 bytes) and the mbedTLS context, configuration and trust anchor. The net task stack is now 1536 words. Measured on the bench **[V]**:
- 928 words never used, so the peak is 608 words (2.4 KB, against 2.1 KB in P4 #14).
- FreeRTOS heap free: 20800 bytes before the first connection, 12824 while connected, 9568 at the lowest point, which is the handshake. So a connection holds about 8 KB (mostly the record buffers), and the handshake peaks at about 11.2 KB.
- Static RAM plus the 4 KB main stack: about 60 KB of 128 KB. These figures feed #16 and #17.

**Bench test (2026-11-14) [V].** Image flashed with OpenOCD `program ... verify` (Verified OK), with tanoclo in Docker on `10.42.0.1` (`test-bench.md`).
- **8 resets in a row:** each one authenticated on the first attempt.
  - Handshake 1070-1076 ms (TCP connect 1-2 ms, CPU 1051-1057 ms, longest step 364-367 ms).
  - WebSocket open 1075-1083 ms after the connect started; auth/key and auth/token 4-6 ms each.
  - `session: ready` 1088-1097 ms after the connect started.
- **tanoclo's side:** it logged `WS upgrade ... protocol: binary`, `AUTH_KEY: device=<serial> ipv6=fe80:0:0:0:21b:...`, `AUTH OK: <serial> registered` and `TIME_SYNC: Sent <unix>`. The `devices` row shows `connection_state` 1, the bridge's link-local address and a stored token.
- **`docker restart tanoclo-ws`:** the bridge logged `tls closed by server (0)` and was ready again about 3 s later.
- **`docker stop` for 40 s:** retries were refused (`tls tcp (-14)`, lwIP `ERR_RST`) after 3.1, 8.5, 19.6 and 32.1 s. After `docker start`, the next retry succeeded, 25 s after the start.
- **Idle:** the session stayed up for 130 s, past three ping intervals.
- **Watchdog:** with the timed kick window, no `watchdog: not kicked` line in these runs.

**Found during the test [V]:**
- **The timeout bug.** At first, about one connection in three stalled, with `no reply to auth/token` or `to time` after 10 s. A packet capture in the server container's network namespace showed the cause: the bridge itself sent a WebSocket ping and a close straight after its request, and the server then closed the connection. The method was a throwaway sidecar, `docker run --rm --net container:tanoclo-ws --cap-add NET_RAW nginx:alpine`, with `apk add tcpdump`, filtered to port 988; the capture stayed out of git. The timeouts compared a tick count read before the reply handlers ran with times those handlers had just stamped. The unsigned difference wrapped, and both timeouts fired at once. They now read the tick fresh and compare signed (`elapsed()` in `src/ws.c` and `src/session.c`).
- **No-fault checks on the server side.** A host-side Python client did the same exchange: TLS 1.3, then TLS 1.2 with the same suite, then through `openssl s_client -maxfraglen 2048`, with stale connections left open. Every reply came within 0.2-3.1 ms, so tanoclo and the max_fragment_length extension were cleared.
- **The first server flight is retransmitted once** about 210 ms after it is sent. lwIP's delayed ACK does not go out while the net task is busy in the first P-256 step, and the server's retransmission timeout is 200 ms. It costs a duplicate 731 bytes and nothing else. Sending an immediate ACK before long steps would avoid it (#16).

**Owner checks on hardware:**
1. The cloud LED (PB5) blinks at 2 Hz for about 1 s after the router LED comes on, then stays on.
2. Unplug Ethernet: the LED goes off (`session: network down`). Plug it back in: the LED is on again within a few seconds.
3. Compare the serial in the log with the label on the bridge (see "Serial" above).
