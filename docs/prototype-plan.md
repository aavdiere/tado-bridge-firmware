# Prototype plan

Builds the IB01 firmware in stages. Each stage ends in something testable and settles specific open questions (`open-questions.md`). Design: `design.md`. Pins: `hardware.md`.

**Status: proposal, for review.** Nothing is created or flashed yet.

## Progress tracking on GitHub

Requirement (user, 2026-11-11): progress is tracked in a GitHub repo on the user's account (`aavdiere`, logged in through `gh`).

Proposal:
- **One repo** holding the firmware and the design docs. Name and visibility still to be decided.
- **Milestones** = the stages below, P0 to P7.
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
| Clock | 16 MHz internal, no setup (`src/firmware.c`) | 26 MHz crystal to about 96-100 MHz via PLL. libopencm3 has no 26 MHz preset, so define an `rcc_clock_scale` **[I]**, checked against RM0383 |
| Flashing | `st-flash --connect-under-reset` | IB01 test points carry no NRST (`tanoclo/patch_ib_firmware/README.md:151-158`), so use OpenOCD `program ... verify reset exit` with `stlink.cfg` and `stm32f4x.cfg` |
| Debugger | Cortex-Debug for J-Link or `stutil` | Cortex-Debug with OpenOCD |
| Logging | J-Link SWO viewer | SEGGER RTT over SWD; installed OpenOCD 0.12 supports `rtt server`. SWO needs PB3, which may not reach a test point |
| C library | Full newlib; `nano.specs` commented out (`cmake/toolchain-arm-none-eabi.cmake`) | Enable `nano.specs` to save flash for the #20 size check |
| Libraries | libopencm3 only | Add FreeRTOS-Kernel, lwIP, mbedTLS as pinned submodules |
| Build tool | Needs `cmake` | **`cmake` is not installed here**; install it first |

Example code blinks PB0, which on the IB01 is the router LED, active high. So the template's own example is a valid first test.

libopencm3 is LGPL-3. That is fine for own use. Distributing binaries would require letting others relink.

## Safety rules for working on the IB01

- Power from the ST-Link only while it is attached; no USB, no Ethernet (`tanoclo/patch_ib_firmware/README.md:211`).
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
| P2 Radio and flash | `fw-driver` | `log-reader` for RTT logs |
| P3 Ethernet | `fw-driver` | `log-reader` |
| P4 TLS and WebSocket | `fw-driver` | `log-reader` for RAM and handshake measurements |
| P5 Radio link to devices | `fw-critical` | `fw-reviewer` before flashing |
| P6 Storage | `fw-driver` | `fw-reviewer` for power-cut safety |
| P7 Size check, bootloader, self-update | `fw-critical` | `fw-reviewer` before flashing; `log-reader` for size reports |
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

### P2: Radio and flash on SPI1

- SPI1 (PA5-7), radio CS PA4, flash CS PB9, GDO0 PA0, GDO2 PA1 (`hardware.md`).
- Read the radio's part number and version, load the register table with `FSCTRL1=0x06` (`radio.md`), enter RX.
- Log received frames: length, `CRC_OK`, RSSI, `FREQEST` (`radio-runtime.md`).
- Read the flash JEDEC ID on the same bus.
- Measure the crystal offset from `FREQEST` on frames from real devices.
- **Done when:** frames from nearby Tado devices arrive with CRC OK, and flash and radio both work on the shared bus. Settles #7.

### P3: Ethernet

- SPI2 (PB12 CS, PB13-15), INT PB2, RESET PC14.
- ENC28J60 driver: revision ID, link status.
- lwIP with DHCP, DNS and ping.
- **Done when:** the bridge gets an address, answers ping and resolves the server name. The router LED shows link and DHCP.

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
- **Device to test with:** tanoclo's ESP32 room-unit emulator (`tanoclo/docs/emulated_devices.md`), so no real valve has to leave the stock network early. Then one real device.
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

## Equipment

- ST-Link V2 and access to the SWD test points.
- Logic analyser or scope, for ACK and beacon timing.
- A tanoclo server on the LAN.
- An ESP32 with SX1276 for the emulator (TTGO LoRa32, `tanoclo/docs/emulated_devices.md`), then one spare real device.
