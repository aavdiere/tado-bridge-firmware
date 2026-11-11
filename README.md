# tado-bridge-firmware

Replacement firmware for the tado Internet Bridge (IB01) hardware. It speaks the stock bridge API to a self-hosted server instead of the tado cloud, with [tanoclo](https://github.com/tanoclo/tanoclo) as the server and test harness.

This is heavily AI-assisted work. The code and the documentation were written largely with AI assistance, under the direction of the project owner. Behavior was checked with automated tests, and on a real plant only where the [Status](#status) section says so.

This project is not affiliated with tado. "tado" is used only to describe the hardware it targets.

## Status

Design done, nothing built or tested yet. Do not flash this onto a bridge without a verified backup (see stage P0).

| Stage | Goal | Status |

| Stage | Goal | Status |
| :--- | :--- | :--- |
| P0 | Backup and restore rehearsal | Done |
| P1 | Skeleton: clock, LEDs, button, RTT, FreeRTOS | Done (2026-11-11): skeleton and RTT (#6), clock tree (#7), FreeRTOS, LEDs, buttons and watchdog (#8) flashed and verified on the bench |
| P2 | Radio and flash on SPI1 | Not started |
| P3 | Ethernet (ENC28J60, lwIP) | Not started |
| P4 | TLS and WebSocket to tanoclo | Not started |
| P5 | Radio link to devices, pairing | Not started |
| P6 | Persistent storage | Not started |
| P7 | Size check, bootloader, self-update | Not started |

Each stage is a GitHub milestone, and its tasks are issues. Details: [`docs/prototype-plan.md`](docs/prototype-plan.md).

## Docs

- [`docs/README.md`](docs/README.md): index of the design notes and project decisions
- [`docs/open-questions.md`](docs/open-questions.md): what is still to be measured or decided
- [`docs/design.md`](docs/design.md): approved design (stack, storage, self-update)
- [`docs/hardware.md`](docs/hardware.md): IB01 hardware and pin map
- [`docs/test-bench.md`](docs/test-bench.md): bench setup and how the owner's stock tado network is protected

## Hardware

| Part | Device |
| :--- | :--- |
| MCU | STM32F411CEU6 (512 KB flash, 128 KB RAM) |
| Radio | TI CC110L, 868 MHz |
| Ethernet | Microchip ENC28J60 |
| Storage | Macronix MX25R1635F, 2 MB |

## Working with Claude Code

`CLAUDE.md` gives the project rules, and `.claude/agents/` defines subagents with a fixed model and effort for each kind of task: routine code, drivers, the hardest radio and bootloader work, review before flashing, log reading, and docs upkeep. Mapping per stage: [`docs/prototype-plan.md`](docs/prototype-plan.md#subagents-per-stage).

## Never commit firmware

A dump of a real bridge holds its network key and identity. Firmware images, flash dumps, captures and keys must never enter this repository:

- `.gitignore` excludes them.
- `tools/check-no-firmware.sh` refuses binaries, firmware file types and hex dumps. Enable it as a pre-commit hook once per clone: `git config core.hooksPath .githooks`.
- The `no-firmware` GitHub Action scans the whole history on every push.

Keep backups of your bridge **outside any git repository**, for example in `~/ib01-backups/`.

## Licence

MIT, see [`LICENSE`](LICENSE). Code ported from other projects keeps its original licence and notices.
