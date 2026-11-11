# Design notes

Research and design notes for the IB01 firmware. This file is the index.

## Source repos

Citations such as `freedo/docs/PROTOCOL.md:1305` are relative to a working folder that holds these repos side by side:

| Repo | What it is |
| :--- | :--- |
| `freedo/` | Local coordinator for tado V3+ 868 MHz devices: Go daemon, ESP32 + CC1101 radio, Home Assistant integration. |
| `tanoclo/` | Self-hosted Tado backend: REST/WebSocket server, patched Internet Bridge, ESP32 + SX1276 sniffer and emulator. |
| `Tado_Hardware_Teardown/` | Teardown material: logic captures and an over-the-air protocol dissector for Wireshark. |
| `Tado_Hardware_Teardown.wiki/` | Wiki for the teardown: CC110L registers, RF parameters, board analysis, link protocol. |

## Conventions

- One Markdown file per topic, created when the topic comes up and listed in the table below.
- Every factual claim cites its source as a repo-relative path (e.g. `freedo/docs/PROTOCOL.md`), with a line number where useful.
- Mark each claim's status: **verified** (seen on hardware), **documented** (stated in a repo or its docs), or **inferred** (our own reasoning).
- When repos disagree, record the conflict rather than picking one silently.
- Update notes as findings arrive, not at the end of the work.
- Datasheet PDFs, their text extracts (`datasheets/*.pdf`, `datasheets/*.txt`) and firmware dumps are kept locally only and never committed.

## Topics

| File | Covers |
| :--- | :--- |
| [`open-questions.md`](open-questions.md) | Master list of what is still required for the IB01 firmware. Start here. |
| [`hardware.md`](hardware.md) | IB01 components, pin map, RF path, handling. |
| [`server-api.md`](server-api.md) | Bridge-to-server API the firmware must implement. |
| [`datasheets/cc110l-vs-cc1101.md`](datasheets/cc110l-vs-cc1101.md) | CC110L against CC1101, and what carries over from freedo. Datasheet PDFs and text extracts are kept locally in `datasheets/`, not in git. |
| [`radio-runtime.md`](radio-runtime.md) | How to drive the CC110L after init: RX, TX, ACKs, wake trains, anchors. |
| [`tools/read_ib01_system_areas.tcl`](tools/read_ib01_system_areas.tcl) | Read-only OpenOCD script for option bytes, OTP and unique ID. Run alongside `read.sh`. |
| [`test-bench.md`](test-bench.md) | Bench setup (ST-Link, Ethernet, RTL-SDR), what Claude may do on hardware, protecting the stock network. |
| [`prototype-plan.md`](prototype-plan.md) | Staged prototype plan P0-P7 based on `stm-template`, with GitHub progress tracking. |
| [`build.md`](build.md) | Build, flash (OpenOCD) and RTT log commands. |
| [`design.md`](design.md) | Firmware design proposals. Persistent storage, entropy, software stack, self-update and first flash. |
| [`radio.md`](radio.md) | 868 MHz PHY, CC110L registers, frames, coordinator behaviour, repo conflicts, reuse. |

## Project decisions

- Goal: new firmware for the IB01 that speaks the stock bridge API to a self-hosted server, with no Tado cloud.
- HomeKit is out of scope for now. Device pairing does not need the HomeKit chip (project owner's decision); follow freedo and tanoclo for pairing.
- Existing devices do not stay paired. They are re-paired to the new firmware.
- Stay strictly compatible with tanoclo's frame format and auth flow, so tanoclo is the server and test harness.
- Support 16 devices for now.
- 2026-11-11: test-only code paths (such as the P1 watchdog hang test) are removed once their test passes, not left in behind flags.
- Firmware update images are not signed.
- Listen before transmit, as the stock bridge does it.
- Claude may flash and test the bench bridge within `test-bench.md`; power from the ST-Link 3V3 only; tanoclo runs in Docker on the laptop; the stock network's channel is found by SDR scan; no test device yet (2026-11-11).
- Stock network found on channel 26 (868.3237 MHz) by SDR scan: protected channel (2026-11-11). Test channel 10 (865.1245 MHz), owner decision 2026-11-11. Pair only a device named by EUI-64.
- Work is delegated to subagents with a fixed model and effort per task type (`CLAUDE.md`, `.claude/agents/`), 2026-11-11.
- Progress is tracked in this repo: milestones P0-P7 and issues, see [`prototype-plan.md`](prototype-plan.md).
