# tado-bridge-firmware

Replacement firmware for the tado Internet Bridge (IB01) hardware. It speaks the stock bridge API to a self-hosted server instead of the tado cloud, with [tanoclo](https://github.com/tanoclo/tanoclo) as the server and test harness.

This project is not affiliated with tado. "tado" is used only to describe the hardware it targets.

**Status: design done, nothing built yet.** Do not flash this onto a bridge without a verified backup (see stage P0).

## Progress

| Stage | Goal | Status |
| :--- | :--- | :--- |
| P0 | Backup and restore rehearsal | Not started |
| P1 | Skeleton: clock, LEDs, button, RTT, FreeRTOS | Not started |
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

## Hardware

| Part | Device |
| :--- | :--- |
| MCU | STM32F411CEU6 (512 KB flash, 128 KB RAM) |
| Radio | TI CC110L, 868 MHz |
| Ethernet | Microchip ENC28J60 |
| Storage | Macronix MX25R1635F, 2 MB |
