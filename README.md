# tado-bridge-firmware

Replacement firmware for the tado Internet Bridge (IB01) hardware. It speaks the stock bridge API to a self-hosted server instead of the tado cloud, with [tanoclo](https://github.com/tanoclo/tanoclo) as the server and test harness.

This is heavily AI-assisted work. The code and the documentation were written largely with AI assistance, under the direction of the project owner. Behavior was checked with automated tests, and on a real plant only where the [Status](#status) section says so.

This project is not affiliated with tado. "tado" is used only to describe the hardware it targets.

## Status

P0 to P4 are done and verified on one bench bridge: backup, clock, radio receive, Ethernet, and a TLS and WebSocket session to tanoclo. The bridge now transmits on the test channel 16 (TX-on image flashed and verified on the bench, 2026-11-17/18): a spare device paired and downlink works (P5, done except #11; `docs/test-bench.md`). Do not flash this onto a bridge without a verified backup (see stage P0).

| Stage | Goal | Status |
| :--- | :--- | :--- |
| P0 | Backup and restore rehearsal | Done |
| P1 | Skeleton: clock, LEDs, button, RTT, FreeRTOS | Done (2026-11-11): skeleton and RTT (#6), clock tree (#7), FreeRTOS, LEDs, buttons and watchdog (#8) flashed and verified on the bench |
| P2 | Radio and flash on SPI1 | Done, except the crystal offset (#11, moved to P5): receive-only image verified on the bench (2026-11-12): radio PARTNUM/VERSION, 99.6% CRC OK on channel 26, flash and radio on the shared bus (#9, #10); `FREQEST` now valid in the packet (0 to -6 units on stock senders, #11); per-unit trim decision waits for our own transmissions in P5 |
| P3 | Ethernet (ENC28J60, lwIP) | Done: ENC28J60 driver (#12) done, incl. cable pull and power cycle; lwIP (#13) done (2026-11-14): DHCP lease, DNS through the laptop's dnsmasq, ping both ways with full-size frames, router LED shows link and lease; cable pull with lwIP passed (relink, same lease, no re-init); tanoclo in Docker on the link (#27) done (2026-11-14): ports bound to 10.42.0.1 only, bench CA and IP-SAN certificate outside git |
| P3.1 | Reliable SPI2 bus to the ENC28J60 | Done (2026-11-14): the cause was the ST-Link 3V3 sagging to about 2.7 V; with the owner-changed supply (3.29 V) SPI2 reads 12000 of 12000 correctly at 12 and 6 MHz, runs at 6 MHz for margin, and a 10-minute run had no re-init |
| P4 | TLS and WebSocket to tanoclo | Done: entropy pool (#14) done (2026-11-14): PA3 noise measured (mostly deterministic; 0.87 bit per reading estimated, 0.1 credited), SP 800-90B health tests that fail closed, mbedTLS 3.6.7 CTR_DRBG seeded at boot in 43-45 ms, LWIP_RAND from the DRBG; TLS and WebSocket to tanoclo (#15) done (2026-11-14): TLS 1.2 ECDHE-ECDSA-AES128-GCM-SHA256 with the bench CA and its IP SAN verified, handshake 1.07 s, /hw/v2 upgrade, auth/key, auth/token and GET time accepted by tanoclo, reconnect with backoff after a server restart; a connection holds about 8 KB of heap, and the handshake peaks at about 11 KB. PA3 fail-closed, cold-start and low-supply checks (#35) and the power LED fault code (#36) done (2026-11-16). RAM and handshake time (#16) done (2026-11-16): 59 KB reserved and about 46 KB used at the handshake peak of 128 KB, 69 KB unreserved for P5; immediate TCP ACK removes the server retransmissions (2 per connection before, 0 of 8 after) |
| P5 | Radio link to devices, pairing | Done except the crystal offset (#11, open): MAC and 6LoWPAN port (#17) written 2026-11-17 and host-tested (877,202 checks, 0 failed; text 142640, bss 79392 bytes), with transmit then still switched off in the committed build (`RADIO_TX_ALLOWED` 0), reviewed, flashed and verified (2026-11-17). First over-the-air test done 2026-11-17 (#11): 5 frames on channel 10 only (the test channel then; 16 since 2026-11-17, owner decision), 4 decoded by the SDR, nothing on channel 26, bridge about +0.6 ppm from the stock coordinator so no trim looks necessary (one unit, 4 frames); sender reverted. Network and counter records (#37) built, host-tested, reviewed and flashed 2026-11-17. Pairing (#19) built and host-tested 2026-11-17 (3,116,202 checks, 0 failed; text 150964, bss 80412 bytes; transmitter then still off): pair button, one device named at build time, beacon about 3.9-4.0 ms after the request (designed, not measured); reviewed (verdict A), then changed by owner decisions of 2026-11-17: test channel 16, a 10-minute window that a pairing does not close, pairing LED flashing while waiting and lit while pairing (host-tested; reviewed, verdict A, 2026-11-17). Transmitter on in committed code (owner decision 2026-11-17): `RADIO_TX_ALLOWED` is a build option, default 1, `make RADIO_TX_ALLOWED=0` for a receive-only image; the transmit gate, the network key and the named pairing target are the safety layers; host tests cover both variants (3,119,963 checks, 0 failed; default build text 146576, bss 79932 bytes); `fw-reviewer` verdict A on `3e7f171..52cff30`, also covering `1d4e83c` and `7313ab8` (2026-11-17). Spare device paired on channel 16 on the bench 2026-11-17, beacon 4057 us after the request on air (`docs/test-bench.md`, "Pairing test (#19) result"); tanoclo drops its uplinks until it has a `devices` row (tanoclo pull request collected in #38). Downlink (`z/s`) and the device's `d/config` reply work end to end on the bench (2026-11-17/18). MAC and 6LoWPAN port (#17), pairing (#19) and ACK timing (#18) done. ACK timing measured over the air: bridge ACK 225-239 us after the Data frame, median 226 us, n=31, 0.76 ms margin to the 1 ms target (`docs/test-bench.md`, "ACK timing (#18)"); with a temporary test image, 21 ACKs inside TLS handshake steps also came at 225.2-233.9 us, median 226 us (2026-11-18, `docs/test-bench.md`, "ACK during a TLS handshake (#18)"). Listen-before-transmit delivery measurement (#20) skipped for now (owner, 2026-11-18); listen-before-transmit stays on. Remote pairing trigger from tanoclo (`POST d/I/<serial>/pair`, #40) built and host-tested 2026-11-20, not reviewed or on air yet (`docs/test-bench.md`, "Remote pairing trigger (#40), plan"). Open: crystal offset (#11) |
| P6 | Persistent storage | Started: identity record (#21) done 2026-11-18, provisioned and flashed on the bench, seq 1, kept over a power cycle with the network record; the Ethernet MAC from the original bridge is #39 (end of P7; identity record v2 built and host-tested 2026-11-20, reviewed (`fw-reviewer`: safe to flash), and installed on the bench over the air 2026-11-20 **[V]** with the version 1 record and the placeholder MAC (`test-bench.md`, "Identity v2 image (#39): first flash"); after the owner confirmed that the MAC is this unit's own, provisioned and verified on the bench 2026-11-20 **[V]**: version 2 seq 2 in slot B, boot with the original bridge's MAC, new DHCP lease `10.42.0.13`, session and downlinks fine, kept over three warm resets (`test-bench.md`, "Identity v2 provisioned (#39)"); #39 done, and kept over a power cycle on 2026-11-20 **[V]** (`test-bench.md`, "Power cycle with identity v2")); flash work during radio traffic (#22) and the factory reset (PB10 held 10 s) built, host-tested, reviewed and flashed 2026-11-18; #22 measured on air the same day (87 of 87 uplinks ACKed, 42 during sector erases, 226 us median; one late short train after an erase, fixed and rerun the same evening: 71 of 71 downlinks during forced erases delivered at attempt 1); the factory reset and re-pair checked by the owner by eye 2026-11-18; open: the counter log read-back after a reset; the network and counter records moved to P5 (#37) |
| P7 | Size check, bootloader, self-update | Started: size check done 2026-11-18 (#23), image 161392 bytes of the 448 KB region, bootloader estimated 6-8 KB of 64 KB, layout kept; bootloader (#24) built and host-tested 2026-11-18, 6124 bytes, application moved to `0x08010000`, reviewed (`fw-reviewer` verdict A, 2026-11-18), flashed on the bench 2026-11-19 **[V]** (`test-bench.md`, "Bootloader (#24): first flash and checks, result"); the application's update side (`POST b/fw` Block1, save to the previous slot, `PENDING`, confirm, write guard as three areas) and the uploader `tools/update/upload.py` built and host-tested 2026-11-19, image 161376 bytes without `config/local.cmake` (170392 with the bench config), reviewed (`fw-reviewer` verdict A with findings), M1 and L1 fixed 2026-11-19, delta review verdict A, flashed and run on the bench 2026-11-19 **[V]**: update A → B confirmed, session cut and corrupted transfer refused, a candidate that never confirms rolled back after 3 starts in 30.5 min, ACK gap during the transfer median 226.0 us (`test-bench.md`, "Self-update (#24): first over-the-air update, result"); end of step 5 on the bench 2026-11-20 **[V]**: after the rollback, the running image B was refused, and A installed and confirmed 60 s after session ready (bench left on A, `CONFIRMED`; `test-bench.md`, "End of step 5"); the #39 image (`6da2352`, 171984 bytes) installed over the air from A and confirmed 2026-11-20 **[V]**, now on the bench; the owner's power cut during an install (step 6) passed on the bench 2026-11-20 **[V]**: image 6 (`9e92ac7de0709ab0`) installed after the cut and confirmed 60 s after session ready (`test-bench.md`, "Power cut during an install (#24 step 6), 2026-11-20"); the power-cycle check is done too ("Power cycle with identity v2"); the uploader needs an additive tanoclo change to see answers (#38), else `--blind` |
| P8 | Code clean-up checkpoint | In progress: #33 done (rows 1-20 landed, 21-23 decided by the owner); #34 open, proposals parked by the owner |

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


