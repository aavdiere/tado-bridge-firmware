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
| P2 | Radio and flash on SPI1 | In progress: receive-only image verified on the bench (2026-11-12): radio PARTNUM/VERSION, 99.6% CRC OK on channel 26, flash and radio on the shared bus (#9, #10); `FREQEST` now valid in the packet (0 to -6 units on stock senders, #11); per-unit trim decision waits for our own transmissions in P5 |
| P3 | Ethernet (ENC28J60, lwIP) | In progress: ENC28J60 driver (#12) done, incl. cable pull and power cycle; lwIP (#13) done (2026-11-14): DHCP lease, DNS through the laptop's dnsmasq, ping both ways with full-size frames, router LED shows link and lease; cable pull with lwIP passed (relink, same lease, no re-init); tanoclo in Docker on the link (#27) done (2026-11-14): ports bound to 10.42.0.1 only, bench CA and IP-SAN certificate outside git |
| P3.1 | Reliable SPI2 bus to the ENC28J60 | Done (2026-11-14): the cause was the ST-Link 3V3 sagging to about 2.7 V; with the owner-changed supply (3.29 V) SPI2 reads 12000 of 12000 correctly at 12 and 6 MHz, runs at 6 MHz for margin, and a 10-minute run had no re-init |
| P4 | TLS and WebSocket to tanoclo | Not started |
| P5 | Radio link to devices, pairing | Not started |
| P6 | Persistent storage | Not started |
| P7 | Size check, bootloader, self-update | Not started |
| P8 | Code clean-up checkpoint | Not started: runs after P7; behaviour-preserving (#32, #33), major changes only after owner approval (#34) |

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
- `tools/check-no-firmware.sh` refuses binaries, firmware file types and hex dumps. The pre-commit hook runs it and also formats staged `src/` files with clang-format (`.clang-format`; a partly staged file is only checked, not re-staged). Enable the hook once per clone: `git config core.hooksPath .githooks`.
- The `no-firmware` GitHub Action scans the whole history on every push.

Keep backups of your bridge **outside any git repository**, for example in `~/ib01-backups/`.

## Licence

MIT, see [`LICENSE`](LICENSE). Code ported from other projects keeps its original licence and notices.

## FAQ

**Q: Why make it identical to the existing internet bridge instead of simplifying for local use?**

**A:** Having the hardware mapped out, a custom API could be made.
I do feel that having a _stable_ common ground to work around will help development in the long run.
By ensuring the firmware can function as a 1-on-1 copy of the original, maybe even talking to the actual Tado servers, others (such as tanoclo) can implement the server part and have it work together by design.
After that, additional functionality or simpler designs can still be made, knowing that there is an original implementation to reference back to.

**Q: Why make this only using LLMs?**

**A:** Because I can. That's really it. I estimate myself capable of doing all this myself given enough time, but this project was an ideal test case to see how good LLMs have become and for me to learn how to prepare and constrain them. The idea is that, aside from this FAQ section, everything is done using LLMs, up to fixing any mistakes the LLM makes, using another LLM.


