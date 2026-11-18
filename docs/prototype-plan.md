# Prototype plan

Builds the IB01 firmware in stages. Each stage ends in something testable and settles specific open questions (`open-questions.md`). Design: `design.md`. Pins: `hardware.md`.

**Status (2026-11-18): P0 to P4 and P3.1 are done and verified on the bench. P5 is nearly done (2026-11-18): #17, #18 and #19 done, #21 (P6) closed (#18 closed with the TLS-handshake ACK measurement, commit 45735e2); #20 skipped for now (owner decision 2026-11-18), #11 open. P6: #22 and the factory reset built and host-tested 2026-11-18, waiting for review and the bench. P7 started: size check done 2026-11-18 (#23), layout kept. P8 is not started.** The plan was reviewed on 2026-11-16; the changes are listed in "Plan review" at the end.

## Progress tracking on GitHub

Requirement (user, 2026-11-11): progress is tracked in a GitHub repo on the user's account (`aavdiere`, logged in through `gh`).

- **One public repo**, `tado-bridge-firmware`, holding the firmware and the design docs.
- **Milestones** = the stages below, P0 to P8.
- **Issues** = the tasks in each stage.
- **README** shows a status table per stage. Each finished task is a commit that closes its issue.
- **Two number series.** Open questions in `open-questions.md` and GitHub issues both run from 1, and they do not match: open question 16 (ACK turnaround) is issue #18, and issue #16 was the RAM measurement (open question 17). In this file, "#N" always means GitHub issue N, and an open question is written "open question N". Older docs still write "#N" for open questions; read the number against the topic.

| Topic | Open question | Issue |
| :--- | :--- | :--- |
| Crystal offset and trim | 7 | #11 |
| ACK turnaround | 16 | #18 |
| RAM budget | 17 | #16 (closed) |
| Radio deadlines against flash work | 19 | #22 |
| Self-update sizes | 20 | #23 |
| Listen-before-transmit delivery | 25 | #20 |
| Entropy | 26 | #14, #35 (closed) |
| Storage of key and counters | 27 | #37 (network, counters), #21 (identity; done), #39 (MAC, v2) |
| Beacon answer | 28 | #19 |
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
| C library | Full newlib; `nano.specs` commented out (`cmake/toolchain-arm-none-eabi.cmake`) | Enable `nano.specs` to save flash for the size check (open question 20, #23) |
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
| P8 Code clean-up | `fw-critical` (Opus, high) | `fw-reviewer`; `log-reader` for size reports |
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
- RAM at runtime: FreeRTOS heap free 25632 of 32768 bytes (minimum the same); unused stack words radio/net/hk 489/489/352 of 512 (early data point for open question 17, not a conclusion).
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
- **Done when:** frames from nearby Tado devices arrive with CRC OK, and flash and radio both work on the shared bus. Settles open question 7.
- **Status (2026-11-12, [V]): done except the crystal offset.** The receive-only run met the frame and shared-bus criteria (1115 of 1119 CRC OK, 30 of 30 flash ID checks; `radio-runtime.md`, `hardware.md`). `FREQEST` is now read inside the packet and is valid for the stock senders. The bridge's own crystal offset and the per-unit trim decision (open question 7, issue #11) need our own transmissions, so #11 moved to P5 (2026-11-16).

### P3: Ethernet

- SPI2 (PB12 CS, PB13-15), INT PB2, RESET PC14.
- ENC28J60 driver: revision ID, link status.
- Bench network: static address on `enp196s0f0`, `dnsmasq`, tanoclo in Docker (`test-bench.md`).
- lwIP with DHCP, DNS and ping.
- **Done when:** the bridge gets an address, answers ping and resolves the server name. The router LED shows link and DHCP.
- **Status 2026-11-14 [V]:** ENC28J60 driver (#12), done: half duplex, link status, frame TX and RX against the laptop (ARP probe answered, 1514-byte frames both ways, 125 of 125 received pings intact), cable pull and full power cycle passed (`hardware.md`, "Result: half duplex, link, frame RX and TX"). lwIP (#13): result below. tanoclo in Docker on the link (#27): done 2026-11-14 (`test-bench.md`). **P3 is done.**

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

**Result 2026-11-14 [V]: done.** With the owner-changed supply, the board rail reads 3.28-3.29 V. Margin test with the firmware's own CS timings: 12000 of 12000 iterations good at 12 MHz and at 6 MHz. SPI2 stays at 6 MHz for a factor of 2 on timing margin. Bring-up passed at the first attempt on 7 consecutive resets, and a 10-minute run showed no `reinit`, no read-back mismatch and 65 of 65 self-test frames sent (`hardware.md`, "Result: margin test and 10-minute run"). The diagnostic code is removed. Bring-up after a full power cycle passed in the #12 run (P3 status above).

### P4: TLS and WebSocket to tanoclo

- Entropy pool from PA3 (`design.md`); log the raw readings to measure the noise (open question 26, issue #14).
- mbedTLS: TLS 1.2, `ECDHE-ECDSA-AES128-GCM-SHA256`, 4 KB in, 2 KB out (`design.md`).
- Connect to tanoclo on port 988, path `/hw/v2`; `auth/key`, `auth/token`, `GET time` (`server-api.md`). Seed the bridge serial in tanoclo first (`tanoclo/ws-server/test/ci_seed.sql:39-67`).
- Measure peak RAM and handshake time (open question 17, issue #16).
- **Done when:** tanoclo shows the bridge connected and authenticated, time is set, and RAM is within budget. The cloud LED shows the connection.

**P4 #15 result (2026-11-14) [V]:**
- tanoclo logs `AUTH OK` and `TIME_SYNC` for the bench bridge, and its `devices` row shows it connected.
- Time is set from `GET time`.
- Handshake 1.07 s.
- Heap free 12824 bytes while connected, 9568 at the lowest point (handshake); bss 56 KB.
- Reconnect after a tanoclo restart: about 3 s; backoff to 32 s during a 40 s outage.

Details: `build.md`, P4 TLS. The cloud LED is wired to the session; the owner saw it steady on with the session up (2026-11-16) **[V]**.

**P4 #16 result (2026-11-16) [V]:** peak RAM 59 KB reserved and about 46 KB used of 128 KB, at the TLS handshake; 69 KB unreserved, against about 11 KB estimated for P5 (`design.md`, "RAM measured (#16, #17)", open question 17). Handshake 1070-1083 ms, 1055-1064 ms CPU, three P-256 steps of 316-366 ms. The server retransmitted its first flight and the `GET time` reply in every connection, because lwIP's delayed ACK waited for its 250 ms timer; the bridge now ACKs every segment at once, and 8 of 8 connections had no retransmission (`design.md`, "Immediate ACK (#16)"). Production image `8e1c455`, sha256 of `build/tado-bridge.bin` `b1f40033a7646cf54c38f0bcf679fed09670a933a9a6ea510885d3063294fd5e`, OpenOCD verify OK. The radio ACK timing during a handshake (open question 16, issue #18) is measured in P5. **P4 is done.**

### P5: Radio link to devices

Restructured in the 2026-11-16 review. The steps run in this order, because each one is the safety gate for the next. Nothing transmits before step 3.

1. **Port, tested on the host (#17).** From freedo (`radio-runtime.md`): frame parse and build, Enhanced ACK, CCM, 6LoWPAN, wake trains, anchors, listen-before-transmit, and the CoAP relay in both directions through the WebSocket frame. The protocol logic is hardware-independent and checked on the laptop against vectors from freedo. Radio buffers are static, not on the FreeRTOS heap (`design.md`, "RAM measured (#16, #17)").
   - Transmit guards at the lowest layer that keys the transmitter: never on channel 26, ACK and answer only frames addressed to our EUI-64, pair only on the test channel, 16 since 2026-11-17, 10 before (`test-bench.md`, "The other tado network").
   - Nothing on the ACK path logs: `log_printf` takes a mutex without a timeout (last open item of #28).
   - The DRBG gets a lock before the radio task uses it (`design.md:151`; open question 26).
2. **Review.** `fw-reviewer` gives a "safe to flash" verdict on the transmit code (`test-bench.md`, gate 4).
3. **First transmissions, no device involved (#11).** Before keying the transmitter: re-check with the SDR that channel 10 is still quiet, and check the regulatory limits of its sub-band (duty cycle against 1 s wake trains, listen-before-transmit); `test-bench.md:98` records that this was not done. Then send test frames on channel 10 and confirm with the SDR: right frequency, deviation and CRC, and no emission on channel 26 (`burstscan.py`). Measure the bench unit's crystal offset with `demod.py` and decide on the per-unit trim. Settles open question 7.
4. **Network and counter records (#37, moved from P6).** Owner decision, 2026-11-16: the network key and frame counters are stored in external flash already in P5, so a paired device stays paired across resets and flashes. Layout as in `design.md`, "Slotted records" and "Counter log". This is a storage-format change, so `fw-driver` writes it and `fw-reviewer` reviews it before it is flashed. A fresh network key comes from the DRBG; it never leaves the bridge or enters git.
5. **Pairing (#19).** Beacon answer, double Router Advertisements, `d/pair` with tag `0x12` (`radio.md`), on the test channel 16 only (owner decision 2026-11-17; channel 10 before), in a 10-minute window (owner spec 2026-11-17) that a pairing does not close, for one device named by its EUI-64. **Status (2026-11-17):** built and host-tested; the committed build has the transmitter on by default since 2026-11-17 (owner decision, `RADIO_TX_ALLOWED` build option) and pairs only with a target named at build time; next `fw-reviewer`, then a bench image with the spare device's EUI-64 (`radio-runtime.md`, "Pairing (#19)"). **Done (2026-11-17/18) [V]:** reviewed (verdict A), flashed, spare device paired on channel 16, downlink and `d/config` reply work (`test-bench.md`, "Pairing test (#19) result"). #17, #19 and #21 are done.
6. **Measurements.**
   - ACK start after the Data frame ends, idle and during a TLS handshake (open question 16, #18). Measured on air with `demod.py`, which timestamps to under 1 µs (`test-bench.md`, "Demodulator"). The pairing LED (PB8) as a marker on a logic analyser is only the fallback if a late ACK has to be traced inside the firmware.
   - Beacon answer time (open question 28, #19).
   - Delivery rate with listen-before-transmit on (open question 25, #20). **Skipped for now (owner decision, 2026-11-18)**; listen-before-transmit stays on.
   - **Status (2026-11-18) [V]:** idle ACK 226 us median, n=31 (`test-bench.md`, "ACK timing (#18)"); beacon 4057 us after the request; ACK inside a TLS handshake 225.2-233.9 us, median 226 us, n=21 of 34 uplinks in handshake steps (`test-bench.md`, "ACK during a TLS handshake (#18)"). #18 closed.

- **Device to test with: one spare real tado device** that is not part of the stock network (owner decision, 2026-11-16; replaces "none chosen yet" of 2026-11-11). The owner names it by its EUI-64, which goes into the gitignored `config/local.cmake`, never into git. tanoclo's ESP32 emulator is not used for now: it sends plain MAC ACKs and no channel scan, and its config defaults to channel 26, the protected channel (`tanoclo/esphome/tado_emulator/tado_emulator.yaml:47`), so it must never be powered up with that default near the stock network.
- **tanoclo changes (#38, owner action point 2026-11-17):** a pull request to tanoclo will be needed in the end. Known item: tanoclo drops a paired device's uplinks until it has a `devices` row (`tanoclo/ws-server/lib/handlers.js:229-232`; open question 28b, answered no [V]); a real flow needs registration from the bridge's pairing report or an admin action. On the bench the row is added by hand to the local database, and `../tanoclo` stays unchanged. Open question 28c.
- **Owner hand work in P5:** put the spare device into pairing mode when step 5 is ready, and again after a factory reset test. With step 4 done, flashing does not unpair it.
- **Done when:** the spare device pairs, reports through to tanoclo and takes a setpoint; it stays paired across a reset and a reflash; ACKs start within 0-1 ms of the end of the Data frame (project decision 2026-11-11, stricter than the stock bridge's measured 1.5-2.0 ms), also during a TLS handshake.

### P6: Storage

- Identity record in external flash (`design.md`); provision the serial and EUI-64 from P0 into it instead of the build-time `config/local.cmake` (#21). The network and counter records moved to P5 (#37). **Status (2026-11-18):** built and host-tested (`design.md`, "Implementation: identity record (#21)"; provisioning with `tools/identity/provision.sh`, `build.md`); reviewed, provisioned and flashed on the bench 2026-11-18, identity record seq 1, kept over a power cycle together with the network record **[V]** (`design.md`). #21 closed; the Ethernet MAC from the original bridge follows as #39 (end of P7).
- Erase and program while radio traffic runs (open question 19, #22). **Status (2026-11-18):** built and host-tested (`design.md`, "Implementation: flash work during radio traffic (#22)"); next `fw-reviewer`, then the bench check (`test-bench.md`, "Flash work during traffic (#22) and factory reset, plan").
- Factory reset (PB10): erases the network and counter records, keeps the identity. **Status (2026-11-18):** built and host-tested, 10 s hold (`design.md`, "Implementation: factory reset (P6)"); next `fw-reviewer`, then the bench test (owner presses the button and re-pairs the spare device).
- **Done when:** identity and network survive power cycles, a factory reset re-pairs cleanly, and no ACK is missed during flash work.

### P7: Size check, bootloader and self-update

- **First the size check (open question 20, #23):** measure the release application size and estimate the bootloader size. Confirm the layout fits, or adjust it. Data point at P4: the application is 131.7 KB of the 448 KB planned for it (`make` size report, 2026-11-16) **[V]**. **Status (2026-11-18): done, the layout fits and is kept.** Release image at `9c36419` 161392 bytes with the transmitter on, 154860 with `RADIO_TX_ALLOWED=0` (35.2 % and 33.8 % of 448 KB) **[V]**; bootloader estimated at 6-8 KB of its 64 KB **[I]**; the 512 KB external update slots hold a full 448 KB image plus header (`design.md`, "Size check (#23)").
- Bootloader at `0x08000000`, application at `0x08010000`, update and rollback as in `design.md`.
- Uploader through tanoclo (`POST /api/send-raw` or one added handler).
- **Last step, Ethernet MAC from the original bridge (#39, owner decision 2026-11-18):** the owner gives the MAC by hand, or it is copied from the stock firmware in the backup (location not yet known, `design.md`); identity record version 2; runs after the bootloader and before P8.
- **Done when:** an update installs, a broken image rolls back, and a power cut during install recovers.

### P8: Code clean-up checkpoint

Owner request, 2026-11-14 (`docs/README.md`, "Project decisions"). Runs after P7. Opus skims the whole repository and rewrites messy or unclear code without changing functionality. Issues: #32 (survey and progress list), #33 (per-module clean-up), #34 (proposal for major changes).

- **Behaviour-preserving only.** No functional change. Verify each step with the build and the size report, and where hardware is involved with the existing bench checks within `test-bench.md`. Any change touching the bootloader, the storage format or radio timing still needs a `fw-reviewer` "safe to flash" verdict before it is flashed.
- **Test-only code:** remove none that the owner wants kept; otherwise the existing rule applies, no test-only code is left in (`docs/README.md`, "Project decisions", 2026-11-11).
- **Small steps, frequent checkpoints.** One commit per file or module, with the build passing, so work can resume from the last commit if usage runs out.
- **Progress list:** a checklist of files and modules (done, todo, needs proposal) lives in #32 and is ticked after each commit, so a later session can continue.
- **Major changes are not made directly.** Architecture, module boundaries, API changes and renames across modules are written up as a plan and proposed to the owner for approval first (#34).
- **Subagents:** `fw-critical` (Opus, high) does the rewriting; `fw-reviewer` reviews.
- **Done when:** every module on the progress list is done or moved to an approved proposal, the build and size report match the pre-clean-up behaviour, and the bench checks still pass.

## Equipment

- ST-Link V2 and access to the SWD test points.
- RTL-SDR, for checking our transmissions and for ACK and beacon timing. A logic analyser or scope is optional (fallback in P5, step 6).
- A tanoclo server on the bench link (Docker on the laptop, `test-bench.md`).
- One spare real tado device, not part of the stock network (P5).

## Plan review (2026-11-16)

Changes made in this review, with the reason for each.

| Change | Reason |
| :--- | :--- |
| P5 split into six ordered steps, with first transmissions before any device is involved | The old list had no order. The transmit guards, the review and the SDR check of our own emissions must come before pairing, to protect channel 26 |
| Test device: one spare real device (owner, 2026-11-16) | The plan said "decide before P5". The emulator also defaults to channel 26 and does not behave like a real device at the MAC |
| Network and counter records moved from P6 to P5 as #37 (owner, 2026-11-16) | Without them the bridge loses its network key at every reset, and a real device would need the owner's hands after every flash |
| ACK and beacon timing measured with the SDR; logic analyser only as fallback | `demod.py` already timestamps frames to under 1 µs on air, with no probing by the owner |
| Regulatory check and a fresh channel-10 scan added before the first transmit | `test-bench.md:98` records both as not done |
| #11 (crystal offset) and the last item of #28 moved to P5; milestones P2, P3, P3.1, P4 closed | Both need transmit code. Nothing else was open in those stages |
| "#N" now always means a GitHub issue; open questions are named as such, with a mapping table | The two number series overlap (issue #16 is open question 17), which already caused one wrong reference (`fed0467`) |
| Stale statements removed: "proposal, nothing flashed yet", P3 "open: #27", P3.1 "still to try" | They contradicted the results recorded below them |

Not changed, but worth knowing:

- **No stage covers daily use.** The plan ends at P8. A soak run of several days, the production server address and CA in place of the bench values, and moving the stock network's devices over are not planned (owner, 2026-11-16: not now).
- **The ACK target of 1 ms is stricter than stock.** `test-bench.md` notes that matching the stock 1.5-2.0 ms would be enough **[I]**; the project decision of 2026-11-11 keeps 1 ms. If P5 measures more than 1 ms but under 2 ms during a TLS handshake, that is a decision for the owner, not a failure of the port.
- **Open questions 10-14** (stock behaviour seen through tanoclo's proxy mode) stay optional. The firmware matches tanoclo, not the stock cloud.
