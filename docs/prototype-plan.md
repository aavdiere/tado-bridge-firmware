# Prototype plan

Builds the IB01 firmware in stages. Each stage ends in something testable and settles specific open questions (`open-questions.md`). Design: `design.md`. Pins: `hardware.md`.

**Status: proposal, for review.** Nothing is created or flashed yet.

## Progress tracking on GitHub

Requirement (user, 2026-11-11): progress is tracked in a GitHub repo on the user's account (`aavdiere`, logged in through `gh`).

Proposal:
- **One repo** holding the firmware and the design docs. Name and visibility still to be decided.
- **Milestones** = the stages below, P0 to P8.
- **Issues** = the tasks in each stage, plus the open measurements: #7, #16, #17, #19, #26, #28, the pair button, and the #20 size check.
- **README** shows a status table per stage. Each finished task is a commit or pull request that closes its issue.
- **Never pushed:** firmware dumps (they hold the bridge's secrets), keys, datasheet PDFs and their text extracts (third-party copyright). Enforced by `.gitignore`, `tools/check-no-firmware.sh` as a pre-commit hook, and the `no-firmware` CI scan of the whole history.

## Base: `stm-template`

[`aavdiere/stm-template`](https://github.com/aavdiere/stm-template) (public): CMake with a Makefile wrapper and libopencm3 as a submodule, with an STM32F4 family file and VS Code debug setup.

| Item | Template now | Change for the IB01 |
| :--- | :--- | :--- |
| Device | `STM32F413ZH` (`Makefile`) | `STM32F411CE`. libopencm3 knows it: 512 KB, 128 KB (`vendor/libopencm3/ld/devices.data:159`) |
| Linker script | `cmake/stm32f413zh.ld` with `ram2` | New `cmake/stm32f411ce.ld`: 512 KB at `0x08000000`, 128 KB RAM, no `ram2`. App moves to `0x08010000` in P7 |
| Family file | `cmake/stm32f4.cmake` (Cortex-M4F, hard float) | No change |
| Clock | 16 MHz internal, no setup (`src/firmware.c`) | 26 MHz crystal to about 96-100 MHz via PLL. libopencm3 has no 26 MHz preset and its `rcc_clock_setup_pll()` waits forever on HSE, so `src/clock.c` sets the registers itself with timeouts: 96 MHz, checked against RM0383 (`hardware.md`, Clock tree) **[I]** |
| Flashing | `st-flash --connect-under-reset` | IB01 test points carry no NRST (`tanoclo/patch_ib_firmware/README.md:151-158`), so use OpenOCD `program ... verify reset exit` with `stlink.cfg` and `stm32f4x.cfg` |
| Debugger | Cortex-Debug for J-Link or `stutil` | Cortex-Debug with OpenOCD |
| Logging | J-Link SWO viewer | SEGGER RTT over SWD; installed OpenOCD 0.12 supports `rtt server`. SWO needs PB3, which may not reach a test point |
| C library | Full newlib; `nano.specs` commented out (`cmake/toolchain-arm-none-eabi.cmake`) | Enable `nano.specs` to save flash for the #20 size check |
| Libraries | libopencm3 only | Add FreeRTOS-Kernel, lwIP, mbedTLS as pinned submodules |
| Build tool | Needs `cmake` | Installed |

Example code blinks PB0, which on the IB01 is the router LED, active high. So the template's own example is a valid first test.

libopencm3 is LGPL-3. That is fine for own use. Distributing binaries would require letting others relink.

## Safety rules for working on the IB01

Bench setup, Claude's permission to flash and test, and protection of the owner's stock network: `test-bench.md`.

- Power over USB with the ST-Link's 3V3 wire disconnected (owner decision, 2026-11-14); never two supplies at once (`tanoclo/patch_ib_firmware/README.md:211`).
- Never reconfigure PA13 or PA14, the SWD pins. With no NRST on the test points, losing SWD makes recovery hard.
- Start every build with a short delay before low-power modes or risky code, so the debugger can still attach **[I]**.
- Last resort if SWD is lost: pull BOOT0 high so the chip starts in ST's ROM bootloader, which leaves SWD usable (`hardware.md`) **[I]**.
- Keep the backup and restore images ready from P0.

## Subagents per stage

Each task is delegated to a subagent with its own model and effort, defined in `.claude/agents/` and summarised in `CLAUDE.md`. Owner decision, 2026-11-11. Prices per million tokens (input/output) as listed 2026-11-03: Fable 5.2 $8/$40, Opus 5.5 $4/$20, Sonnet 5.5 $2/$10, Haiku 5.5 $0.10/$0.50.

| Stage | Main subagent | Also |
| :--- | :--- | :--- |
| P0 Backup and restore | `fw-routine` (scripts, checks) | `log-reader` for OpenOCD output; the owner runs the hardware steps |
| P1 Skeleton | `fw-routine` | `fw-driver` for the clock tree |
| P2 Radio and flash | `fw-driver` | `rf-bench` for the channel scan and demodulator; `log-reader` for RTT logs |
| P3 Ethernet | `fw-driver` | `log-reader` |
| P3.1 ENC28J60 SPI2 bus | `fw-driver` | `log-reader`; the owner probes or captures the bus if the firmware tests do not settle it |
| P4 TLS and WebSocket | `fw-driver` | `log-reader` for RAM and handshake measurements |
| P5 Radio link to devices | `fw-critical` | `rf-bench` for over-the-air timing; `fw-reviewer` before flashing |
| P6 Storage | `fw-driver` | `fw-reviewer` for power-cut safety |
| P7 Size check, bootloader, self-update | `fw-critical` | `fw-reviewer` before flashing; `log-reader` for size reports |
| P8 Code clean-up | `fw-critical` (Fable, high) | `fw-reviewer`; `log-reader` for size reports |
| Any stage | `docs-keeper` for docs, issues and commits | `fw-reviewer` before anything goes onto a board |

## Stages

### P0: Backup and restore (before anything is flashed)

- `read.sh` twice and compare, plus `tools/read_ib01_system_areas.tcl` (`hardware.md`, "Full backup").
- Confirm read protection is level 0 (`0xAA`). If not, stop.
- Extract the serial and EUI-64 from the dump (#8).
- Optional while connected: find the pair button through the GPIO input registers (`open-questions.md`, Todo).
- **Rehearse the restore:** write the backup back and re-read it.
- Store all dumps **outside any git repository** (for example `~/ib01-backups/`). This repo refuses them through `.gitignore`, a pre-commit hook and a CI scan of the whole history.
- **Done when:** two identical dumps are stored privately and the restore is proven. Closes #8, #9.

### P1: Skeleton

- Repo from the template, with the changes above.
- Clock at about 96-100 MHz from the 26 MHz crystal, RTT logging, OpenOCD flashing.
- All four LEDs (PB0, PB1, PB5, PB8, high = on) and the factory reset button (PB10, active low, internal pull-up).
- FreeRTOS with empty radio, net and housekeeping tasks; independent watchdog.
- **Done when:** LEDs follow the button, RTT shows logs, and the watchdog resets a deliberate hang.

**P1 result: done, 2026-11-11 [V]** (RTT logs, kept out of git; image sha256 `6f9a6a4acc3aeb12d1b95283d73b044ac672459334d6e40c5c99234d24378bba`, flashed with `make flash` after the `fw-reviewer` verdict "safe to flash"):

- Clock: HSE crystal, 96 MHz, registers as predicted (`hardware.md`). The #7 VOSRDY concern is settled: HSE+PLL completed without the "regulator scale 1 not ready" fault, PWR_CR `0000c000`, so VOSRDY sets within the 5 ms timeout on this unit.
- LEDs and buttons: power (PB1) and cloud (PB5) blink from the net and radio tasks (P1 test wiring); router (PB0) follows factory reset (PB10), pairing (PB8) follows pair (PC15), observed by the owner. RTT logged clean press and release events for both buttons.
- Watchdog: factory reset held >5 s gave `HANG TEST: net task stops checking in`, about 5 `watchdog: not kicked, missing check-in mask 2` lines at 500 ms intervals, then a reset with `reset: RCC_CSR 24000000: IWDG PIN`. Both buttons >5 s gave `HANG TEST: radio task spins`, a reset and `IWDG PIN`. RTT via OpenOCD reattached by itself after each reset.
- The temporary hang path used for the watchdog test was removed from the firmware afterwards (owner decision 2026-11-11, `README.md` "Project decisions"); the lines above are history.
- RAM at runtime: FreeRTOS heap free 25632 of 32768 bytes (minimum the same); unused stack words radio/net/hk 489/489/352 of 512 (early data point for #17, not a conclusion).
- The owner power-cycled the board afterwards so the CC110L is back at power-on defaults rather than the stock channel-26 configuration.
- Non-blocking review notes are tracked in the "Follow-ups from the P1 pre-flash review" issue (P2 milestone).

### P2: Radio and flash on SPI1

- SPI1 (PA5-7), radio CS PA4, flash CS PB9, GDO0 PA0, GDO2 PA1 (`hardware.md`).
- Read the radio's part number and version, load the register table with `FSCTRL1=0x06` (`radio.md`), enter RX.
- Log received frames: length, `CRC_OK`, RSSI, `FREQEST` (`radio-runtime.md`).
- Read the flash JEDEC ID on the same bus.
- Measure the crystal offset from `FREQEST` on frames from real devices.
- Find the stock network's channel with the RTL-SDR and set it as the protected channel (`test-bench.md`).
- Build the SDR demodulator used later for timing measurements.
- **Done when:** frames from nearby Tado devices arrive with CRC OK, and flash and radio both work on the shared bus. Settles #7.
- **Status (2026-11-12, [V]):** receive-only run met the frame and shared-bus criteria (1115 of 1119 CRC OK, 30 of 30 flash ID checks; `radio-runtime.md`, `hardware.md`). #7 is not settled: `FREQEST` needs an earlier read (#11).

### P3: Ethernet

- SPI2 (PB12 CS, PB13-15), INT PB2, RESET PC14.
- ENC28J60 driver: revision ID, link status.
- Bench network: static address on `enp196s0f0`, `dnsmasq`, tanoclo in Docker (`test-bench.md`).
- lwIP with DHCP, DNS and ping.
- **Done when:** the bridge gets an address, answers ping and resolves the server name. The router LED shows link and DHCP.
- **Status 2026-11-14 [V]:** ENC28J60 driver (#12), done: half duplex, link status, frame TX and RX against the laptop (ARP probe answered, 1514-byte frames both ways, 125 of 125 received pings intact), cable pull and full power cycle passed (`hardware.md`, "Result: half duplex, link, frame RX and TX"). lwIP (#13): result below. Open in P3: tanoclo in Docker on the link (#27).

**P3 lwIP result (#13), 2026-11-14 [V].** Bench as in `test-bench.md` (USB supply, `enp196s0f0` shared mode, radio receive-only on channel 26 as in P2). lwIP 2.2.1 in the net task (`build.md`, P3). RTT logs kept outside the repo.

| Check | Result |
| :--- | :--- |
| DHCP | Lease `10.42.0.43/24`, gateway and DNS `10.42.0.1`, 3600 s, on each of 5 boots (flash resets and OpenOCD `reset run`). The laptop's journal shows DISCOVER, OFFER, REQUEST, ACK for `02:7a:d0:b1:00:01 tado-bridge` within the same second. On the very first boot the bridge sent DISCOVER twice: dnsmasq checks a new address with a ping before it offers it, and lwIP retransmitted meanwhile **[I]**. dnsmasq then resolves `tado-bridge` to `10.42.0.43` |
| DNS | Test image built with `NET_SERVER_HOST=example.com`: `dns: example.com is 104.20.23.154 (163 ms)` and on another boot `172.66.147.243 (613 ms)`, the two A records the laptop's own query to `10.42.0.1` returned. The final image resolves the literal `10.42.0.1` without a query |
| Ping, laptop to bridge | Final image: 200 of 200 (56 bytes, average 1.11 ms) and 100 of 100 with `-s 1472 -M do` (1514-byte frames, average 10.3 ms). Test image: 100/100, 100/100 and 20/20 at 0 bytes |
| Ping, bridge to laptop | Test-only gateway ping in the test image (raw API, 1472-byte payload checked byte by byte): 60 of 60 good replies, RTT 10.1-12.3 ms. Removed afterwards, with `LWIP_RAW` back to 0 (project rule) |
| TCP | A connect from the laptop to ports 80 and 988 is refused at once (lwIP answers with RST) |
| Router LED | PB0 in GPIOB_ODR read over SWD while running: steady 1 with the lease; power LED PB1 toggling at 1 Hz |
| Renewal and 30-minute run | Final image: the laptop logged the renewal REQUEST and ACK at 11:06:08, exactly T1 = 1800 s after the 10:36:08 ACK; no `address lost` on the bridge. 30 per-minute lines without an error or re-init and no fault or watchdog reset, while the radio received up to 2053 frames a minute on channel 26 (50 of 50 pings then, average 1.16 ms) |
| Counters | Per-minute lines: `bad 0`, `no pbuf 0`, `busy 0`, `drop 0`, `tx err 0`, `late-col retry 0`, `reinit 0`, `int storm 0`; laptop `rx_errors` and `collisions` 0 |
| RAM | Heap free 23504-23512 of 32768 bytes; net task 766-768 of 1024 stack words never used |
| Cable pull with lwIP [V] | 2026-11-14: the owner unplugged the cable at the bridge for about 10 s and replugged it. RTT log: `eth: link down (PHSTAT2 0000...)`, the 60 s line `link irq 1, reinit 0`, `eth: link up (PHSTAT2 0400...)`, then `ip: DHCP lease 10.42.0.43/255.255.255.0, gateway 10.42.0.1, DNS 10.42.0.1, lease 3600 s` (same address as before). Laptop ping 3 of 3, 1.1-1.9 ms. No PANIC, no re-init. Router LED (owner's observation): off unplugged, blinking after replug, steady on once the lease was back |

Final image sha256 (`build/tado-bridge.bin`) `74836cca837f716334afcae069a14f9d5d7a481f0fdd0198781666deed5994d2`, flashed with OpenOCD verify OK.

`nmcli device disconnect` on the laptop (IP outage, PHY carrier stays up): the bridge kept its lease and answered again right after reconnect. Cable pull with lwIP: see the last row of the result table above.

### P3.1: Reliable SPI2 bus to the ENC28J60

Inserted 2026-11-13 after the first P3 bench runs (`hardware.md`, "SPI2: ENC28J60 bring-up"). The chip works from the debugger at 6 MHz and below, but not from the firmware.

- Find what the firmware does differently: CS setup and CS-high time between transactions first (try about 1 µs and 5 µs, then shrink), then gaps between bytes. Small diagnostic images; no change to the radio.
- Find the margin: the largest SCK and the shortest CS timings that read 1000 of 1000 register reads correctly, then pick values with a factor of 2 margin.
- If the firmware tests do not explain it: owner measures CS (ENC28J60 pin 5) and SO (pin 2) with a scope or logic analyser during a read.
- Read the stock firmware's APB1 clock from the backup to know its real SPI2 rate.
- Record the cause and the chosen timings in `hardware.md`; remove the diagnostic code.
- **Done when:** the #12 bring-up passes on every boot (EREVID, PHID, read-back of every configured register) and a 10-minute run shows no `reinit` and no read-back mismatch. Then #12 continues.

**Status 2026-11-14 [V]:** the cause is not CS timing but the supply. With the PHY active, the board rail sags to about 2.7 V, below the ENC28J60's 3.10 V minimum, and the chip stops answering. With the chip in power save (3.1-3.2 V), 1000 of 1000 iterations are correct at 12, 6 and 3 MHz with the existing CS timings (`hardware.md`, "Cause: the supply sags below the ENC28J60's minimum"). Next: the owner decides on the supply (`test-bench.md`, "Power risk"). Then re-run `tools/bench/enc28j60_supply.tcl` and the margin test.

**Result 2026-11-14 [V]: done.** With the owner-changed supply, the board rail reads 3.28-3.29 V. Margin test with the firmware's own CS timings: 12000 of 12000 iterations good at 12 MHz and at 6 MHz. SPI2 stays at 6 MHz for a factor of 2 on timing margin. Bring-up passed at the first attempt on 7 consecutive resets, and a 10-minute run showed no `reinit`, no read-back mismatch and 65 of 65 self-test frames sent (`hardware.md`, "Result: margin test and 10-minute run"). The diagnostic code is removed. Still to try: bring-up after a full power cycle.

### P4: TLS and WebSocket to tanoclo

- Entropy pool from PA3 (`design.md`); log the raw readings to measure the noise (#26).
- mbedTLS: TLS 1.2, `ECDHE-ECDSA-AES128-GCM-SHA256`, 4 KB in, 2 KB out (`design.md`).
- Connect to tanoclo on port 988, path `/hw/v2`; `auth/key`, `auth/token`, `GET time` (`server-api.md`). Seed the bridge serial in tanoclo first (`tanoclo/ws-server/test/ci_seed.sql:39-67`).
- Measure peak RAM and handshake time (#17).
- **Done when:** tanoclo shows the bridge connected and authenticated, time is set, and RAM is within budget. The cloud LED shows the connection.

### P5: Radio link to devices

- Port from freedo (`radio-runtime.md`): ACKs, CCM, 6LoWPAN, wake trains, anchors, listen-before-transmit.
- Pairing: beacon answer, double Router Advertisements, `d/pair` with tag `0x12` (`radio.md`).
- Relay CoAP in both directions through the WebSocket frame.
- **Device to test with:** none chosen yet (owner, 2026-11-11); decide before P5. Candidates: tanoclo's ESP32 room-unit emulator (`tanoclo/docs/emulated_devices.md`), so no real valve has to leave the stock network early. Then one real device.
- **Measure:**
  - ACK start time after the Data frame ends, idle and during a TLS handshake. Use the pairing LED (PB8) as a marker on a logic analyser or scope (#16).
  - Beacon answer time (#28).
  - Delivery rate with listen-before-transmit on (#25 decision).
- **Done when:** a device pairs, reports through to tanoclo, and takes a setpoint; ACKs land inside 0-1 ms.

### P6: Storage

- Identity, network and counter records in external flash (`design.md`).
- Provision serial and EUI-64 from P0.
- Erase and program while radio traffic runs (#19).
- **Done when:** the network survives power cycles, a factory reset re-pairs cleanly, and no ACK is missed during flash work.

### P7: Size check, bootloader and self-update

- **First the size check (#20):** measure the release application size and estimate the bootloader size. Confirm the layout fits, or adjust it.
- Bootloader at `0x08000000`, application at `0x08010000`, update and rollback as in `design.md`.
- Uploader through tanoclo (`POST /api/send-raw` or one added handler).
- **Done when:** an update installs, a broken image rolls back, and a power cut during install recovers.

### P8: Code clean-up checkpoint

Owner request, 2026-11-14 (`docs/README.md`, "Project decisions"). Runs after P7. Fable skims the whole repository and rewrites messy or unclear code without changing functionality. Issues: #32 (survey and progress list), #33 (per-module clean-up), #34 (proposal for major changes).

- **Behaviour-preserving only.** No functional change. Verify each step with the build and the size report, and where hardware is involved with the existing bench checks within `test-bench.md`. Any change touching the bootloader, the storage format or radio timing still needs a `fw-reviewer` "safe to flash" verdict before it is flashed.
- **Test-only code:** remove none that the owner wants kept; otherwise the existing rule applies, no test-only code is left in (`docs/README.md`, "Project decisions", 2026-11-11).
- **Small steps, frequent checkpoints.** One commit per file or module, with the build passing, so work can resume from the last commit if Fable usage runs out. On a Fable limit, rerun on Opus high (owner's standing fallback rule).
- **Progress list:** a checklist of files and modules (done, todo, needs proposal) lives in #32 and is ticked after each commit, so a later session can continue.
- **Major changes are not made directly.** Architecture, module boundaries, API changes and renames across modules are written up as a plan and proposed to the owner for approval first (#34).
- **Subagents:** `fw-critical` (Fable, high) does the rewriting; `fw-reviewer` reviews.
- **Done when:** every module on the progress list is done or moved to an approved proposal, the build and size report match the pre-clean-up behaviour, and the bench checks still pass.

## Equipment

- ST-Link V2 and access to the SWD test points.
- Logic analyser or scope, for ACK and beacon timing.
- A tanoclo server on the LAN.
- An ESP32 with SX1276 for the emulator (TTGO LoRa32, `tanoclo/docs/emulated_devices.md`), then one spare real device.
