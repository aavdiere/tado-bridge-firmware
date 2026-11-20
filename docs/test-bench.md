# Test bench

How the bench IB01 is connected, what Claude may do with it, and how the owner's other tado network is protected.

Owner decisions, 2026-11-11, unless noted. Status tags: **[V]** measured, **[D]** documented, **[I]** inference.

## Setup

| Link | Connection |
| :--- | :--- |
| SWD | ST-Link V2 on the test points: 3V3, GND, SWDIO, SWCLK (`hardware.md`) |
| Power | **USB, owner-changed supply (2026-11-14)**; the ST-Link 3V3 wire is disconnected, the ST-Link carries only GND, SWDIO and SWCLK. Never feed the board from two sources at once (`tanoclo/patch_ib_firmware/README.md:211`). Until 2026-11-14: ST-Link 3V3 only, which sagged (below) |
| Ethernet | Bridge straight to the laptop's wired port `enp196s0f0`, on a private subnet of its own |
| RF | RTL-SDR dongle on the laptop, receiving 863-873 MHz |
| Server | tanoclo in Docker on the laptop, reachable only on the private Ethernet link |

**Power risk:** the ENC28J60 draws much more current while transmitting than the MCU and radio. A clone ST-Link's 3.3 V regulator may sag under that load. The fallback is a current-limited 3.3 V bench supply with the ST-Link wired without its 3V3. That fallback is a decision for the owner.

**The sag is real (2026-11-14) [V]:** with the ENC28J60 PHY active and the link up, the board's VDDA measures 2.67-2.83 V, below the chip's 3.10 V minimum. With the chip in power save it measures 3.09-3.21 V. The ST-Link reports 3.20 V in both cases, so its own reading does not show the sag. Out of range, the chip stops answering on SPI2 (`hardware.md`, "Cause: the supply sags below the ENC28J60's minimum"). Check it with `tools/bench/enc28j60_supply.tcl`.

**Fixed by the owner-changed supply (2026-11-14) [V]:** VDDA 3.28-3.29 V with the PHY on, in power save and on again, measured by `tools/bench/enc28j60_supply.tcl` against the running firmware. The ST-Link still reports about 3.20 V, its own regulator rather than the board (OpenOCD `Target voltage`). P3.1 results: `hardware.md`, "Result: margin test and 10-minute run".

## What Claude may do (owner, 2026-11-11)

Claude may flash the bench bridge and run tests on it, within these gates:

1. **P0 first.** No new firmware until the backup is done twice and compared, read protection is confirmed at level 0, and the restore has been rehearsed. Claude may run P0 itself once the hardware is connected.
2. **Never change option bytes** (read protection, write protection, brown-out level) without asking. A wrong read-protection change can erase the chip or lock it permanently (`design.md`).
3. **Flash with OpenOCD and verify** every write. Keep the backup images ready for a restore.
4. **Review first:** `fw-reviewer` must give a "safe to flash" verdict before radio transmit code (P5) or bootloader code (P7) goes onto the board. Since 2026-11-17 the default build has the transmitter in (`RADIO_TX_ALLOWED` 1, owner decision, `README.md`, "Project decisions"), so every default image is radio transmit code; an image for work that does not need the transmitter can be built receive-only with `make RADIO_TX_ALLOWED=0` (`build.md`, "Build options").
5. **Never reconfigure PA13 or PA14** (SWD). Without NRST on the test points, losing SWD makes recovery hard.
6. **Hardware steps still belong to the owner:** plugging cables, pressing buttons, probing. Ask for them.

## The other tado network

The owner runs a stock tado network nearby at the same time. Our test network must never disturb it.

**Shared parameters [D]:** both networks use the same band, the same PAN ID `0xABCD` and the same address suffix (`radio.md`). They differ only in **RF channel** and **network key**.

Rules:
1. **Find its channel with the SDR before any radio transmit** (P2). Stock defaults to channel 26 but may differ per install (`radio.md`).
2. **Protected channel:** the firmware gets a configured protected channel and refuses to transmit on it. The test channel must be at least 5 channels (about 1 MHz) away **[I]**.
3. **Receive-only on the stock channel.** Listening there to measure stock timing as a reference is fine. Never ACK, answer, wake or replay anything there.
4. **Answer only frames addressed to our own EUI-64.** Freedo's ACK rule already does this (`radio-runtime.md`).
5. **Pairing windows only on the test channel,** and bounded: 10 minutes, for the one device named at build time, with fixed limits on what a window sends (owner spec 2026-11-17). Stock devices only join while in pairing mode, but our Router Advertisements under the pairing key must not reach the stock channel.
   **Pair only a device named by its EUI-64.** A tado device nearby scans channels with Beacon Requests (scan below) and could otherwise join the test network. Freedo's pairing flow already supports naming the target (`freedo/docs/PROTOCOL.md:699-705`).
6. **Fresh network key** for the test network, generated as in `design.md`. Never try to obtain the stock network's key.
7. **Test device: one spare real tado device** that is not part of the stock network (owner, 2026-11-16; until then "no test device yet", 2026-11-11). Its EUI-64 stays out of git. Never power up tanoclo's ESP32 emulator with its default configuration: it transmits on channel 26 (`tanoclo/esphome/tado_emulator/tado_emulator.yaml:47`) **[D]**.
8. **Listen-before-transmit stays on,** which also helps coexistence (`radio-runtime.md`).

## The bench unit's own stock configuration [V]

The backup shows the bench IB01 is configured for **channel 26**, the same channel as the active stock network (`tanoclo/patch_ib_firmware/extract_channel.js` on the external flash backup, 2026-11-11). Whenever its stock firmware runs, the bench unit therefore acts as a tado bridge on the protected channel, and if it ever belonged to the same home it may share that network's key **[I]**.

- **The bench unit no longer runs the stock firmware [V]:** the P1 image replaced it on 2026-11-11 (`make flash`, OpenOCD verify OK). The stock firmware only returns if the backup is restored.
- Re-plugging the ST-Link or a reset now starts our firmware, not the stock one. The CC110L keeps its register state across an MCU reset, so after a test that touched the radio, power-cycle the board so it returns to power-on defaults instead of the stock channel-26 configuration (reviewer request, done after P1).
- The backup and the proven restore bring the stock firmware back if needed; never restore it while Ethernet is plugged in, since it would reach the tado cloud through the laptop's NAT (see Ethernet section) **[I]**.

## Bench tools

- `docs/tools/read_ib01_system_areas.tcl`: option bytes, OTP, unique ID (read-only).
- `tools/bench/io_check.tcl`: drives the LEDs in a fixed order and logs button presses as pin changes; peripheral registers only, undone by the final reset. Keeps the stock firmware halted.
- `tools/bench/enc28j60_probe.tcl`: SPI2 SCK sweep, EREVID and free-pin sweep by hand from the debugger (#12). Restores every register it touches.
- `tools/bench/enc28j60_supply.tcl`: board VDDA from the MCU's VREFINT and SPI2 read success per SCK, with the ENC28J60 PHY on, then in power save, then on again (#31). Works before and after the firmware's bring-up: good reads are counted against a reference read at 187 kHz, either the reset values or the firmware's buffer layout. Halts only between two SPI2 transactions and restores every register it touches, the ENC28J60's bank select and RXEN included; the running firmware continues without a re-init **[V]** (2026-11-14).
- `tools/entropy/pa3_noise.py`: host analysis of raw ADC readings (#14): spread, block-to-block repeatability, autocorrelation, bit bias, SP 800-90B min-entropy estimates and health-test cutoffs (`design.md`, "Entropy: measurement"). Raw readings are read out of RAM over SWD by temporary firmware code, and stay outside git.
- `tools/sdr/chanscan.py`, `tools/sdr/frames.py`: channel activity and tado frame decoding.
- `tools/sdr/demod.py`: per-frame GFSK demodulator with sub-microsecond timestamps, carrier offset and MAC header fields (#26). Usage and accuracy below.
- `tools/sdr/burstscan.py`: power-versus-time scan of a whole capture; every energy burst per channel must be a decoded frame, spill-over from a stronger channel, or is listed with duration, centroid, width and symbol rate. Used to confirm that a receive-only image emits nothing.
- `tools/sdr/ackgap.py`: Data end to Enhanced ACK preamble per frame from a `demod.py --csv` file, uplinks and downlinks apart, unanswered uplinks listed, each pair on host time and tagged when it falls in a TLS handshake from the RTT log (#18). Usage and results: "ACK timing (#18)".
- `tools/sdr/timedcap.py`: writes `rtl_sdr`'s output to a file plus `<file>.times.csv` (host time per byte offset, about every 0.1 s), for a capture without hopping.
- `tools/sdr/rttlog.py`: the RTT stream from OpenOCD's port 19021 with a host-time prefix per line; read-only.
- `tools/sdr/hs18place.py`: for the temporary #18 test image, places each bridge ACK inside a TLS handshake step, between steps or outside, from the RTT `rx: t` and `hs18:` ticks, and checks the tick against the air time. Usage and result: "ACK during a TLS handshake (#18)".
- `tools/sdr/burstzoom.py`: per-pulse close look at one stretch of a capture (edges, carrier, tone/sweep/FSK, -10 dB width, optional PNG), for bursts `burstscan.py` cannot explain.
- `tools/bench/flash22_force.py`: forces counter-log sector erases on the running bridge for #22, through the telnet port of the OpenOCD that `tools/rtt.sh` started: `mdw` reads and two `mww` writes (`ctr.next`, then `limit`), never a halt, reset or flash command. It forces only when a frame counter has been used since the last reservation and the previous lowering has been taken, checks the counter log's RAM state after every append, and stops on anything unexpected. Usage and the aim modes: its docstring and "Flash work during traffic (#22) result". With `--put-log` it aims at a downlink just queued (from the PUT driver's response lines), and with `--csl-state` it logs the coordinator's state and hold at each write (used in "Flash work during traffic (#22) rerun").
- `tools/sdr/flash22place.py`: places each uplink and the bridge's ACK inside or outside the forced flash windows of `flash22_force.py`'s log, from a `demod.py --csv` file and `timedcap.py`'s host times.
- `tools/sdr/flash22down.py`: checks each bridge downlink against the forces of `flash22_force.py`'s log: on air (wake-up frames in its train, the device's ACK) and in the RTT (`tx:` attempt, result, wake-up frames; `no ack` lines), per force and in total. No address is printed.

- `tools/update/upload.py`: the bridge's firmware update over the server session (#24): `info`, `upload`, `status` through tanoclo's `POST /api/send-raw`, `--docker tanoclo-ws` for the API's localhost rule, `--blind` while tanoclo cannot return the bridge's answers (#38). Sends to the bridge only through tanoclo, never on air. `design.md`, "Uploader and tanoclo (#24, #38)".

## RTL-SDR uses

| Use | How | Settles |
| :--- | :--- | :--- |
| Find the stock channel | Power scan across 863-873 MHz, then demodulate to confirm sync word `D3 91` | Protected channel |
| Check our own transmissions | IQ capture, GFSK demodulation, sync and CRC check | P2/P5 bring-up |
| Crystal offset | Carrier offset of our frames against channel centre | #7 |
| ACK and beacon timing | Gap between the end of a Data frame and the start of our ACK, from the IQ envelope | #16, #28 |
| Stock reference timing | Receive-only on the stock channel | Comparison for #16, #25 |

Tools present: `rtl_test`, `rtl_sdr`, `rtl_power`, `rtl_fm`; Python with numpy and scipy for a small GFSK demodulator (50 kbps, deviation about 25 kHz, sync `D391D391`, `radio.md`). The `dvb_usb_rtl28xxu` kernel module must not claim the dongle; blacklist it if it loads.

**SDR captures stay out of git.** They contain the stock network's traffic. `.gitignore` and `tools/check-no-firmware.sh` refuse common IQ formats.

## Channel scan result (2026-11-11) [V]

Receive-only, RTL-SDR (R820T, gain 30 dB, 2.4 MS/s), tools `tools/sdr/chanscan.py` (burst-aware power per channel) and `tools/sdr/frames.py` (GFSK demodulation, sync `D391D391`, radio CRC check). Nothing was written to disk.

**Stock network: channel 26 (868.3237 MHz). This is the protected channel.**

| Channel | Finding |
| :--- | :--- |
| 26 | Strongest signal, 57 dB above the noise floor. 84 CRC-valid tado frames in 30 s: wake-up 55, broadcast 20, data 4, Enhanced ACK 4, Beacon Request 1. Mean carrier offset +3.5 kHz (dongle error included) |
| 11, 16, 21, 29 | One Beacon Request each, all near +14 kHz offset: probably **one tado device scanning channels** for its bridge, not part of an active network |
| 20, 24, 25, 27, 28 | Many bursts in the power scan but **no** tado frames: other users of the band. These frequencies match common LoRaWAN channels (867.1-867.9, 868.1-868.5 MHz) **[I]** |
| 0-10, 39-49 | Almost no activity in a 60 s power scan per span |

**Test channel: 16 (866.3242 MHz), owner decision 2026-11-17.** It replaces channel 10 (865.1245 MHz, confirmed by the owner 2026-11-11, used for the first transmit test below): the spare device scans channels 11, 16, 21 and 26 for a network and never asked on 10, so it could not pair there. Channel 16 is 10 channels (2.0 MHz) from the protected channel, inside tado's own channel plan, which stock bridges also pick from, and inside the 865-868 MHz band like channel 10. The scan above saw one Beacon Request on it (a scanning device) and no other user; it has not been checked since **[I]**: re-check its activity with the SDR before the first transmission on it. The transmit gate allows channel 16 only and refuses everything within 999 kHz of channel 26 (`src/txgate.h`); a network record for channel 10 is refused like any other channel (`store_net_allowed()`, `src/store.c`). Since 2026-11-17 the driver enforces 30 s of airtime per rolling hour on every frame, against the 1 % (36 s) of the 865-868 MHz band **[I]** (`radio-runtime.md`, "Airtime").

Frequency offsets include the dongle's own crystal error, which is not yet calibrated, so they do not give the bridge's offset (#7) yet.

## Demodulator `tools/sdr/demod.py` (#26)

Receive-only. Reads rtl_sdr's unsigned 8-bit IQ from a file or stdin and prints one line per frame: sync start, preamble start and end of the last CRC bit (seconds from stream start), envelope edges, length, radio CRC result, RSSI (dBFS, uncalibrated), SNR, carrier offset, deviation, bit-rate error of the sender, frame type, sequence number, addresses and payload hex. `--redact` hides addresses and payload, `--summary` adds counts, offsets per sender and the ACK, wake-up and beacon timing statistics, `--csv` writes one row per frame for later analysis.

```
# Capture first, then decode: a pipe that falls behind loses samples silently
rtl_sdr -f 867923700 -s 2400000 -g 20 -n 720000000 ~/ib01-captures/ch26.cu8
python3 -I tools/sdr/demod.py --fc 867923700 --channels 26 \
    --file ~/ib01-captures/ch26.cu8 --redact --summary

# Live (runs about 4x faster than real time at 2.4 MS/s)
rtl_sdr -f 867923700 -s 2400000 -g 20 - | python3 -I tools/sdr/demod.py --fc 867923700 --redact
```

Tune 400 kHz below the channel (`--fc` must equal `rtl_sdr -f`) to keep the dongle's DC spike out of the band. For the test channel 16 use `-f 865924158` and `--channels 16` (for channel 10, the test channel until 2026-11-17: `-f 864724451`, `--channels 10`). Captures, CSV files and printed payloads stay in `~/ib01-captures/`, never in git: they carry the stock network's addresses.

**How it measures [V]/[I]:** 2.4 MS/s, channel filter and decimation to 600 kS/s (1.667 µs per sample, 12 samples per bit), FM discriminator, one-bit matched filter, sync search with at most 2 bit errors. The sync start comes from a least-squares fit of every bit-boundary zero crossing in the frame (linearly interpolated between samples), with the pattern-dependent GFSK crossing shift modelled from the neighbouring bits; filter delays are removed, so times refer to the raw IQ sample index. Offset and deviation come from a fit of the bit-centre frequency with neighbour terms, against the channel centre the stock registers program (channel 0 = 863.124939 MHz, spacing 199.951172 kHz, `radio.md` register table). Radio CRC: CRC-16 poly `0x8005`, init `0xFFFF` over length and frame (`frames.py`).

**Accuracy [V]:** synthetic GFSK frames with known timing (BT 0.5, 25.39 kHz deviation, random offsets ±15 kHz, gaps down to 0.3 ms): all 111 frames decoded at 16-42 dB SNR, sync-start error std 0.02 µs at 42 dB, 0.06 µs at 22 dB, 0.11 µs at 16 dB, worst case 0.6 µs; offset error std 3-7 Hz; bit-rate error std about 7 ppm on 35-60 byte frames, 20 ppm on 12-20 byte frames. On air, one device's ACK turnaround repeated within 0.2 µs (std, n=3), so the timestamps are good to well under 1 µs; the 10 µs target holds with a wide margin. Time intervals scale with the dongle's crystal error (tens of ppm), which is negligible for millisecond gaps. Absolute offsets still include the uncalibrated dongle error; offsets between senders do not.

## Stock network reference (2026-11-11) [V]

Channel 26, receive-only, 300 s capture plus 10 s and 30 s runs, `demod.py` as above, gain 20 dB. No address of any device is recorded here.

| Item | Result |
| :--- | :--- |
| Frames | 1447 sync hits, **1413 CRC OK (97.7%)** in 300 s: wake-up 934, broadcast 301, Enhanced ACK 88, Data 90. A live 30 s run: 51 of 57 (89%), including 3 Beacon Requests from one device, unanswered |
| Senders | Coordinator plus 6 devices. All frames: 32-bit preamble, deviation 25.4 kHz, no sync errors among CRC-OK frames |
| Carrier offset, relative to the dongle | Coordinator **+5.2 kHz**; devices +8.2, +8.3, +9.4, +10.7, +12.8, +13.3 kHz. Spread across devices 5.1 kHz (5.9 ppm). Each sender drifts within about 0.5 kHz (5-95 %) over 5 min, the same for all, so that drift is the dongle's |
| Bit-rate error, relative to the dongle | Every device's bit clock agrees with its carrier offset within 1.4 ppm (e.g. +15.3 ppm RF, +14.0 ppm bit rate). **The coordinator does not:** +6.0 ppm RF but **-30.6 ppm** bit rate (n=33 frames of 40+ bytes, standard error 0.6 ppm) |
| Uplink ACK (coordinator answers a device) | Data end to ACK preamble start **1.50-1.99 ms**, median 1.91 ms (n=56), spread fairly evenly over that range |
| Downlink ACK (device answers the coordinator) | **350-393 µs**, median 373 µs (n=32); spread per device 2-14 µs (std) |
| Wake-up trains | Step sync to sync 3.94-3.99 ms or 4.40-4.45 ms, in clusters about 7-10 µs apart with 1.4-3.3 µs spread each (the coordinator's own timing). Data frame starts 0.4-3.8 ms after the last wake-up ends, median 1.4 ms (n=34) |
| Beacon answer (#28) | Not seen: the coordinator was not in pairing mode and did not answer Beacon Requests |

**What this means [I]:**
- **#16:** the stock coordinator starts its Enhanced ACK 1.5-2.0 ms after the Data frame ends, and devices accept that. The "well under 1 ms" target in `radio.md` is stricter than stock; matching stock is enough. Devices answer in about 0.37 ms.
- **#7:** the coordinator's bit clock runs about 37 ppm slower than its carrier offset implies, while every device's two figures agree. The likeliest reading: the coordinator's crystal is far off (about -42 ppm against the devices' average bit clock), and its RF is pulled back to within about 6 ppm of the devices by a frequency trim that stock applies after the logged init, which writes `FSCTRL0=0` (`radio.md`, register table; freedo's trim: `freedo/docs/PROTOCOL.md:1670-1718`). Alternatively the coordinator does not use the logged FREQ or data-rate values. Either way a per-unit trim is probably needed on the IB01. The bench unit's own crystal is still to be measured, from our first transmissions on channel 10 (P5) with this tool.
- `tools/sdr/hopcap.py`: one RTL-SDR at 2.4 MS/s alternating between two views. For the first transmit test (`81972b7`): A 864.724 MHz (channels 3-13) and B 867.924 MHz (channels 19-29), hop after each channel-10 frame, an arm word written over SWD and automatic disarm on unexpected bursts; that safety stop false-alarmed on a strong stock frame (see "First transmit test"). Since 2026-11-17, for the pairing test on channel 16: A 865.924158 MHz (channels 9-19), B 867.923670 MHz (channels 19-29), A held while channel 16 is busy, no memory write; see "Pairing test (#19), plan".

## P2 receive-only image on air (2026-11-11/12) [V]

Check that the P2 image (CC110L in RX on channel 26, `RADIO_TX_ALLOWED` 0, `docs/radio-runtime.md:115`) emits nothing. RTL-SDR at fc 867.9237 MHz, 2.4 MS/s, gain 20 dB, span 866.72-869.11 MHz (channels 19-29), `burstscan.py` against `demod.py` frames. Captures: before our firmware (16:09, P1 running, radio untouched), across the P2 flash (18:57), and an A/B pair with P2 running (A) and the MCU halted by OpenOCD `halt` for 108 s (B). Then (C, 2026-11-12) a 239 s capture with the bridge unpowered from t = 7.8 s to 228.9 s: the owner unplugged the ST-Link, and the times come from polling its USB presence.

| Item | Result |
| :--- | :--- |
| Narrowband tones (LO leak, spurs) | None more than 6 dB over the floor in any capture |
| Channel 26 | Every burst is a decoded tado frame or a collision of two, recovered by re-decoding |
| Channel 24 | Bursts every 504 ms, 9.4 ms long, 74 kHz below the channel centre, 23 kHz symbol rate. Also before our firmware: a neighbour |
| Channels 22, 29 | Single 20 ms events, also before our firmware |
| About 868.48 MHz (channel 27) | Every 6.103 s: a train of 19 pulses (1.97 ms each, every 6.43 ms), then 1.27 s later a short exchange between two transmitters; a weaker train from a second source runs 0.15-0.2 s earlier. 2-FSK with a comb of lines about 35 kHz apart, about 70 kHz transition rate: neither our register table nor tado's PHY. **Not ours:** same period (6.1034 s) and phase with the MCU halted, phase-continuous through the P2 flash and reset, present (looser cadence, same pulse shape) before our firmware, and it keeps going with the bridge unpowered (C: 2397 bursts in the 221 s without power, cadence about 4.9 s). 6.103 s = 200000/32768 s points to a 32.768 kHz timebase, which the bridge does not use |

**Result:** no emission from the bench unit seen with the P2 image running, halted or unpowered. Every remaining burst belongs to other users of the band.

## First transmit test (2026-11-17) [V]

First over-the-air transmission of the bridge (#11, P5). Captures stay in `~/ib01-captures/tx1/`, outside git.

**Setup.** `fw-reviewer` gave "safe to flash" for the TX-off images and for a temporary sender image (`ab6602f`). Its rules: unicast frame control 69 EC only, a made-up locally administered destination, dummy payload and MIC, listen before each frame, at least 5 s apart, at most 60 frames per 5 min, armed only by an SWD magic word. The sender was reverted in `3b4ea7a`; the tree equals `279b71a` again and the TX-off image was flashed and verified afterwards. Before: boot log showed bring-up calibration in 1 try (FSCAL3 ef, FSCAL2 2b, FSCAL1 19, PLL locked), RX on channel 10 (865124449 Hz), PATABLE 26; 60 s logs with airtime 0 of 30000 ms, stale 0, overflow 0, radio stack 376 of 640 words never used (348 on the earlier `88c7894` TX-off run), heap minimum 8976.

**Pre-check.** Channel 10 quiet (0 bursts in 65 s); channel 26 baseline 244 CRC-OK frames in 60 s from 7 senders.

**Run.** 5 frames sent (sequence 00-04, all "sent (ok)", 0 listen-before-transmit skips, 8311 us airtime charged each). The `hopcap.py` safety stop fired at t = 39.96 s on a +45 dB burst on channel 6; offline decoding showed a CRC-OK Beacon Request from a stock device, so it was a false alarm. The sender was disarmed over SWD; the bridge emitted nothing outside channel 10. The SDR decoded sequence 00, 01, 03, 04; 02 fell while the dongle was hopped to B (B showed nothing on 866.7-869.1 MHz then).

| Item | Result |
| :--- | :--- |
| Frame | Frame control 69 EC, correct sequence, payload text, CRC OK, 32-bit preamble, 0 sync errors |
| Deviation | 25.4 kHz |
| Duration | 8.162 ms preamble to CRC (51 bytes) |
| Spacing | 4.99994 s, 10.0999 s (two slots), 5.01196 s |
| Level | -3.9 dBFS, SNR 59.7 dB; about 4 dB below the strongest stock device at the dongle |
| Channel 26 | Only decoded frames from stock devices, nothing on a 5 s cadence: pass |
| Offset | +5815 Hz (std 33 Hz, 4 frames) = +6.72 ppm above 865.1245 MHz against the uncalibrated dongle; the stock coordinator measured +6.09 ppm on the same dongle, so the bridge is about +0.6 ppm from the stock coordinator and 3-8 ppm below stock devices |

**[I]** No per-unit trim appears necessary (`FSCTRL0` stays as stock); one unit, 4 frames.

**Observation.** The stock coordinator was last heard at 10:11:30, before the arm at 10:18:16 (the bridge's tx frame count was 0 every minute until then). Afterwards stock devices sent Beacon Requests on channels 3, 6, 11, 21, 26, 29. Not caused by our transmissions; the owner is asked to check the stock bridge (open question 31).

**Duty cycle.** 8311 us per frame; the 1 % basis in 865-868 MHz (30 s per hour budget) is still **[I]**, awaiting owner acceptance.

## Pairing test (#19), plan [I]

Run on 2026-11-17; see "Pairing test (#19) result" below. The code is in `radio-runtime.md`, "Pairing (#19)". Gates before the image goes onto the bench bridge: a `fw-reviewer` "safe to flash" verdict on the pairing code (rule 4), then a local image whose target is never committed:

1. `config/local.cmake` (gitignored): `PAIR_TARGET_EUI64` = the spare device's EUI-64, in the order of `BRIDGE_EUI64` (`00 1b c5` first; the source address of its Beacon Requests in `~/ib01-captures/spare/`, read backwards). CMake refuses any other prefix and the bridge's own EUI-64.
2. The transmitter is built in by default since 2026-11-17 (`RADIO_TX_ALLOWED`, CMake option, default 1; owner decision, `README.md`, "Project decisions"); nothing to edit. Check that the boot log's first line says `TRANSMIT image (RADIO_TX_ALLOWED 1)` and the second `pairing for the device named in the build only`. (Until 2026-11-17 this step was editing `RADIO_TX_ALLOWED` to 1 in `src/cc110l.h`, locally only, as for the first transmit test.)
3. SDR before arming (`rf-bench`): channel 16 quiet, channel 26 baseline. Done 2026-11-17 for channel 16 ("Step 3 baseline, channel 16" below). `hopcap.py` is retuned for channel 16 (owner decision 2026-11-17, test channel 16); commands below.
4. Owner: hold the bridge's pair button for 1 s, then put the spare device into pairing mode (the window lasts 10 minutes, the device's own about 2). The pairing LED (PB8) flashes slowly (500 ms on, 500 ms off) while the window waits, flashes quickly (100 ms on, 100 ms off) while the spare device is pairing (from the beacon until its 2.04, a failure or 30 s without progress), after a success is fully on for 2 s, then flashes slowly again, and goes off when the window closes after 10 minutes; a failed exchange goes straight back to the slow flash (owner spec amended 2026-11-18; seen by the owner on 2026-11-18, fine [V, owner by eye], see "Factory reset and LEDs, owner check"); a pairing does not close it (owner spec 2026-11-17). The first network is created at that moment if the flash holds none (this erases the stock configuration sector at `0x102000`, owner decision 2026-11-17).
5. Read: RTT `pair:` lines (beacon STX time, window closed, `PAIRED`), the minute counters; SDR: request-to-beacon gap (2.4-7.4 ms), the advertisement pairs (`59 e8` then `49 e8`), `d/pair` and the device's 2.04, nothing on channel 26. Stock devices also send Beacon Requests on channel 16 (baseline below): every Enhanced Beacon must follow a request from the spare device, none a stock device's.
6. Optional, scope: PB8 against GDO0 (`radio-runtime.md`, "Measuring it").

**Step 3 baseline, channel 16 (2026-11-17, 21:17-21:31) [V].** Receive-only, the bridge and the ST-Link not touched. `rtl_sdr` at 2.4 MS/s, tuner gain 19.7 dB: 62 s at fc 865.924158 MHz (channels 9-19; 21:17:54-21:18:57), 32 s at fc 867.923670 MHz (channels 19-29; 21:18:57-21:19:30), then 120 s more at 865.924158 MHz (21:28:36-21:30:36) and a 45 s `hopcap.py` dry run (21:27). `demod.py` on 10-18 and 20-28, `burstscan.py` on all (427 us rows, bursts >10 dB over the channel median within +-70 kHz). Times from `demod.py` sync fits are good to well under 1 us; burst times to one 427 us row. Captures in `~/ib01-captures/pairing-baseline16/`. No address is recorded here; senders are told apart by a hash of their source address.

| Item | Result |
| :--- | :--- |
| Noise floor, +-70 kHz | Channel 16 median -54.9 dBFS (10-90 % -55.9 to -53.9); channel 26 -54.8 dBFS; channels 9-29 -54.0 to -55.8 dBFS. No narrowband tone >6 dB over the floor at either fc |
| Channel 16, first 62 s | 15 bursts, busy 0.15 %, all explained: 5 CRC-OK Beacon Requests from the spare device (below), 5 spill-over of its requests on channel 11 (+17 dB on channel 16), 5 its request on channel 21 folded in from beyond the view's edge (+16 to +17 dB on 16; decoded from the fold at -39.9 dBFS, CRC OK, same offset). No other transmitter, no Enhanced Beacon |
| Channel 16, 120 s more | 4 CRC-OK Beacon Requests from **two stock devices** (2 each, -28 and -14 dBFS, +6.0 and +12.4 kHz), each 1.008 s after the same device's request on channel 11, all in the first 15 s; the same two devices that scanned at 20:22 (channel-10 baseline below). A short 8 s `hopcap.py` run at 21:28 saw one more (-29.5 dBFS, +6.1 kHz). Nothing else decoded |
| Spare device on 16 | This run: 5 requests, 21:18:01-21:18:33, 27 bytes, 6.08 ms preamble to CRC, spacing 7.99998-8.00004 s, each 1.0078 s after its request on channel 11, -4.1 dBFS, SNR 59 dB, offset +11955 to +12233 Hz (mean +12077, std 98, falling during the 32 s), then none in the last 22 s. 2026-11-16 (`~/ib01-captures/spare/pair2`, fc 865.724 MHz): 5 requests, spacing 7.99998-8.00004 s, +10843 to +10872 Hz (mean +10853, std 10), -0.1 to +0.4 dBFS, i.e. at full scale. Offsets are against the uncalibrated dongle (+12.5 and +13.9 ppm); the stock coordinator measured +5.15 and +5.24 kHz at 868.3 MHz today (+5.2 kHz on 2026-11-11), so the dongle did not move and the spare's 1.2 kHz change is its own |
| Spare device's scan | One channel per 1.0078 s, cycle 8.0 s, 5 cycles (about 40 s) per scan, in both sessions. 1.008 s before each request on 11 a weak frame folds into the view with the spare's offset (+12.2 kHz) but a failed CRC: its fold position fits channel 6 or 30, so the order is probably 6, 11, 16, 21, 26 **[I]** |
| Channel 26 | 409 of 410 sync hits CRC OK in 32 s (wake-up 357, broadcast 34, Enhanced ACK 9, data 9) from 7 senders, busy 6.1 %; every burst a decoded frame or spill-over. Coordinator present (bit rate -32 ppm against its carrier, as before): uplink ACK 1.52-1.89 ms (n=4), device ACKs 332-395 us (n=5). No Beacon Request in this run. The dry run's 7 s on B at 21:27: 63 of 63 CRC OK, coordinator present |
| Edge of the A view | Channels 17-19: bursts of 10-17 dB, up to 20 ms, about 200 kHz wide, none above 10 dB on 16: probably the non-tado users near 867.1 MHz just outside the view **[I]** |

**What this means [I]:** channel 16 has no user besides tado devices that scan for a network: the spare device and, at times, two stock devices. Our bridge answers only the spare device's EUI-64, so the stock devices' requests must stay unanswered. A spill-over or fold-in of +17 dB on channel 16 is not a frame; `hopcap.py` ignores it for its hold (below), `demod.py` does not decode it. The spare device's level at the dongle varies with its position (full scale on 2026-11-16, -4 dBFS today): if `clip` in `hopcap.py`'s burst lines rises above 0, move it away from the dongle or lower `--gain`.

**Step 3 baseline, channel 10 (2026-11-17, 20:21-20:23) [V], before the test channel moved to 16.** Receive-only, the bridge not touched (TX-off image). `rtl_sdr` at 2.4 MS/s, tuner gain 19.7 dB: 65 s at fc 864.724451 MHz (channels 3-13), then 60 s at fc 867.923670 MHz (channels 19-29). `demod.py` on 4-12 and 20-28 (its usable span), `burstscan.py` on all (256-point Hann FFTs, 4 averaged: 427 us rows, bursts >10 dB over the channel median within +-70 kHz). Captures in `~/ib01-captures/pairing-baseline/`.

| Item | Result |
| :--- | :--- |
| Noise floor, +-70 kHz | Channel 10 median -55.1 dBFS (10-90 % -56.1 to -54.1); channel 26 the same, -55.1 dBFS; all channels -54.2 to -55.9 dBFS. No narrowband tone >6 dB over the floor at either fc |
| Channel 10 | **Quiet:** 0 sync hits with CRC OK (3 noise sync hits), 10 bursts: 9 spill-over from stock Beacon Requests on channel 11 (+42 dB there), 1 of 0.43 ms at +10.3 dB that is a wideband impulse seen on channels 10-13 at the same instant. Below `hopcap.py`'s 12 dB threshold; a 25 s `hopcap.py` dry run saw 0 channel-10 bursts |
| Channel 26 | 235 CRC-OK frames in 60 s (wake-up 151, broadcast 54, data 14, Enhanced ACK 13, Beacon Request 3) from 8 senders, busy 2.45 %; every burst a decoded frame or spill-over. The stock coordinator is back: 7 wake-up trains, uplink ACK 1.66-1.93 ms after the Data frame, device ACKs 0.36-0.39 ms. Strongest device at +0.8 dBFS: 0.15 % of samples clipped at gain 19.7 dB |
| Beacon Requests | Two stock devices (not the spare) scanned 3, 6, 11, 21, 26, 29, one channel per 1.008 s, cycle 8.0 s, from about 20:22:11 to 20:22:52; none after. No Enhanced Beacon decoded in reply |
| Other users | Channel 27 near -33 kHz, 70 kHz transition rate: the known non-tado neighbour ("P2 receive-only image on air") |

**`hopcap.py` during the pairing test (retuned 2026-11-17).** View A at 865.924158 MHz (channel 16 at +400 kHz, channels 9-19), view B at 867.923670 MHz (channel 26 at +400 kHz, channels 19-29). The arm word and `--arm-after` are gone: the address `0x20007990` belonged to the reverted sender image and is ordinary RAM in the pairing image, and the script writes nothing to the bridge. Alerts are only logged (`ALERT`); `--halt-on-alert` adds an OpenOCD `halt` and nothing else. Channel 26 is watched on B only (from A it folds onto the centre frequency), so read the `BURST B ch26` lines and decode the B file afterwards. Hop policy (`tools/sdr/hopcap.py` docstring): A is held while channel 16 is busy, and B is visited only 5.0-5.5 s after the last channel-16 burst, or after 9 s without one, for 2.0 s, so that a visit ends before the spare device's next request 8.0 s on and never falls in the 3 s before `d/pair` or between the trains of an advertisement pair. Rows of spill-over or under +25 dB do not hold A: the +17 dB that the spare's requests on 11 and 21 put on channel 16 otherwise moved the B visit onto its next request. Replayed through the script with the channel-16 capture at five start phases, all 5 requests on channel 16 fell on A; with channel 16 quiet, A gets about 80 % of the time (9 s A, 2.0 s B, 28 ms retune and 80 ms dropped per hop; 76 % measured in the 45 s dry run with an earlier 6 s A setting). The live burst lines are good to one 427 us row.

Pairing test, SDR side (start before the owner holds the pair button; captures stay outside git):

```
mkdir -p ~/ib01-captures/pair16
# RTT with a host-time prefix, for hopcap's beacon check (tools/rtt.sh running)
bash -c 'exec 3<>/dev/tcp/127.0.0.1/19021; cat <&3' | python3 -I -u -c 'import sys, time
for l in sys.stdin: print("%.3f %s" % (time.time(), l.rstrip()), flush=True)' > ~/ib01-captures/pair16/rtt.log &
python3 -I tools/sdr/hopcap.py --out ~/ib01-captures/pair16/run --seconds 900 --gain 197 \
    --rtt-log ~/ib01-captures/pair16/rtt.log | tee ~/ib01-captures/pair16/run.log
```

900 s covers the 10-minute window plus the start; about 3.2 GB on disk. Afterwards, the request-to-beacon gap (#28) and the exchange on channel 16, then channel 26:

```
python3 -I tools/sdr/demod.py --fc 865924158 --channels 16 --file ~/ib01-captures/pair16/run.A.cu8 \
    --redact --summary --csv ~/ib01-captures/pair16/run.A.frames.csv
python3 -I tools/sdr/demod.py --fc 867923670 --channels 26 --file ~/ib01-captures/pair16/run.B.cu8 \
    --redact --summary --csv ~/ib01-captures/pair16/run.B.frames.csv
```

The gap is the summary line `beacon-req end -> enh-beacon preamble start, us` (expect 3.8-4.1 ms, accept 2.4-7.4 ms; `--channels 16` alone, so that request and beacon are consecutive in the frame list). A request and its beacon never straddle two A stretches, since a hop needs 5 s without a burst. File times map to stream and host time through `run.segments.csv`. For a capture without hopping, `rtl_sdr -f 865924158 -s 2400000 -g 19.7 <file>` and the same `demod.py` line: `-f` and `--fc` must be equal, and `--channels` belongs to `demod.py`, not to `rtl_sdr`.

## Pairing test (#19) result (2026-11-17) [V]

The spare device paired with the bench bridge on channel 16 and stayed in the test network. Image: the local pairing build (`RADIO_TX_ALLOWED` 1, the spare device named in `config/local.cmake`, nothing committed). The flash held no network record, so the button created one on channel 16 (`~/ib01-captures/pair16/rtt.log:34-37`). Analysis was offline afterwards; the bridge and the ST-Link were not touched for it.

**Method.** `hopcap.py` as above, 900 s, gain 19.7 dB. A was on air 740.8 s (82.3 %), B 141.8 s (15.8 %), and 150 retunes lost 1.9 %. During the 600 s window, A covered 82.7 % and B 15.4 %. Decoders: `demod.py` (the commands above, plus channels 10-15, 17 and 18 on A and 20-25, 27 and 28 on B), `burstscan.py` on both files, and the new `tools/sdr/hoplist.py`, which maps file times to stream and host time through `run.segments.csv`. The mapping is exact in samples. Host time comes from hopcap's START line and is good to a few ms; the bridge's RTT lines come 18-105 ms after each frame's end. Frame times are `demod.py` sync fits at 600 kS/s (1.667 us per sample, 12 per bit), good to well under 1 us ("Demodulator", accuracy). Burst times are good to one 427 us row. Senders are named by role here; no address is recorded. Stream times are seconds from hopcap's start, at 21:42:20.

**Exchange on channel 16, all on A:**

| Stream s | On air | Source |
| --: | :--- | :--- |
| 78.69 | Window opens (no frame) | `rtt.log:37` |
| 79.76-81.81 | Two 1 s trains, then Router Advertisement `59 e8` seq 00 and `49 e8` seq 01, 108 bytes each: pair 1, sent 1.07 s after the window opened | `rtt.log:40-41` |
| 85.1610 | Beacon Request from the spare device, 27 bytes. Its request 8.0 s earlier fell in a B visit, came before the window, and was seen folded onto B's channel 28 | `rtt.log:31,43` |
| 85.1712 | **Enhanced Beacon** seq 02, 32 bytes | `rtt.log:42` |
| 87.8395 | Spare: broadcast `49 e8`, 52 bytes, 2.678 s after the beacon (its Router Solicitation under the pairing key) | `rtt.log:45` |
| 89.76-92.84 | Three 1 s trains: Router Advertisement `59 e8` seq 03, `49 e8` seq 04, then **`d/pair`** (Data seq 05, 82 bytes, train to the spare's short address) | `rtt.log:60-62` |
| 92.8353 | Spare's Enhanced ACK to seq 05 | `rtt.log:63` |
| 94.2149 | Spare: Data, 45 bytes, under the network key: **its 2.04**, 1.38 s after its ACK; the bridge ACKs it | `rtt.log:64` (`PAIRED`) |
| 95.46-890.16 | Spare: 22 more Data frames, 91 bytes each, all ACKed | `rtt.log` `to server, acked` |
| 463.56-464.58 | One train, then a lone `49 e8` seq 06: the periodic advertisement (`RA 3`) | `rtt.log:192,215` |
| 473.95-511.18 | Two stock devices send 10 Beacon Requests: 8 seen on A, 2 during B visits. None was answered | `rtt.log:195-223,237` |

**Results:**

| Item | Result |
| :--- | :--- |
| Beacon answer (#28) | The request's end to the beacon's preamble start took **4057.1 us** (envelope to envelope 4030.0 us), inside 2.4-7.4 ms: 1.66 ms after the earliest start and 3.34 ms before the latest. The RTT says STX 3919 us after the GDO0 stamp, with the preamble estimated at 3994 us (`rtt.log:42`). The air shows 63 us more than that estimate, so 138 us pass from the STX time to the preamble: 75 us IDLE to TX plus the GDO0 fall and stamp latency **[I]** for the split. n=1, because the spare device took the first beacon |
| Beacons | 1 sent, to the target. The 10 requests from stock devices during the window went unanswered: no other Enhanced Beacon on 16, and the RTT counts `beacons sent 1` against `from others 10` |
| Advertisement pairs | 2 pairs, each `59 e8` then `49 e8`, with consecutive seq (00/01, 03/04) and each behind its own train. Afterwards one lone `49 e8`. The RTT counts `RA pairs 2`, `RA 3` |
| `d/pair` | Sent on the first attempt, 4.98 s after the Router Solicitation, against the 3 s in the design: it waited behind the advertisement pair the solicitation asked for (two 1.02 s trains). The spare's ACK came after **375.1 us**; stock devices took 350-393 us in this run (n=15) |
| Enhanced ACK (#16, 17d) | Bridge: Data end to ACK preamble start **226.0 us** median, 225.3-239.3 us, std 2.8 us, n=24. 22 of the 24 fall within 225.3-226.2 us; the other two took 231.2 and 239.3 us **[I]** (the interrupt waited for an SPI1 access). The estimate was 0.15-0.3 ms. In the same run the stock coordinator took 1507-1978 us (n=22). The RTT counts acks prepared 24, sent 24, too late 0, failed 0, and stale stays 0 in every minute log |
| Count | The RTT counts 1495 tx frames (499 + 747 + 249, `rtt.log:49,77,212`) and 24 ACKs inside the capture. Channel 16 shows 1488 wake-ups (6 trains of 248), the 6 frames after them, the beacon and 24 ACKs: **1519 of 1519 transmissions decoded on channel 16** |
| The 747 tx frames (`rtt.log:77`) | 3 × (248 wake-ups + 1 frame): the second advertisement pair and `d/pair`, each behind a 1 s CSL train, not a fault. A train has 248 wake-ups at a step of 4047.6 us (std 8.2 us), rendezvous 3125 counting down to 0, and spans 1.0035 s. The frame follows 0.41-0.43 ms after the last wake-up. From the first preamble to the frame's end a train holds the channel for 1.0230 s (1.0189 s for `d/pair`); the radio is keyed for 248 × 3.681 ms + the frame = 0.93 s of that. The stock coordinator's step on 26 is 3970 us median |
| Airtime | The driver charged 962 ms per advertisement train, 958 ms for `d/pair`, 7.0 ms for the beacon and 6.5 ms per ACK. That gives 1931 ms and 2910 ms in the first two minutes (`rtt.log:50,78`), and 5934 ms in the last hour at the end (`rtt.log:361`), 20 % of the 30 s budget |
| Offset (#7) | Bridge against the dongle: **+5.75 ppm** median at 866.3242 MHz. Over 15 min it ranged +4.86 to +5.88 ppm: it fell by 0.9 ppm in the first 9 min and recovered 0.2. The stock coordinator measured +5.25 to +6.09 ppm on 26 against the same dongle. Paired within 30 s, **bridge minus coordinator is -0.12 ppm** median (-0.44 to +0.01, 24 points); the first transmit test gave +0.6 ppm. The bridge sits 7.8 ppm below the spare device (-6730 Hz, each ACK against the Data frame it answers) and 1-8 ppm below the stock devices on 26. Its bit rate, +6.2 ppm (frames of 40 bytes or more, n=6), agrees with its carrier; the stock coordinator's does not (-31 ppm bit rate against +5.35 ppm carrier). Two stock devices heard on both 16 and 26 agree within 0.17 ppm, so the two views compare. **[I]** No per-unit trim is needed |
| Level | Bridge -0.4 dBFS median (frames -0.8 to +0.3) with PATABLE 0x26 (-10 dBm nominal): the ADC's limit, with up to 22 % of samples clipped in hopcap's rows of its first trains. So the figure is only a lower bound. The spare device clipped too (-1.2 dBFS median, up to 47 % clipped). On channel 12, the IQ image of the trains gave CRC-failed sync hits at -50 dBFS. For the next run, lower the gain (about 10 dB) or move the dongle away |
| Channel 26 (B, 141.8 s: 15.8 % of the run, 15.4 % of the window) | 742 sync hits, 734 CRC OK. The stock coordinator sent 609 of them: 571 wake-ups to 4 destinations, steps 3965-3981 us, -22 to -25 dBFS, and 38 Data and ACK frames with a -31 ppm bit rate. Five stock devices sent 121 and the spare device 4 (see below). Of the 8 CRC failures, 6 are noise sync hits at -58 dBFS and 2 are damaged coordinator wake-ups. **No frame has the bridge's address as source or destination, and no train has the bridge's step (4048 us) or level (0 dBFS).** By carrier offset alone the bridge cannot be told from the coordinator: it would sit at +4.3 to +5.1 kHz on 26, the coordinator at +4.6 to +5.1 kHz. Bit rate, step and address tell them apart |
| `burstscan.py` B | No narrowband tone. Channel 26: 227 bursts, 223 decoded frames and 4 others. The 4 are a collision tail at the coordinator's ACK (stream 21.90 s), a decoded device exchange (321.88 s), and two retune-edge fragments (567.12 and 870.89 s); the bridge sent nothing at any of those times. Others: the channel-27 neighbour (-31 kHz, 70 kHz rate), a 20 ms event at a 100 kHz rate on 29, and a 6.4 ms wideband impulse on 19-24 and 29 (77.16 s). None is tado |
| Off channel 16 (A 10-18, B 20-28) | 13 CRC-OK frames, all Beacon Requests: the spare device's on 11, its requests on 6 and 16 seen as 2.4 MHz folds (A channel 18 = channel 6, B channel 28 = channel 16), and two stock devices' on 11 and 21. None is the bridge's |
| `burstscan.py` A | No narrowband tone. Channel 16: 53 bursts, 42 decoded and 1 spill-over. The 10 others are the spare's channel-21 request folded in (78.17 s, +20 dB) and wideband 0.43 ms impulses (30.85, 66.84, 69.84 and 72.84 s), all before the window, when the bridge sent nothing. Channels 9-15: the spare's folded channel-6 and channel-21 requests (75.15, 78.17 and 83.14 s), the same impulses, and weak multi-pulse events on 9 at 477-512 s while the stock devices scanned. Channels 17-19: 891 bursts of about 20 ms, 200 kHz wide, at a 213 kHz rate, from the start of the capture: the non-tado users near 867.1 MHz from the baseline. Everything else is spill-over, mostly from the bridge's full-scale trains |
| Server (28b) | Each of the spare's 91-byte uplinks reached tanoclo within 90 ms and was dropped there with `AUTH REJECTED: Device ... is not a known device in standalone mode` (`docker logs tanoclo-ws`; `tanoclo/ws-server/lib/handlers.js:229-232`, no answer sent). The bridge relayed them (`to server`, dup 0) and received `downlinks 0`. The device retries with a doubling back-off: about 11, 22, 44, 89 s, then over again |
| Bridge health | Radio stack: 550 of 896 words never used, so the window with network creation used at most 346 words (1.4 KB), below the 1.9 KB estimate. Heap minimum 7960 bytes. Periodic calibration took 1 try, PLL locked (`rtt.log:125,240`) |

**Verdict.** Nothing of ours appeared on channel 26 or off channel 16. Two independent checks back this:
1. Every transmission the bridge's driver counted (1495 frames, 24 ACKs) was decoded on channel 16 at the time its RTT line gives. `STX` is strobed in two places only, both counted (`radio-runtime.md`, "Transmit guard"). This check covers the whole run.
2. The SDR's own check covers less. Channel 26 was watched for 15.8 % of the run (15.4 % of the window), and in that time held only stock frames and the spare device's broadcasts. Channels 9-19 were watched for 82.3 %, and every burst there off channel 16 is explained as above.

No B visit overlapped a transmission of the bridge, since A is held while channel 16 is busy. So the SDR could not have seen a copy of one of our frames on 26, only stray emissions.

**Unexpected.**
- **The spare device itself transmitted on channel 26** before the window opened: 4 broadcasts of 52 bytes (`49 e8`), at full scale, at stream 9.8-54.8 s, 12 s apart in the B visits, in between its Beacon Requests on 6, 11, 16 and 21. That is its own search for a network (the same length as its Router Solicitation on 16), not caused by the bridge. In B's coverage it sent nothing on 26 after it paired. The two stock devices that scan sent the same 52-byte broadcasts on 26.
- `d/pair` came 2 s later than designed (see above); the device accepted it.
- The bridge's ACK, at 226 us, is faster than the stock devices' (375 us) and far faster than the stock coordinator's (1.5-2.0 ms). The device accepted all 24 at first try (dup 0).
- The wake-up step is 4047.6 us; stock uses 3.94-3.99 or 4.40-4.45 ms. Every train was delivered on its first attempt.

## ACK timing (#18) (2026-11-18) [V]

Enhanced ACK turnaround of the bench bridge over the air, in normal operation and around TLS reconnects. Receive-only on the SDR; the bridge and the ST-Link were not touched (no flash, no reset, RTT read over the running OpenOCD's port 19021). Image: the local build of `c2d545c` on the bench (transmitter on, spare device named in `config/local.cmake`, plus the uncommitted local edit of `src/airtime.h`: the minute log shows a budget of 3600000 ms per hour instead of the committed 30000 ms; the last hour used 5.7-8.9 s). Captures in `~/ib01-captures/ack18/` and `~/ib01-captures/ack18b/`, outside git. Senders are named by role; no address is recorded.

**Method.** `hopcap.py` as for the pairing test (A 865.924158 MHz, channels 9-19; B 867.923670 MHz, channels 19-29), `--max-a-hard 20`, tuner gain **12.5 dB** (19.7 dB clipped the bridge in the pairing test; here the bridge read -7.6 dBFS, the spare device -8.8 dBFS, 0 clipped samples in every row). RTT logged with a host-time prefix.
- Run 1, 01:24:10-01:29:40, 330 s: observation only, plus `docker restart tanoclo-ws` at 60, 130, 200 and 270 s to make the bridge reconnect (4 TLS handshakes).
- Run 2, 01:31:11-01:38:21, 430 s: every 50 s an overlay PUT on zone 1 of the local bench server (bench user, password-grant token, 21.5 and 21.0 °C alternately), each giving one downlink to the spare device and its reply; in 6 of the 8 cycles `docker restart tanoclo-ws` 1.5-7.0 s after the PUT (6 handshakes). The overlay was left as found (MANUAL 21 °C, read back afterwards).

Decoding: `demod.py` (channels 10-18 on A, 20-28 on B), `burstscan.py` on all four files, and the new `tools/sdr/ackgap.py`, which pairs each CRC-OK unicast Data frame with the Enhanced ACK of the same sequence number, separates uplinks (our bridge ACKs) from downlinks (the device ACKs) as `demod.py --summary` does, maps each pair to host time through `run.segments.csv` and tags pairs that fall in a TLS handshake taken from the RTT `tls: ... handshake N ms` lines. **Resolution:** gaps are from Data end (last CRC bit) to ACK preamble start, both from `demod.py` sync fits (least squares over every bit-boundary zero crossing at 600 kS/s, 1.667 us per sample), good to well under 1 us ("Demodulator", accuracy); envelope edges are half-amplitude crossings interpolated between 600 kS/s samples. Host times are good to a few ms, plus 20-100 ms RTT line latency, far below the 1 s handshake windows.

| Item | Result |
| :--- | :--- |
| Bridge ACK, this run | Data end to ACK preamble start **225.3-233.9 us, median 226.0 us, std 2.8 us (n=7)**; 6 of 7 within 225.3-226.1 us. Envelope off to envelope on 200.1-208.6 us, median 200.3 us |
| Bridge ACK, with the pairing test | Same tool on the pairing capture reproduces 225.3-239.3 us, median 226.0 us (n=24). Together: **n=31, 225.3-239.3 us, median 226.0 us, mean 226.8 us, std 2.8 us; 28 of 31 at or below 226.2 us** |
| Against the target | Target: start within 1 ms of the Data frame's end (`radio.md`, "Conflicts", ACK timing). Worst case 239.3 us: **0.76 ms of margin** |
| Unanswered uplinks | 0 of the decoded uplinks. The RTT counts acks prepared 12, sent 12, too late 0, failed 0, refused for airtime 0 in run 2 (0 in run 1). 7 were decoded on A; the other 5 fell in B visits (checked against `run.segments.csv`). The device repeated none (link `dup 0`) |
| Reference | Spare device's ACK to the bridge's downlinks 360.4-392.7 us, median 384.1 us (n=8). On 26: stock devices 376.6-392.6 us (n=10), stock coordinator 1471.8-1963.3 us (n=9) |
| TLS handshakes | 10, all `TLSv1.2 TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256`: 1059-1064 ms each, 1043-1044 ms CPU in 19-21 steps, longest step 361 ms; each reconnect ready on attempt 1 |
| **ACK during a handshake** | **Not measured in these runs: no device Data frame fell in any handshake window** (handshake -0.3 s to +0.2 s). See "Why no ACK during a handshake" below. Measured later the same day with a test image that reconnects on its own: "ACK during a TLS handshake (#18)" |
| ACK during server work | Each of the 7 measured ACKs answered the device's reply 0.28 s after a downlink. By then the net task had received the server's TLS record, decrypted it and handed the datagram to the radio, and it then relayed the acknowledged uplink back over TLS (`to server, acked`). The gaps did not differ from those of the pairing test |
| Wake-up trains | Step 4046.6 us median, std 2.2 us (n=172); Data 0.4 ms after the last wake-up; short trains of 21-29 wake-ups (the device is anchored) |

**Why no ACK during a handshake [V]/[I].** The bridge has a handshake only after the server closes the connection. It then waits 1.5-2.5 s before it retries (75-125 % of 2 s, `src/session.c:250-251`), so the handshake starts at least 2.1 s after the close. The device sent uplinks only in two cases. It answered a downlink 0.2-0.3 s after it, and a downlink needs a live session: a PUT sent while the bridge was reconnecting (run 2, 01:37:20) was never delivered. Or it started an exchange of its own, which happened once in about 15 minutes (01:37:55-01:38:08, 4 uplinks). So no server action can place a device frame inside a handshake, and none fell there by chance. Measuring it needs a device uplink at a known time during a handshake. Options: a bench image that reconnects on a timer, then PB8 against GDO0 on a scope as the issue proposes, or a long observation while the device reports on its own. **[I] By design the handshake cannot delay the ACK.** The radio task runs above the net task (`PRIO_RADIO` +3, `PRIO_NET` +2, `src/app.c:17-19`), so it prepares the ACK while a P-256 step runs. The interrupt on the CRC-OK edge sends it (`radio-runtime.md`, "Enhanced ACK path"). The handshake uses neither SPI1 nor the radio.

**Reconnect back-off [V].** With restarts 50 s apart the retry delay doubled: 1.7, 4.8, 8.6, 16.2, 34.3, 47.1 s. With restarts 70 s apart it stayed at 1.8-2.1 s. This is as designed: the back-off returns to 2 s only after a session has been ready for 60 s (`STABLE_MS`, `src/session.c:42,248`).

**Nothing on channel 26 or off channel 16:**

| Item | Result |
| :--- | :--- |
| Count, run 1 | The RTT counts 500 tx frames (two periodic Router Advertisement trains of 249 wake-ups plus the `49 e8` frame each) and 0 ACKs. Channel 16 decoded **500 of 500** (498 wake-ups, 2 broadcasts) |
| Count, run 2 | The RTT counts 279 tx frames (12 datagrams of 21-29 wake-ups plus the frame) and 12 ACKs. On A: 180 wake-ups, 8 Data frames and 7 ACKs, i.e. 8 trains and 7 ACKs. The other 4 trains and 5 ACKs fall in B visits, each at the time of its RTT line |
| Channel 26 | B on air 54.8 s (16.6 %) in run 1 and 68.9 s (16.0 %) in run 2: 191 and 201 CRC-OK frames from the stock coordinator and 6 stock devices. **No frame had the bridge's or the spare device's address as source or destination.** `burstscan.py`: every burst on 26 is a decoded frame, except one, the tail of a stock coordinator train whose last wake-up failed its CRC (run 2, 01:36:38, the bridge silent since 01:36:31) |
| B while the bridge transmitted | 4 B visits in run 2 overlapped a short train, its Data frame, the device's ACK and the bridge's ACK. Channel 26 then held only 2 broadcasts of 52 bytes from a stock device that sends about 20 per minute of B in both runs. The bridge's frames showed up in B only as receiver products about 43 dB below their level on A: CRC-OK wake-ups at -50 dBFS on channel 28 (channel 16 + 2.4 MHz alias), the same frames at the same instants on channel 23, and short bursts on 19-21 and 29. The spare device's frames appeared there the same way, so the dongle makes them **[I]** |
| A, off channel 16 | `burstscan.py`: every burst on channels 9-15 and 17-19 is spill-over from a stronger channel-16 frame (run 1: 5515, run 2: 2405), none other; `demod.py` decoded no CRC-OK frame on 10-15, 17 or 18 in run 1. On channel 16 every burst is a decoded frame (2 and 15). No narrowband tone at either centre frequency in either run |
| Carrier offset (#7) | Bridge +8.48 ppm (run 1) and +8.11 ppm (run 2) against the dongle; stock coordinator +6.69 and +6.19 ppm. Bridge minus coordinator is +1.8 to +1.9 ppm (medians per run, not paired in time), against -0.12 ppm in the pairing test. The bridge now sits 6.0 ppm below the spare device (5229 Hz), against 7.8 ppm on 2026-11-17. So the bridge moved about +1.8 ppm against both since then **[I]**. Still inside the spread of the stock devices, so no trim **[I]** |

**Tool fix.** `demod.py` wrote the envelope edges (`env_on`, `env_off`) as float32: under numpy 2, float32 plus a Python int stays float32. A few hundred seconds into a file that gives steps of 10-60 us. The sync-fit times (`t_sync`, `t_pre`, `t_end`) were float64 and not affected. Fixed. Earlier envelope-to-envelope figures (for example 4030.0 us in "Pairing test (#19) result") carry that coarse step; the preamble and end times beside them do not.

**What this means for #16 and #18 [I].** The ACK starts 226 us after the Data frame in normal operation, also right after server and TLS record traffic: a quarter of the 1 ms target, faster than stock devices (about 0.38 ms) and the stock coordinator (1.5-2.0 ms). The target holds. The second condition of #18, "also during a TLS handshake", is still unmeasured on air; only the design argues for it. #18 stays open until a device frame is caught during a handshake. Measured the same evening; see the next section.

## ACK during a TLS handshake (#18) (2026-11-18, 19:26-19:56) [V]

**Image.** A temporary test build, never committed or pushed: the patch `~/ib01-backups/test18-handshake/test18-handshake.patch` (local branch `test/18-handshake`) on the bench build, with the committed airtime budget of 30 s per hour (the minute log shows `of 30000 ms`). The ELF is `~/ib01-backups/test18-handshake/budget30s/tado-bridge.elf`, SHA-256 `7befdfb2…f911ce`. It was flashed with `tools/flash.sh` (`Verified OK`, no option bytes touched) at 19:26:50, first line `TRANSMIT image (RADIO_TX_ALLOWED 1)`. While armed, the session closes itself and reconnects at once after a downlink for a device is queued, or after 4 s ready. It logs every handshake step on the 1 ms tick clock (`hs18:` lines, the same clock as `rx: t`). It disarms itself after 300 closes; it disarmed at 19:52:00. Radio, link, airtime and transmit gate code are unchanged. The image stays on the bench, disarmed, until main is flashed again.

**Method.** `rtl_sdr -f 865924158 -s 2400000 -g 12.5`, 1800 s without hopping (channels 9-19, channel 16 at +400 kHz). Host time per byte offset came from `tools/sdr/timedcap.py`, and RTT was logged by `tools/sdr/rttlog.py`. Channel 26 was not watched in this run. Seven overlay PUTs on zone 1 went out 240 s apart (21.0, 21.5, alternating, last 21.0; read back afterwards as MANUAL 21.0). Each one makes the image close the session, so the device's reply lands near the next handshake. Decoding used `demod.py --channels 16 --csv` and the new `tools/sdr/hs18place.py`. That tool pairs each uplink with the bridge's ACK (Data end to ACK preamble start, from `demod.py` sync fits). It matches each uplink to its RTT `rx: t` line by sequence number and host time. It then places the RTT tick inside a handshake step, between steps, or outside every handshake.

**Resolution.** Gaps come from sync fits at 600 kS/s (1.667 us per sample, fitted over every bit-boundary zero crossing), good to well under 1 us ("Demodulator", accuracy). Placement is good to 1 tick (1 ms): the step's start tick and its length rounded to 1 ms. Check of the tick: a straight-line fit of the RTT `rx: t` tick against the frame's end in the capture leaves the tick 0.0-1.1 ms after the frame's end (median 0.4 ms, n=33). The bridge's tick clock runs +8.8 ppm against the dongle. Placing by the tick predicted from the air time gives the same answer for 34 of 34 frames.

| Item | Result |
| :--- | :--- |
| Handshakes | 302: the first one, 300 test closes, and one after the disarm. All were `done`, none failed or abandoned. 1060-1226 ms (median 1062 ms) in 19-21 steps. The longest steps (the P-256 operations) took 319-387 ms each |
| Uplinks and ACKs | 34 CRC-OK uplinks from the spare device on channel 16, **34 ACKed** by the bridge. The RTT counts acks prepared 34, sent 34, too late 0, failed 0, refused for airtime 0 |
| **ACK inside a handshake** | **21 of 34**, each inside a handshake step of 319-387 ms. In each, the net task was inside `mbedtls_ssl_handshake_step()` (a P-256 operation), and the frame ended 23-370 ms into the step. 17 were the device's 51-byte replies to downlinks, 4 its own 109-byte reports. Data end to ACK preamble start: **225.2-233.9 us, median 226.0 us, mean 226.6 us, std 2.1 us (n=21)**; 18 of 21 within 225.2-226.1 us, the others 228.6, 231.3 and 233.9 us. Envelope off to on: median 200.1 us |
| ACK outside a handshake, same run | 225.3-226.2 us, median 226.1 us (n=13). Earlier runs outside handshakes: 225.3-239.3 us, median 226.0 us, n=31, with 3 of 31 at 231.2-239.3 us ("ACK timing (#18)") |
| Against the target | Worst case inside a handshake 233.9 us: **0.77 ms of margin** to 1 ms |
| Device ACKs to the bridge's downlinks | 352.6-392.8 us, median 377.9 us (n=28) |
| Count | The RTT counts 1864 tx frames in the capture (1832 wake-ups, 28 downlinks, 4 Router Advertisement broadcasts) and 34 ACKs. Channel 16 decoded 1831 wake-ups, 28 downlinks, 4 broadcasts and 34 ACKs: 1897 of 1898. One wake-up was not decoded; no sync hit of the bridge's level failed its CRC (all 154 CRC failures are noise hits at -58 to -59 dBFS) |
| Off channel 16 (10-15, 17, 18) | `demod.py`, first 888 s of the capture only (the pass was stopped there): the only CRC-OK frames are 10 Beacon Requests on channel 11, a frame type the bridge never sends. The CRC failures are noise hits at -59 to -51 dBFS. No `burstscan.py` pass. Channel 26 was not in view |
| Airtime | 7701 ms in the last hour at the end, of 30000 ms; no refusal |
| Bridge health | Heap minimum 7952 bytes; radio stack never used 550 of 896 words; no reset or watchdog line |

**What this means for #18 [V]/[I].** The Enhanced ACK goes out on time while the net task is inside a TLS handshake step. All 21 such cases fell in the long P-256 steps, the worst case for a preemption failure, and none missed or came late. The gaps match those outside handshakes. The three slower ACKs (228.6-233.9 us) are of the same size as those seen outside handshakes on other runs (231.2, 233.9, 239.3 us), so they do not come from the handshake **[I]** (more likely the interrupt waiting for an SPI1 access, as noted earlier). This confirms the design argument (radio task above the net task, ACK sent from the CRC-OK interrupt) on air. Both conditions of #18 now hold.

## Flash work during traffic (#22) and factory reset, plan [I]

Part 1 run on 2026-11-18, see "Flash work during traffic (#22) result" below, and rerun after the late-train fix the same evening ("Flash work during traffic (#22) rerun" below); part 2 (factory reset) checked by the owner by eye on 2026-11-18 ("Factory reset and LEDs, owner check" below), without RTT lines recorded. Code: `design.md`, "Implementation: flash work during radio traffic (#22)" and "Implementation: factory reset (P6)". Gates: a `fw-reviewer` "safe to flash" verdict on both (they change radio timing and erase records), then main flashed with `tools/flash.sh` (OpenOCD verify, no option bytes). The bench edit of `src/airtime.h` (budget 3600 s per hour) stays local and uncommitted, as before.

**1. Flash work while the spare device talks (#22), no owner hand work.** Normal operation writes the flash too rarely to test (one counter-log entry per 2048 frames sent, one sector erase per 512 entries), so the writes are forced over SWD in RAM, with no test code in the image. Symbols from the flashed ELF (`arm-none-eabi-nm -S`; the addresses change with every build): `limit` in `src/network.c` (4 bytes, the reservation's end as the network module last saw it) and `ctr` in `src/store.c` (20 bytes; `next`, the next log slot, at offset 16).
- One program: lower `limit` by 2100 while it stays above the link's counter. The next quiet moment then runs `store_counter_extend()`, which appends one 8-byte entry above every entry already in the log, so no counter is reused [I]. Needs at least one frame sent since the last reservation (wake trains, Router Advertisements), otherwise the write is refused and retried after 10 s.
- One erase: set `ctr.next` to 512 first. The next append then moves to the other sector and erases it (58-240 ms, `datasheets/mx25r1635f.txt:3340-3341`).
- Run: tanoclo sends a setpoint to the spare device every 20-30 s, so that it sends uplinks; an OpenOCD Tcl loop forces an erase every 2-5 s (each one about 2-12 % of the time in an erase) for 30-60 minutes; RTT and the SDR on channel 16 as in "ACK timing (#18)". Wear: one erase per sector per forced erase, far below 100,000 cycles.
- Pass: the minute line `flash: 60 s: radio served N times during flash work, frames N, acks N` shows ACKs during flash work; `link: 60 s: acks prepared N sent N too late 0`; no overflow; no `gave up`; every CRC-OK uplink on air has its ACK, with the gap as without flash work (225-240 us); the counter log reads back after a reset (boot line `network: record seq ..., frame counter ..., reserved below ...` above every counter seen on air). Also note the radio stack in the "alive:" line.

**2. Factory reset (owner hand work: the button, then re-pairing).**
1. Hold the factory reset button (PB10) for less than 10 s: only `button: factory reset (PB10) pressed/released`, nothing erased.
   Also watch the LEDs (owner, by eye): from 7 s the router, power and cloud LEDs flicker together (the pairing LED does not take part), 500 ms off then 500 ms on, until 10 s; released before 10 s they must return to their normal state at once (try a release at about 8 s) [I].
2. Hold it 10 s: RTT `button: factory reset held 10 s: factory reset requested`, `factory reset: erasing the network record and the counter log; the identity stays`, `factory reset: done; restarting`, then a boot with the identity record's seq 1 and `network: no network record: no key, nothing sent`. The spare device's frames are no longer acknowledged.
3. Optional: power up (USB) with the button already held and keep it held 15 s: nothing is erased until it has been released once.
4. Re-pair: hold the pair button 1 s (a new network is created on channel 16), put the spare device into pairing mode, then check downlink and uplink through tanoclo, and that a reset keeps the new network.

**Factory reset and LEDs, owner check (2026-11-18) [V, owner by eye].** On the image now on the bench (main `774ec94` plus the local `src/airtime.h` edit, binary SHA-256 `9a71cd17…c87b9a`, the `build/tado-bridge.bin` of 22:39:20, the time of commit `774ec94`), the owner reported: the factory reset warning flickers the router, power and cloud LEDs while the pairing LED stays steady; the pairing LED's patterns (slow flash while a window waits, quick flash during an exchange, 2 s on after a pairing) are fine; a factory reset and a re-pair of the spare device worked. These are the owner's observations by eye; the RTT lines of steps 1-4 were not recorded by Claude for this check, so the timing (7 s, 500 ms halves) and the optional step 3 are not measured.

## Flash work during traffic (#22) result (2026-11-18, 21:28-21:59) [V]

Part 1 of the plan above. Analysis was offline afterwards.

**Image and rules.** Main as reviewed (`fw-reviewer` verdict A; binary SHA-256 `3a7f95bc…2c834c`), already running on the bench, with the local `src/airtime.h` edit (the minute log shows a budget of 3600000 ms per hour). Nothing was flashed. The core was never halted or reset, and no flash or option-byte command was used. The reviewer's conditions: writes only with `mww` on the running core, through the OpenOCD session of `tools/rtt.sh`; first `ctr.next` (`0x20006940` + 16) := 512, then `limit` (`0x20007918`) lowered by 2048; at least one frame counter used since the last reservation for each forced erase; no reset unless `counter reservation write failed` appears. It never appeared.

**Method.**
- **Erases:** `tools/bench/flash22_force.py`. A force writes only when the link's frame counter (`g.counter`, read at `0x20006984`) is above `limit - 4096`, and when the previous lowering has been taken (`limit` moved). Each lowering therefore leads at once to one append and one sector erase, never to a refused write. The driver waits at least 2 s between forces. After each append it reads `ctr` back: the other sector is current, `next` is 1, `max` equals the new `limit`, and `max_sec` equals `cur`.
- **Traffic:** 73 overlay PUTs on zone 1 of the local bench server, 20-30 s apart, alternating 21.5 and 21.0 °C (bench user, password-grant token). Each one gives a downlink to the spare device, and the device answers about 0.26 s later. The overlay was restored to MANUAL 21.0 °C at 22:00:54 and read back.
- **RTT:** `tools/sdr/rttlog.py`.
- **SDR:** `rtl_sdr -f 865924158 -s 2400000 -g 12.5`, 1980 s (21:28:34-22:01:34) with no samples lost (byte count matches the host span), through `tools/sdr/timedcap.py`. Channel 16 was decoded with `demod.py`, and `tools/sdr/flash22place.py` paired every uplink with its ACK and placed it against the force windows. Channel 26 was not in view. A decode of channels 10-15, 17 and 18 was stopped at 24 s: it ran at 1/17 of real time.
- **Aim.** A forced erase needs a frame counter used since the last reservation, so the bridge's own frames set the rate: about 2 forces per PUT cycle, 89 in 29.4 min of forcing (one per 20 s on average, not one per 2-5 s). Forces aimed at counter changes (21:28-21:31, 11 erases) and at the RTT `delivered` lines (21:31-21:39, 22 erases) caught almost no frames. They came too late: the device's reply starts 262 ms after the downlink ends, at the device's fixed sample phase, and an erase that finds a frame arriving waits behind it (the gate). From 21:39:36 on, the force was written at 0.48-0.58 s into the host-time second of the `delivered` line, about 40-130 ms before the reply's preamble (56 erases). The driver was paused three times for 8-22 s to change this.

**Resolution.** ACK gaps run from Data end (last CRC bit) to ACK preamble start, both from `demod.py` sync fits at 600 kS/s (1.667 us per sample, fitted over every bit-boundary zero crossing), good to well under 1 us ("Demodulator", accuracy). A flash window runs from the host time just before the `limit` write to the first 10 ms poll that saw `limit` move. So it also holds the wait for a quiet moment and up to one radio-loop pass before the erase starts, and its end is late by up to one poll plus one OpenOCD read (about 15 ms). Capture time maps to host time through a line fitted to `timedcap.py`'s rows, moved to the earliest arrivals: arrival lag median 4.9 ms, 99th percentile 19.6 ms.

| Item | Result |
| :--- | :--- |
| Forced erases | **89 of 89 completed and checked in RAM.** Write to `limit` moving: 107-757 ms, median 191 ms (the 757 ms one waited for a full 1 s train). Longest flash cycle in the RTT: **150969 us** (the erase plus at most one serve pass), against 58 ms typical and 240 ms maximum in the datasheet (`datasheets/mx25r1635f.txt:3340-3341`). The windows cover 16.1 s of the 1980 s capture (0.82 %) |
| Flash work, RTT (31 minute lines) | `radio served 9718 times during flash work, frames 31, acks 31`; the gate held 43 transactions, longest 70996 us, **gave up 0** |
| Link, RTT | acks prepared 85, sent 85, **too late 0**; tx frames 3400, fail 0; refused for airtime 0; acks failed 0. Radio: **overflow 0**, stale 0, rearm 0, reinit 0, spi err 0, irq storm 0. 2 CRC failures, both noise sync hits at -111.5 dBm. Link totals: dup 0, mic fail 0, dropped 0 |
| Uplinks on air | **87 CRC-OK uplinks from the spare device, 87 ACKed.** 42 ended inside a flash window (39-102 ms from its nearer edge), 1 at its edge (6 ms), 44 outside |
| Bridge ACK gap | Inside a window: **225.1-233.8 us, median 225.9 us (n=42)**. Outside: 225.2-233.7 us, median 225.9 us (n=44). All: 225.1-233.8 us, median 225.9, mean 226.1, std 1.3 us (n=87); envelope off to on, median 199.8 us. The same as without flash work: 225.3-239.3 us, median 226.0 us ("ACK timing (#18)") |
| Window against RTT | 42 frames inside a window, against 31 counted in flash work. A window also holds the wait before the erase starts. A frame arriving then holds the flash work back (the gate), and the radio takes it in the normal loop |
| Downlinks | 88 Data frames from the bridge, 87 ACKed by the device after 352.9-395.6 us (median 383.3 us). RTT: 91 attempts, 90 delivered, 89 on the first attempt, plus 5 periodic Router Advertisements. The one undelivered attempt is the late train below |
| Late short train | One downlink at 21:30:54, attempt 1: 4 wake-ups (rendezvous 31, 19, 6, 0) and the Data frame at its usual slot, no device ACK; the RTT logged `no ack, will retry, 4 wake frames, short train`. A forced erase ran 21:30:54.129-.331 (host), and a short train normally has 21-23 wake-ups over about 88 ms, so this one started about 72 ms late. Attempt 2, 2 s later, used a full train (249 wake-ups) and was delivered. See "What this means" |
| Bridge's own broadcasts | 5 Router Advertisements (108 bytes) ended 3-17 ms after a force write, in windows of 107-119 ms. The force came during the advertisement's train, and the erase started only after the broadcast, since `quiet()` is false during a train |
| Wake-up steps | 4044.8 us median. 20 steps of 4.22-4.41 ms, each the last before a downlink's Data frame, 74-300 ms before a force or with no force near: part of the train's alignment to the sample, not flash work |
| Counter log | After every append, in RAM: sector switched, `next` 1, `max` = new `limit`, `max_sec` = `cur`; the `failing` flag never set. At the end: `max` = `limit` = the link's `counter_limit` = 65638, link counter 61544. Each entry is read back by `program_verified()`. **The read-back after a reset was not done**, since the reviewer allowed no reset in this test: at the next reset, the boot line should give frame counter 65638, reserved below 69734 |
| Radio stack | 530 words never used before the run, **498 from 21:33:09** to the end. The change came right after the first uplink received during an erase (21:33:01), so the serving path inside an erase uses at most 398 words (1.6 KB), below the 2.1 KB estimate (`design.md`, #22 "Nesting"). Net 922, housekeeping 336, heap minimum 7960 bytes, all unchanged |
| Airtime | 20744 ms in the last hour at the end (bench budget 3600000 ms); no refusal |
| Offset (#7) | Bridge +4455 Hz against the dongle at 866.3242 MHz (+5.14 ppm); the spare device +11135 Hz, so the bridge is 7.7 ppm below the spare device (6.0 ppm on the night of 2026-11-17/18, 7.8 ppm in the pairing test) |

**What this means for #22 [V]/[I].**
- **Pass:** ACKs during flash work, too late 0, overflow 0, gave up 0, every uplink on air ACKed, the gap unchanged inside erases (225.9 us median, worst 233.8 us), radio stack well above 350 words never used.
- **Open:** the counter log's read-back after a reset.
- **Finding [I]:** a sector erase can delay a short train that is already scheduled. `quiet()` allows flash work when `csl_busy()` is false, and that holds while a job waits in `ST_WAIT` with its listen 10 ms or more away (`csl_quiet_for(10000)`, `src/csl.c:702-712`). `csl_step()` does not run during flash work, so an erase of up to 240 ms pushes the train's start past the device's sample; the Data frame still goes at `wake_end_us`, and the device misses it. Nothing is lost: the retry uses a full train. The cost is about 1 s of airtime and 2 s of latency per event. Seen once in 89 forced erases. From 21:31 on, the forces followed a delivered downlink or waited for the counter to stay still for 1 s, so few could fall just before a scheduled train. The rate in normal operation is therefore not measured. In normal use a sector erase comes once per 512 × 2048 frames sent, and a counter-entry program (at most 10 ms) is too short to matter. A possible fix, for `fw-critical` and review: start an erase only when `csl_quiet_for()` covers the erase's maximum, or move the train when it starts late.
- **Finding resolved in code (`37b81c8`) and verified on air in the worst case (2026-11-18, 22:54-23:27): 71 of 71 downlinks delivered at attempt 1 with 21-22 wake-up frames, see "Flash work during traffic (#22) rerun" below.** Flash programs and erases now start only with the coordinator idle, and while such work is due the coordinator starts no new transaction (`design.md`, "Flash work and the coordinator"); host-tested, the bench case reproduced and fixed in `tests/host/test_csl.c`. Needs a `fw-reviewer` "safe to flash" verdict, then a rerun of part 1 with the forces aimed at the worst case: `limit` lowered right after tanoclo queues a downlink, while its short train waits (from the PUT's response, before the RTT `delivered` line). Pass: every downlink during a force delivered at attempt 1 with 19-23 wake-up frames (a short train) or about 249 (a full one), in the RTT and on air; no `no ack` line with fewer wake-up frames; ACK gap and counts as in the first run; `limit` may now take up to about 2 s to move, since the write waits for the transaction in flight. The counter log's read-back after a reset is still open.

## Flash work during traffic (#22) rerun, worst case (2026-11-18, 22:54-23:27) [V]

The rerun asked for under "Finding resolved in code" above, after the fix `37b81c8` (flash work only with the coordinator idle, the coordinator held while work is due). Analysis was offline afterwards, from the files in `~/ib01-captures/flash22b/` (outside git).

**Image and rules.** Main `774ec94` with the local `src/airtime.h` edit (budget 3600000 ms per hour in the minute log), binary SHA-256 `9a71cd17…c87b9a`, the same image as "Factory reset and LEDs, owner check" above. The network was the new one from the owner's re-pair (frame counter 18 and `limit` 4096 at the start). Writes as in the first run: `mww` on the running core through the OpenOCD session of `tools/rtt.sh`, `ctr.next` := 512, then `limit` lowered by 2048; the core was never halted or reset, and no flash or option-byte command was used. `counter reservation write failed` never appeared.

**Method.**
- **Traffic:** 71 overlay PUTs on zone 1 of the local bench server, 20-30 s apart, alternating 21.5 and 21.0 °C (bench user, password-grant token), each logged with the host times of its request and of the server's response (all 200). The overlay was restored to MANUAL 21.0 °C at 23:25:20 (200, delivered to the device at attempt 1 with 22 wake-up frames), and read back on 2026-11-19: MANUAL, heating on, 21 °C.
- **Aim (worst case):** `tools/bench/flash22_force.py --put-log ... --put-modes sched --csl-state ...`. After each PUT's response the driver waits for the link's frame counter to move: the coordinator takes the counter when it sets the downlink job up, so its short train is then waiting in `ST_WAIT`. It writes at that 10 ms poll. One force per PUT, 71 in all, no timer forces. The coordinator state read just before the `limit` write was **`WAIT` in 71 of 71**: every force fell on a downlink already scheduled, the case that went late in the first run.
- **RTT:** `tools/sdr/rttlog.py`, 22:53:11-23:28:21.
- **SDR:** `rtl_sdr -f 865924158 -s 2400000 -g 12.5` through `tools/sdr/timedcap.py`, 1950.1 s (22:54:32.7-23:27:02.8). No samples lost: 17855 time rows, each 512 KiB after the last, the file size equal to the last offset, and the host-time residual within -4.4/+10.8 ms of a straight line (one lost USB block would be a 109 ms step). Channel 16 was decoded with `demod.py`; `tools/sdr/flash22place.py` placed the uplinks and their ACKs against the force windows, and `tools/sdr/flash22down.py` checked each downlink against the forces. Channel 26 was not in view (the capture spans 864.72-867.12 MHz).

**Resolution.** ACK gaps run from Data end (last CRC bit) to ACK preamble start, from `demod.py` sync fits at 600 kS/s (2.4 MS/s decimated by 4, 1.667 us per sample, fitted over every bit-boundary zero crossing), good to well under 1 us ("Demodulator", accuracy). Wake-up frames are counted back from the Data frame over wake-up frames less than 10 ms apart. A force window runs from the host time just before the `limit` write to the first 10 ms poll that saw `limit` move; its end is late by up to one poll plus one OpenOCD read (about 15 ms). Capture time maps to host time through a line fitted to `timedcap.py`'s rows (slope -0.4 ppm), moved to the earliest arrivals: arrival lag median 3.2 ms, 99th percentile 14.2 ms.

| Item | Result |
| :--- | :--- |
| Forced erases | **71 of 71 completed and checked in RAM** (sector switched, `next` 1, `max` = new `limit`, `max_sec` = `cur`, `failing` never set). Write to `limit` moving: **224-1207 ms, median 748 ms** (driver log), all under the 2 s bound; no `SLOW` (> 2.5 s) line. During the wait the coordinator went `WAIT`, (`LISTEN`), `WAKE`, (`ACK`), `IDLE` before `limit` moved, in all 71: the transaction in flight ended first, then the work ran. The hold was first seen 13-103 ms after the write and was off after every append (0 of 71), never stuck (no `HOLD STUCK` line). Longest flash cycle in the RTT: 82046 us. The windows cover 51.8 s of the capture (2.65 %) |
| Downlinks in the force windows | **71 of 71 forces: the downlink's RTT line is attempt 1, delivered, short train with 21 or 22 wake-up frames; on air the same train (21-22 wake-ups) and the device's ACK.** One window (force 28) also holds a second downlink, delivered at attempt 1 with 22 wake-up frames. No full train fell in a window |
| All downlinks, on air | 90 trains: 84 unicast with short trains (21 wake-ups: 49, 22: 35), **84 ACKed by the device** after 355.3-395.4 us (median 385.2 us, the device's ACK); 6 broadcasts (Router Advertisements) with full trains of 248 wake-ups |
| All downlinks, RTT | 92 `tx:` lines (90 inside the capture), **all attempt 1, delivered**: 86 short (21: 50, 22: 36), 6 full (248). RTT and air agree on every count. **`no ack` lines: 0** in the whole log |
| Downlinks after each force | Every PUT after a force was delivered at attempt 1, and so was the restore, 59 s after the last force: the coordinator did not stay held |
| Uplinks and the bridge's ACK | **83 CRC-OK uplinks, 83 ACKed; gap 225.2-236.5 us, median 225.9 us, mean 226.1 us, std 1.2 us**; envelope off to on, median 200.0 us. All 83 ended outside a window (0 inside, 0 at an edge): the device answers about 0.26 s after a downlink, and the work now runs after the downlink's transaction and ends before that answer. So this run has no ACK inside an erase; the first run had 42 ("Flash work during traffic (#22) result") |
| Flash work, RTT | `radio served 5833 times during flash work, frames 1, acks 0` (the one frame is a Beacon Request, which gets no ACK); the gate held 1 transaction for 4980 us, **gave up 0** |
| Link, RTT (whole log) | acks prepared 89, sent 89, **too late 0**; tx fail 0, refused 0; refused for airtime 0 frames, 0 acks; acks failed 0. Radio: **overflow 0**, reinit 0, spi err 0; 1 CRC failure (minute to 23:19:11). Link totals at the end: to server 96, dup 0, mic fail 0, dropped 0, downlinks 96 refused 0, RA 8 |
| Radio stack | 526 words never used at the start, **494 from 22:56:21** to the end (above the 350 floor). Net 922, housekeeping 332, heap minimum 7960 bytes |
| Airtime | 19884 ms in the last hour at the end (bench budget 3600000 ms); no refusal |
| Offset (#7) | Bridge +5116 Hz against the dongle at 866.3242 MHz (median of 3287 wake-ups, 5-95 % +4998 to +5322 Hz), the spare device +11176 Hz (median of 83 uplinks): the bridge is 7.0 ppm below the spare device (7.7 ppm in the first run) |
| Counter log | At the end `limit` = `max` = 4203, link counter 107. **The read-back after a reset was not done** (no reset in this run): at the next reset the boot line should give frame counter 4203, reserved below 8299 **[I]** |

**What this means for #22 [V]/[I].**
- **Pass, against the rerun's criteria:** every downlink during a forced erase was delivered at attempt 1 with a short train of 21-22 wake-up frames, in the RTT and on air (71 of 71, each force on a train waiting in `ST_WAIT`); no `no ack` line; `limit` moved within 1.21 s; downlinks went on after every force; ACK gap 225.9 us median, too late 0, overflow 0, gave up 0; radio stack 494 words never used. The late short train of the first run (1 of 89, not aimed) did not occur in 71 forces aimed at it, so the fix `37b81c8` works on air.
- **Not covered by this run:** an ACK inside an erase under the new rule (all 83 uplinks fell outside the windows; the gate and serve path are unchanged since the first run, which had 42 inside **[I]**), and a full train in a force window (none fell in one).
- **Open:** the counter log's read-back after a reset.

## Bootloader (#24): first flash and checks, plan [I]

Run on 2026-11-19 by Claude over SWD, with no owner hand work, except step 3 (power cycle), which is deferred: see "Bootloader (#24): first flash and checks, result" below. Code and recovery: `design.md`, "Implementation: bootloader and boot state (#24)" and "Recovery over SWD (#24)".

**Gates:**
- A `fw-reviewer` "safe to flash" verdict on the bootloader commits (`c69ad4b..` HEAD of the #24 branch). Rule 4 applies twice over: the image is bootloader code, and the default build transmits.
- Build from the reviewed commit with the bench `config/local.cmake`.
- No option bytes, and every write verified, as always.
- **Verdict A of `fw-reviewer` (2026-11-18) [D]:** plan accepted with the changes below (steps 1, 2 and 5, and the build check). Its non-blocking notes are at the end of this section.
- **Build check before flashing:** a clean `make` from the reviewed commit. Confirm with `arm-none-eabi-readelf -l` that the bootloader ELF loads at `0x08000000` (under `0x4000` bytes) and the application at `0x08010000`. Never pass a stale pre-P7 `build/tado-bridge.elf` to `tools/flash.sh`. The main tree's uncommitted `src/airtime.h` bench edit is not covered by the review: build without it, or the owner confirms it for this image.

The application's radio code does not change in #24, but it now runs from `0x08010000`. Since the bootloader writes no boot state, the bench keeps its network, counter log and identity records.

**1. First flash.** `tools/flash.sh` writes the bootloader to sector 0 and the application to sectors 4-5. Each `program` erases only the sectors it covers and verifies (expect `Verified OK` twice). The old image's remains in sectors 1-3 are never executed. Expected RTT output: the usual boot log, with a new line `image: <size> bytes at 08010000, VTOR 08010000` before the reset cause, then session ready and the radio as before. `tools/flash.sh` with two images in one OpenOCD run is itself new **[I]**: if the second `program` fails, run `tools/flash.sh build/tado-bridge.elf` again; if OpenOCD fails, the core usually stays halted, so LED code 5 appears only after a reset or a power cycle. If the cut falls during the sector-0 write, the core locks up; recover by running `tools/flash.sh` again with both images, not the application-only form. Nothing else can go wrong.

**2. Boot without a boot state.** There is no tool to check that the boot-state sectors are blank, so this check is dropped **[D]**: the no-state path has no write path, shown by the disassembly and the host test. Only a reviewed read-only `identity.tcl` variant that drives PA4 high first (as `identity.tcl:113-125`) would be allowed. Never improvise SPI1 traffic from OpenOCD after a reset halt (PA4, the CC110L's CSn, floats, and a stray TX strobe is possible), and never take a full SPI dump (`0x102000` holds the network key). The bootloader writes nothing without a state, and the application does not write one yet. The time from reset to the first RTT line should grow by only a few milliseconds over the 2 s delay (flash wake 1-2 ms, JEDEC ID, record load, descriptor check).

**3. Power cycle (owner: unplug and replug USB).** The board boots again. Reset cause `POR/PDR` and the identity and network records as before.

**4. Debugger halt.** `halt`, wait 70 s (longer than the bootloader's longest IWDG timeout), `resume`. No reset happens: `DBG_IWDG_STOP` is set (the next boot log must not show `IWDG`).

**5. Recovery rehearsal, LED code 5.**
1. `reset halt`, then `flash info 0` and confirm that sector 4 is `0x08010000`, 64 KB. Then exactly `flash erase_address 0x08010000 0x10000` (the application's vector table and commit mark), then `reset run`. Nothing else is allowed: no `stm32f2x mass_erase`, no `options_write`, no lock, unlock or `flash protect`.
2. The power LED shows 5 flashes every 2.5 s. The owner confirms by eye, or it is read over SWD as for code 3: PB1's ODR bit sampled every 20 ms (`hardware.md`, "LED roles").
3. `tools/flash.sh` restores the board; verify the boot log.

This proves the recovery path in "Recovery over SWD (#24)", step 1, before any update code exists.

**6. ACK timing regression.** One `ackgap.py` run on channel 16 as in "ACK timing (#18)" (SDR, spare device sending). The ACK code is unchanged but sits at new addresses; the gap should stay at 225-240 us. PB8 is not needed: the SDR measures on the air.

**Later, with the application's update side (built 2026-11-19, not flashed):** "Self-update (#24): first over-the-air update, plan" below.

**Non-blocking notes from verdict A (2026-11-18) [D]/[I]:**
- Step 4's halt test only regresses `DBG_IWDG_STOP` **[I]**; it does not exercise the bootloader's own watchdog handling.
- `BSY_SPIN` of 50M iterations takes about 15-20 s **[I]**, close to the 22.3 s IWDG minimum (`bl_hw.c:363`).
- With no boot state the bootloader trusts the descriptor and head checks without a SHA-256 **[D]**; `verify` after the SWD write catches torn writes.

## Bootloader (#24): first flash and checks, result (2026-11-19) [V]

Steps 1, 2, 4, 5 and 6 of the plan above, all by Claude over SWD and the SDR, with no owner hand work. Image: `51b7418` built in a clean worktree with the bench `config/local.cmake` and the committed airtime budget (the minute log shows `of 30000 ms`, so no bench edit of `src/airtime.h`). Bootloader binary 6124 bytes, SHA-256 `eb2f44fb…bb9fa5290`; application binary 162088 bytes, SHA-256 `125b07d2…70b510eb0`.

| Step | Result |
| :--- | :--- |
| 1. First flash | `tools/flash.sh` with both ELFs in one OpenOCD run: **`Verified OK` twice**. Boot log: `image: 162088 bytes at 08010000, VTOR 08010000`, then the network record from flash (channel 16), session ready and the radio as before |
| 2. No boot state | No boot-state write in the boot log, as designed; the boot-state sectors were not read (the plan drops that check) |
| Warm resets | Two OpenOCD `reset run`: each boots cleanly through the bootloader, reset cause `SOFTWARE` (RCC_CSR), frame counter 8299, then 12395 |
| 3. Power cycle (POR) | **Not done**: needs the owner to unplug USB; deferred |
| 4. Debugger halt | Plain `halt` for 70 s, then `resume`: no IWDG reset and no new boot banner |
| 5. Recovery rehearsal | `reset halt`, `flash info 0`: sector 4 = `0x08010000`, 64 KB. `flash erase_address 0x08010000 0x10000`, then `reset run`. `0x08010000` and `0x08010200` read `ffffffff`. PB1 sampled over SWD (ODR and IDR, as for code 3; `hardware.md`, "LED roles"): five flashes of about 150 ms high and 250 ms low, then about 750 ms pause, i.e. **LED code 5**; 1 of 400 samples had ODR and IDR differ (a sample taken across an edge **[I]**). The owner's eye check was replaced by this sampling. Restored with `tools/flash.sh` and both ELFs: `Verified OK` twice, boots again (frame counter 20587) |
| 6. ACK timing | Below |

**Step 6, ACK timing regression (2026-11-19, 02:17-02:29 CET).** Receive-only. `rtl_sdr -f 865924158 -s 2400000 -g 12.5` through `tools/sdr/timedcap.py`, 720 s with no samples lost (byte count matches the host span). The view covers channels 10-18 only; channel 26 (2.4 MHz above the centre) is outside it. RTT logged by `tools/sdr/rttlog.py` from the running OpenOCD's port 19021; no OpenOCD command was sent. Traffic: 24 overlay PUTs on zone 1 of the local bench server, 22-28 s apart (bench user, password-grant token, 21.5 and 21.0 °C alternating, the last 21.0), each giving a downlink and the spare device's reply about 0.28 s later; the device also sent 5 reports of its own, and the server answered some of them (30 downlinks in all). The overlay read back MANUAL 21.0 °C afterwards. Decoding: `demod.py --channels 16 --csv`, then `ackgap.py --list`. **Resolution:** Data end (last CRC bit) to ACK preamble start, both from `demod.py` sync fits at 600 kS/s (1.667 us per sample, least squares over every bit-boundary zero crossing), good to well under 1 us ("Demodulator", accuracy); envelope edges are half-amplitude crossings interpolated between 600 kS/s samples.

| Item | Result |
| :--- | :--- |
| **Bridge ACK** | Data end to ACK preamble start **225.2-234.8 us, median 225.9 us, mean 226.8 us, std 2.6 us (n=29)**; 26 of 29 within 225.2-226.0 us, the others 233.9, 234.8 and 234.8 us. Envelope off to on 199.7-208.9 us, median 200.0 us |
| Against earlier runs | Before #24 (application at `0x08000000`): 225.3-239.3 us, median 226.0 us, n=31 ("ACK timing (#18)"); 225.1-233.8 us, median 225.9 us, n=87 ("Flash work during traffic (#22) result"). **Unchanged.** Inside the expected 225-240 us; 0.77 ms of margin to the 1 ms target |
| Uplinks | 29 CRC-OK uplinks from the spare device, **29 ACKed**, none unanswered. The RTT counts acks prepared 29, sent 29, too late 0, failed 0, refused for airtime 0 |
| Downlinks | 30 Data frames from the bridge, all ACKed by the device after 355.2-393.1 us (median 385.3 us, n=30). RTT: 30 datagrams plus 1 Router Advertisement, all delivered at attempt 1; short trains of 21-22 wake-ups, 248 for the first one and the advertisement |
| Radio and health | Overflow 0, stale 0, rearm 0, reinit 0, spi err 0; heap minimum 7960 bytes; stack words never used radio/net/hk 526/922/336; no reset during the run |
| Airtime | 4957 ms in the last hour at the end, of 30000 ms; no refusal |
| Clipping | 189038 clipped bytes in the file (0.0055 %), **all inside the spare device's frames** (its 29 uplinks and 30 ACKs, -0.4 dBFS), none in the bridge's (-7.9 dBFS). The device is louder than on 2026-11-18 (-8.8 dBFS at the same gain), so it has probably moved **[I]**. The uplink end times come from frequency zero crossings, which clipping of a constant-envelope signal barely moves, and the gaps match earlier unclipped runs **[I]**; use a gain below 12.5 dB next time |
| Offset (#7) | Bridge +6947 Hz against the dongle (+8.0 ppm), spare device +12410 Hz: the bridge is 6.3 ppm below the device (7.7 ppm on 2026-11-18) |

**What this means for #24 [V]/[I].** The first flash, the boot path without a boot state, warm resets, the debugger halt and recovery step 1 (LED code 5, then a restore over SWD) work on the bench. Running the application from `0x08010000` behind the bootloader leaves the Enhanced ACK timing unchanged (median 225.9 us, worst 234.8 us). Still open: the power cycle (POR, owner), and the update side (not built).

## Self-update (#24): first over-the-air update, plan [I]

Run on 2026-11-19 (steps 0-5; step 6 skipped): "Self-update (#24): first over-the-air update, result" below. The application's update side: `design.md`, "Implementation: self-update, application side (#24)" and "Uploader and tanoclo (#24, #38)". Planned so that Claude can run it over SWD, RTT, the SDR and the local tanoclo with **no owner hand work**. The power cut during an install (the last step) is the owner's and optional.

**Gates:**
- A `fw-reviewer` "safe to flash" verdict on the #24 application-update commits (rule 4: bootloader-facing code, a new write path, flash work in the radio task, and the default build transmits). The review must look at the write guard's areas and at the update task's stack and RAM. Verdict A with findings was given on `0b62e27`/`07d747f`; the fixes for M1 and L1 (`design.md`, "Review fixes M1 and L1") got a delta review on 2026-11-19: verdict A for `99b3f0f`, for flashing image A and for running this plan unattended **[D]**.
- Builds from the reviewed commit in a clean worktree with the bench `config/local.cmake`; never the main tree's `src/airtime.h` bench edit.
- `../tanoclo` stays unchanged (`README.md`, "Project decisions"). So the uploader runs in **`--blind`** mode, and the bridge's RTT log is the feedback: `tools/rtt.sh`, `tools/sdr/rttlog.py`, the `update:` lines.
- With the #38 change (`send-raw` with `waitAck`) the same steps run without `--blind` and see every answer. Putting that change into the bench container without changing the checkout (for example `docker cp` of a patched `lib/command-api.js`) needs the owner's OK first.
- No option bytes; every SWD write verified, as always.

**Images:**
- **A** = `make FW_VERSION=1`, the reviewed source.
- **B** = `make BUILD_DIR=build-b FW_VERSION=2`: the same source, another id.
- **C**, a candidate that cannot confirm: `make BUILD_DIR=build-c FW_VERSION=3`, then `cmake -Bbuild-c -DNET_SERVER_PORT=989 cmake/ && make -C build-c`. A whole, valid image that never reaches the server, so it is never healthy. No test code: two build options.
- Check each with `tools/update/upload.py info`. The ids must differ.

**Build check, before step 0 (from verdict A):**
- Each image is a clean build from the reviewed commit, in a clean worktree, with the bench `config/local.cmake`.
- `arm-none-eabi-readelf -l` on A's ELF: the application loads at `0x08010000`. Never pass a stale pre-P7 `build/tado-bridge.elf` to `tools/flash.sh`.
- `sha256sum build/tado-bridge-boot.bin` matches the bootloader on the bench, `eb2f44fb…bb9fa5290` ("Bootloader (#24): first flash and checks, result"), byte for byte. So step 0 rewrites the same bootloader.
- Record A's size, `ls -l build/tado-bridge.bin`. Expected about 171 KB with the bench config (171268 bytes with the M1/L1 fixes, 170392 at `07d747f`; the 161376 bytes in `design.md` is the build without `config/local.cmake`).
- `tools/update/upload.py info` gives three different ids for A, B and C.
- C was built with the other port: `grep NET_SERVER_PORT build-c/CMakeCache.txt` shows `989`.

**Stack stop rule (steps 0 and 4):** the `alive:` line's update-task figure (stack words never used) should be at least about 170, and about 250 is expected. Stop if it is below 100.

**Uploader command** (bench host, tanoclo in the container `tanoclo-ws`): `tools/update/upload.py --docker tanoclo-ws upload <image.bin> --bridge <bench serial> --blind`. In blind mode it waits 120 s after block 0 and 1.5 s per block after that, then for the bridge to reconnect.

**Steps:**
0. **Flash A** with `tools/flash.sh` (bootloader and application, `Verified OK` twice). The boot log shows `update: running <A> (<n> B); boot state none tries 0 ...` (ids as 16 hex digits) and then session ready. `boot state unread` instead means the boot-state read was not taken within 10 s (the radio task's loop did not run): stop and read the log. Apply the stack stop rule above.
1. **The running image is refused.** Upload A. RTT: `update: block 0 of 0 ...: 4.03, this image is the one running`, then a 4.08 per later block. Nothing is written: no `saved` line, and the boot state is still none at the next boot.
**Before step 2, the stock backup (L4).** Step 2's block 0 is the first time the firmware erases stock contents in `0x000000-0x0FFFFF`: the previous slot, then the candidate slot's header sector. Both slots hold stock data (`design.md`, "Stock contents of the external flash"). So check first that the P0 backup is there, without reading its contents:
   - `ls -l ~/ib01-backups/2026-11-11/a/unmodded_spi.bin` shows 2097152 bytes;
   - in `~/ib01-backups/2026-11-11/`, `sha256sum -c --ignore-missing SHA256SUMS` prints `a/unmodded_spi.bin: OK` (the P0 record, dated 2026-11-11).

   Stop if either fails.

2. **Interrupted update (session cut).** Upload B. Once RTT shows `block 64`, end the session with **`docker restart tanoclo-ws`** ("TLS and WebSocket"). This is the main way. Fallback only if the container cannot be restarted: take the link down from the laptop side with `nmcli device disconnect enp196s0f0`, wait 30 s, then `nmcli device connect enp196s0f0`. The connection runs in NetworkManager's shared mode ("Bench network"), and whether `nmcli` needs root here is untested **[I]**. Expected:
   - `update: transfer dropped at block N: the server session ended`, and the uploader stops (send-raw: not connected);
   - the session ready again after the restart (or after the link returns);
   - no restart and no install: boot state still none at the next boot.

   **Also a pass:** the bridge does not see the session end, answers 4.08 to the blocks that follow, and drops the transfer after 120 s (`update: transfer dropped: no block for 120 s`). Either way the pass needs no `PENDING` line, no restart, and boot state none (`GET b/fw` or the next boot log).
3. **Corrupted transfer.** A throwaway script in the scratchpad (not committed) imports `tools/update/upload.py`, flips one byte in block 100 of B's body and sends all blocks blind. Expected RTT at the last block: `5.00, candidate read back does not match its SHA-256`. No `PENDING`, no restart. A then still runs, and the candidate slot has no valid header.
4. **Real update A → B,** with the SDR on channel 16 (`rf-bench`: `ackgap.py` as in "ACK timing (#18)") and traffic from the spare device (overlay PUTs every 25 s, as in "Bootloader (#24): first flash and checks, result", step 6). Upload B. Expected:
   - `block 0 ...: 2.31, transfer started; running image saved to the previous slot (<ms>)`; with the earlier steps' saves it may say "already holds";
   - a line every 64 blocks; then `2.04, image verified, previous image saved, PENDING written: restarting`, then `update: restarting into the bootloader`;
   - the power LED flickering while the bootloader installs;
   - B's boot log: `update: running <B> ...; boot state testing tries 1`; about 60 s after session ready, `update: running image <B>... confirmed`;
   - a later boot shows `boot state confirmed`.

   **Measure:** block 0's time (the save) and the time per block from the RTT `(<n> ms)` figures; the whole transfer; the install time (restart to B's first RTT line). During the transfer: the ACK gap (expected 225-240 us as before, too late 0, overflow 0), every downlink delivered at attempt 1 with a short train, the airtime line, and the `alive:` line (radio and update stack words never used, heap minimum, which should be about 0.6 KB above the 7960 bytes of 2026-11-19). Apply the stack stop rule above. The SDR also confirms nothing on any channel but 16 in its view.
5. **Rollback.** Upload C to B. C installs and starts (`tries 1`), never gets a session, and restarts after 10 min (`candidate not confirmed within 10 min`). After the 3rd start the bootloader puts B back, about 32 min in all. B's boot log: `boot state rolled_back`. The uploader's reconnect wait (180 s) will time out at the start of this step, as expected; watch the RTT and `GET /api/clients` instead. Then upload B: refused (it runs). Upload A, and it installs and confirms, as step 4; that is the end state, or flash the reviewed image over SWD.
6. **Optional, owner: a power cut during an install.** During step 4's install (power LED flickering), unplug USB, wait 5 s, plug it back in. The bootloader installs again (`PENDING`, tries counted) and B confirms. Host-tested at every step (`design.md`, power cuts); not needed for the rest.

**Afterwards:** record the figures here and in `design.md` (tags **[V]**), and remove the scratch script. The bench ends on a confirmed image of the reviewed source.

## Self-update (#24): first over-the-air update, result (2026-11-19) [V]

Steps 0-4 of the plan above, by Claude over SWD, RTT, the SDR and the local tanoclo, with no owner hand work; step 5 below as an amendment; step 6 (owner) skipped. Captures and logs: `~/ib01-captures/upd24/` (outside git).

**Images and build check [V].** Built from a clean `git archive 99b3f0f` export with the bench `config/local.cmake` (the main tree's `src/airtime.h` bench edit not included; the minute log shows `of 30000 ms`). All three: application `LOAD` at `0x08010000` (`arm-none-eabi-readelf -l`), bootloader at `0x08000000`, bootloader `.bin` 6124 bytes, SHA-256 `eb2f44fb…bb9fa5290` (the bench's). `tools/update/upload.py info`: 171268 bytes, 168 image blocks plus the header block each.

| Image | Build | Id (first 8 bytes) |
| :--- | :--- | :--- |
| A | `FW_VERSION=1` | `7a3ce39741d809f6` |
| B | `FW_VERSION=2` | `366d6adcc41f3ca3` |
| C | `FW_VERSION=3`, `NET_SERVER_PORT=989` (`build-c/CMakeCache.txt`) | `ea5791f9f1012e7f` |

| Step | Result |
| :--- | :--- |
| 0. Flash A | `tools/flash.sh` with A's two ELFs: **`Verified OK` twice**. The first boot's `update:` line was lost in the RTT buffer (below); after one warm reset through the running OpenOCD: `update: running 7a3ce39741d809f6 (171268 B); boot state none tries 0 ...; no update pending`, session ready at attempt 1. `alive:` update task **261 words never used** (rule: at least about 170), heap minimum **8560 bytes** (7960 before: +600, as expected) |
| 1. Running image refused | Upload A: `block 0 of 0 ...: 4.03, this image is the one running (0 ms)`; blocks 1-4 `4.08, no transfer: start with block 0`; the uploader was then stopped. No `saved` line |
| Stock backup check | `unmodded_spi.bin` 2097152 bytes, `sha256sum -c`: `a/unmodded_spi.bin: OK` |
| 2. Session cut | Upload B. Block 0: `2.31, transfer started; running image saved to the previous slot (9217 ms)`. At `block 64 ... block stored (32 ms)`, `docker restart tanoclo-ws`: 0.7 s later `tls closed by server`, **`update: transfer dropped at block 65: the server session ended`**; session ready again 3.4 s after the restart began (attempt 1). The uploader stopped (relay pipe broken). No `PENDING`, no restart. **Pass** |
| 3. Corrupted transfer | Scratch script (removed afterwards), B with one byte of body block 100 flipped, all 169 blocks blind. Block 0: `previous slot already holds the running image (720 ms)`; last block: **`5.00, candidate read back does not match its SHA-256 (634 ms)`**. No `PENDING`, no restart. A warm reset afterwards: `boot state none tries 0`, so steps 1-3 marked nothing. **Pass** |
| 4. Update A → B | Below. **Pass** |

**Step 4 [V].** Upload B blind, with 28 overlay PUTs on zone 1 (22-28 s apart, ending on MANUAL 21.0 °C) and the SDR running.
- **Transfer:** block 0 `2.31, transfer started; previous slot already holds the running image (725 ms)`; each later block **32 ms** in the RTT figure (blocks 64 and 128; the pace of 1.5 s per block is the blind uploader's); last block **`2.04, image verified, previous image saved, PENDING written: restarting (1356 ms)`**. Block 0's answer to the last answer: 371.5 s, set by the blind pace (120 s + 167 × 1.5 s).
- **Restart and install:** `update: restarting into the bootloader` 2.0 s after the 2.04 line (`FWUPD_RESTART_MS` 3 s counts from the last block's start, `src/fwupd.c:415`, on the 1 s tick **[I]**). Restart line to B's first RTT line: **12.4 s** (bootloader install plus the application's 2 s start delay); to session ready 20.6 s.
- **B:** `update: running 366d6adcc41f3ca3 (171268 B); boot state testing tries 1 cand 366d6adcc41f3ca3 prev 7a3ce39741d809f6`, then **`... confirmed (boot state CONFIRMED) after 60 s with the server session ready and the radio up`**, 59.4 s after session ready. A warm reset later: `boot state confirmed tries 0`.
- The uploader saw the reconnect (`bridge reconnected`).

**Step 4 on air and in the RTT [V].** `rtl_sdr -f 865924158 -s 2400000 -g 8.7` through `tools/sdr/timedcap.py`, 780.1 s, no samples lost (3744464896 bytes against the host span), **no clipped bytes** at 8.7 dB. The view covers channels 10-18; channel 26 is outside it. Decoding: `demod.py --channels 16 --csv`, then `ackgap.py --list`. **Resolution:** Data end (last CRC bit) to ACK preamble start, both from `demod.py` sync fits at 600 kS/s (1.667 us per sample, least squares over every bit-boundary zero crossing), good to well under 1 us; envelope edges are half-amplitude crossings between 600 kS/s samples.

| Item | Result |
| :--- | :--- |
| **Bridge ACK, during the transfer** (block 0 sent to the last answer) | **225.9-227.7 us, median 226.0 us (n=17)** |
| Bridge ACK, whole capture | 225.3-228.7 us, median 226.0 us, mean 226.2 us, std 0.8 us (n=30); envelope off to on 199.7-203.4 us, median 200.0 us. 30 CRC-OK uplinks, **30 ACKed**, none unanswered. Inside 225-240 us; unchanged against "Bootloader (#24) ... result" (median 225.9 us) |
| Downlinks | 30 unicast Data frames, all ACKed by the device after 352.8-395.4 us (median 385.3 us); 2 broadcasts. RTT: 32 `tx:` lines, **all attempt 1, delivered**: 29 short trains (21 wake-ups: 15, 22: 14), 3 full trains (248: B's first downlink, 2 Router Advertisements). `no ack` 0. During the install the bridge was off the air (no frame of ours from the restart to B's session) |
| Link and radio, RTT | acks prepared 32, sent 32, **too late 0**; **overflow 0**, stale 0, reinit 0, spi err 0; flash work **gave up 0**, 1 hold of 4944 us; longest flash cycle 87632 us; ACKs during update operations 0 (`radio served ... acks 0`). 3 CRC failures at -97 to -99 dBm (weak, not ours **[I]**) |
| Stack and heap | Radio 514 words never used at the lowest (526 on 2026-11-19 before), net 844, housekeeping 326, **update 261** throughout (stop rule 100); heap minimum **8560 bytes** |
| Airtime | 3732 ms in the last hour before the restart, of 30000 ms; no refusal (the count is in RAM and restarts at boot) |
| Other channels in view | `demod.py --channels 10-15,17,18`: no CRC-OK frame except 10 beacon requests on channel 11 from two other devices (offsets +7.5 and +12.0 kHz, -32 and -19 dBFS; the bridge sends no beacon requests). The same two also sent 5 beacon requests each on channel 16, which the bridge did not answer. **Nothing of ours off channel 16** |
| Offset (#7) | Bridge +4603 Hz against the dongle at 866.3242 MHz (median of 1364 wake-ups), spare device +11598 Hz: the bridge is 8.1 ppm below the device (6.3-7.7 ppm in earlier runs) |

**RTT host note [V].** The 1 KB RTT buffer overflows at boot when OpenOCD finds the control block late (after `tools/flash.sh` the new `tools/rtt.sh` needed about 3 s): the first boot after the flash lost the lines from the radio bring-up to the `update:` line. With `rtt polling_interval 10` (default 100 ms) set over OpenOCD's telnet and the reader attached before a reset, every later boot log came through whole. A reset leaves NUL bytes from stale RAM in one log line (use `grep -a`).

**Step 5 (rollback), in progress at the time of writing [V]:** upload C to B: block 0 `running image saved to the previous slot (9890 ms)`, last block `2.04 ... PENDING written (1358 ms)`, restart. C: `boot state testing tries 1` (server `wss://10.42.0.1:989`, `tls tcp (-14)` on every try), `candidate not confirmed within 10 min: restarting` exactly 600.0 s after its start line, then `tries 2`, the same, then `tries 3` (start at 07:20:20 UTC). The uploader's reconnect wait timed out after 180 s, as planned. Result of the third restart: see the amendment below.

**Amendment, step 5 result [V].** C's third start also restarted itself after 600.0 s (07:30:20 UTC). The bootloader then put B back: restart line to B's first RTT line **10.4 s**, and B's boot log reads **`update: running 366d6adcc41f3ca3 (171268 B); boot state rolled_back tries 0 cand ea5791f9f1012e7f prev 366d6adcc41f3ca3; no update pending`**, session ready at attempt 1 eight seconds later, update task 261 words never used, heap minimum 8560 bytes. From the restart into C to B's line: **30.5 min** (three starts of 600 s each, plus three restarts, the install of C and the rollback install). **Pass.** `ROLLED_BACK` is not a test state, so B does not confirm again; it stays `ROLLED_BACK` and the bootloader just starts it (`design.md`, "Decisions per boot").

**Not done on 2026-11-19:** the last part of step 5 (upload B, refused; upload A, installs and confirms). The owner left the bench at about 07:31 UTC and unplugged it. Done the next night: "End of step 5" below.

**State the bench was left in (2026-11-19, 07:31 UTC) [V]:** image B (`366d6adcc41f3ca3`, the reviewed source at `99b3f0f` with `FW_VERSION=2`) running from internal flash; boot state `ROLLED_BACK`, tries 0, cand C, prev B; previous slot holds B, candidate slot holds C. Bootloader `eb2f44fb…` unchanged. Overlay on zone 1: MANUAL 21 °C (read back). SDR capture finished; RTT logger and OpenOCD stopped after the last line (07:30:50 UTC). Then all cables unplugged by the owner.

**End of step 5 (2026-11-19, 23:14-23:30 UTC; 2026-11-20 00:14-00:30 CET) [V].** The last part of step 5, by Claude over SWD, RTT, the SDR and the local tanoclo. The owner only plugged the bench back in (USB, ST-Link without 3V3, Ethernet, SDR). No overlay PUTs, no option bytes, no SWD write. Logs and capture: `~/ib01-captures/upd24-end/` (outside git).

**Images [V].** A and B rebuilt from a clean `git archive 99b3f0f` export (the four submodules copied from the main tree, at the commits `99b3f0f` pins and with no local changes) with the bench `config/local.cmake` (unchanged since 2026-11-17). The main tree's `src/airtime.h` bench edit was not in the export (`AIRTIME_BUDGET_US 30000000u`; the minute log shows `of 30000 ms`). Between `99b3f0f` and the HEAD when the run started (`78bf275`) only `docs/` and `README.md` changed. Both builds match the reviewed images byte for byte: A SHA-256 `7a3ce397…8510d3a2bb`, B `366d6adc…b6fd1a66a` (the ids in the table above), 171268 bytes each, `LOAD` at `0x08010000`; bootloader `.bin` 6124 bytes, `eb2f44fb…bb9fa5290`.

| Step | Result |
| :--- | :--- |
| Power-on after re-cabling | The first boot after the owner plugged USB back in: `reset: RCC_CSR 0e000002: POR/PDR PIN BOR`, `image: 171268 bytes at 08010000, VTOR 08010000`, identity record, network record (channel 16) and the radio's PARTNUM, then `alive:` lines every 10 s. The rest of that boot log, from the radio bring-up to the session, was lost in the RTT buffer: the reader attached after the boot ("RTT host note") |
| Boot and session | After `rtt polling_interval 10` over OpenOCD's Tcl port and one warm reset (`reset run`, cause `SOFTWARE PIN`): **`update: running 366d6adcc41f3ca3 (171268 B); boot state rolled_back tries 0 cand ea5791f9f1012e7f prev 366d6adcc41f3ca3; no update pending`**. Session ready at attempt 1, 8.0 s after that line. Update task 261 words never used, heap minimum 8560 bytes |
| Upload B (running) | **Refused:** `update: block 0 of 0 (1024 bytes): 4.03, this image is the one running (1 ms)`. Blocks 1-4 `4.08, no transfer: start with block 0 (0 ms)`; then the uploader was stopped. No `saved` line, no `PENDING`, no restart. **Pass** |
| Upload A | Block 0: `2.31, transfer started; previous slot already holds the running image (722 ms)`. Blocks 64 and 128: `block stored (32 ms)`. Last block: **`2.04, image verified, previous image saved, PENDING written: restarting (1354 ms)`**, 371.5 s after block 0's answer (the blind pace). `update: restarting into the bootloader` 2.0 s later. Restart line to A's first RTT line: **12.4 s**; to session ready (attempt 1) 20.8 s |
| A confirms | **`update: running 7a3ce39741d809f6 (171268 B); boot state testing tries 1 cand 7a3ce39741d809f6 prev 366d6adcc41f3ca3; running the candidate under test: confirmation due`**, then **`update: running image 7a3ce39741d809f6... confirmed (boot state CONFIRMED) after 60 s with the server session ready and the radio up`**, 60.1 s after session ready (RTT host times: stamped to 1 ms, good to about 10 ms with the 10 ms polling; the same for the other RTT times here). The uploader logged `bridge reconnected`. A warm reset 27 s later: `boot state confirmed tries 0 cand 7a3ce39741d809f6 prev 366d6adcc41f3ca3; no update pending`. **Pass** |

**Stop rules and health, whole RTT log (23:14:38-23:53:01 UTC) [V].** Every 60 s line: too late 0, refused for airtime 0 frames and 0 acks, tx refused 0; radio overflow 0, stale 0, rearm 0, reinit 0, spi err 0; flash work gave up 0, longest flash cycle 84376 us (during the A transfer). Stack words never used, lowest values: radio 532, net 844, housekeeping 332, **update 261** throughout (stop rule 100). Heap minimum 8560 bytes in every `alive:` line. Airtime at most 5570 ms in the last hour, of 30000 ms. 14 `tx:` lines, **all attempt 1, delivered**: 7 short trains (21-34 wake-ups), 7 full trains of 248 (the first downlink after a boot, twice, and 5 Router Advertisements); `no ack` 0. No stop rule was hit.

**On air [V].** Receive-only: `rtl_sdr -f 865924158 -s 2400000 -g 8.7` through `tools/sdr/timedcap.py`, 720.0 s from 23:17:56 UTC (40 s before the A upload) to 23:29:56 UTC, no samples lost (3456368640 bytes against the host span). The view covers channels 10-18; channel 26 is outside it. Decoding: `demod.py --channels 16 --csv`, then `ackgap.py --list`. **Resolution:** Data end (last CRC bit) to ACK preamble start, both from `demod.py` sync fits at 600 kS/s (1.667 us per sample, least squares over every bit-boundary zero crossing), good to well under 1 us; envelope edges are half-amplitude crossings between 600 kS/s samples.

| Item | Result |
| :--- | :--- |
| Traffic | Without overlay PUTs the spare device sent no uplink during the transfer or the install. Its first uplinks came after A confirmed (3 in the capture, 1 after it). The capture holds 1 full train with a broadcast (a Router Advertisement, during the transfer), 1 full and 2 short trains (21 wake-ups) of downlinks, 538 wake-ups in all |
| **Bridge ACK** (image A) | Data end to ACK preamble start **225.2-226.0 us, median 225.9 us (n=3)**; envelope off to on 199.9-200.3 us. 3 CRC-OK uplinks, 3 ACKed. Same as before (median 226.0 us during step 4, n=30). With n=3 this only shows that nothing has moved; step 4 is the measurement |
| Downlinks | 3 unicast Data frames, all ACKed by the device after 372.7-375.6 us |
| During the install | No frame of ours from the restart (stream 386.8 s) to A's first downlink. Other devices sent 10 beacon requests on channel 16 between 390.6 and 431.7 s (offsets +8.1 and +12.5 kHz, -27 dBFS median; -61 dBm at the bridge in the RTT, against -24 dBm for the spare device). The bridge sent no beacon |
| Other channels in view | `demod.py --channels 10-15,17,18`: no CRC-OK frame except 10 beacon requests on channel 11 from the same two senders (+8.2 and +12.5 kHz, -33 and -21 dBFS), in the same 41 s as those on channel 16. **Nothing of ours off channel 16.** The same pattern came on 2026-11-19 (step 4, "Other channels in view"), then 106 s before the restart, so it is not tied to the install **[I]** |
| Clipping | No clipped bytes at 8.7 dB |
| Offset (#7) | Bridge +7570 Hz against the dongle at 866.3242 MHz (median of 538 wake-ups, 5-95 % +7546 to +7942 Hz), spare device +12497 Hz: the bridge is 5.7 ppm below the device (6.3-8.1 ppm in earlier runs) |

**What this means for #24 [V]/[I].** Step 5 is complete. After a rollback, re-sending the running image is refused before anything is written, and an update from the `ROLLED_BACK` state installs and confirms like the update A → B in step 4 (A in 12.4 s, confirmed 60.1 s after session ready). With steps 0-4 and the rollback, every bench step of the plan has now passed, except step 6 (the owner's power cut during an install). The power-on boot after re-cabling went through the bootloader to the application (reset cause `POR/PDR`), but its log was cut before the `update:` line. So it does not yet count as the bootloader's power-cycle check ("Bootloader (#24): first flash and checks, result", step 3) **[I]**.

**State the bench was left in (2026-11-19, 23:53 UTC) [V]:** image A (`7a3ce39741d809f6`, the reviewed source at `99b3f0f` with `FW_VERSION=1`) running from internal flash; boot state `CONFIRMED`, tries 0, cand A, prev B. The previous slot holds B, the candidate slot holds A. Bootloader `eb2f44fb…` unchanged. Overlay on zone 1 not touched (MANUAL 21 °C since 2026-11-19). SDR capture finished; RTT logger and OpenOCD stopped after the last line. Cables still connected.

## Identity v2 image (#39): first flash (2026-11-19 23:59 to 2026-11-20 00:10 UTC) [V]

The merged #39 code (`6da2352`) installed over the air from A, by Claude over SWD (read-only, plus two warm resets), RTT, the SDR and the local tanoclo. No owner hand work. **Not provisioned:** the identity record stays version 1 seq 1, and `tools/identity/provision.sh` was not run, because the owner must first confirm that the MAC is this unit's own ("Status" of #39 in `prototype-plan.md`, P7). Logs and capture: `~/ib01-captures/id39/` (outside git).

**Image [V].** Built from a clean `git archive 6da2352` export, with the four submodules copied from the main tree (the commits `6da2352` pins, no local changes) and the bench `config/local.cmake`. The main tree's `src/airtime.h` bench edit was not in the export (`AIRTIME_BUDGET_US 30000000u`; the minute log shows `of 30000 ms`). Built with `make FW_VERSION=3` (`RADIO_TX_ALLOWED` 1, `NET_SERVER_PORT` 988). The `fw-reviewer` verdict "safe to flash" covers the branch at `40dbdaf`. `git diff 40dbdaf 6da2352 -- src tools` shows one change only: in `src/enc28j60.c`, the register check's log line no longer prints the written byte, because that byte is part of the MAC. Application `.bin` **171984 bytes** (the reviewed branch build: 171980 bytes, SHA-256 `aaab530f…`, `FW_VERSION` 0), SHA-256 `c18a7cda…43eec060`, so the id is `c18a7cda337a1a42`. `LOAD` at `0x08010000`. Bootloader `.bin` `eb2f44fb…bb9fa5290`, unchanged.

| Step | Result |
| :--- | :--- |
| Before | A running, `CONFIRMED`; minute log: too late 0, airtime 5570 ms of 30000 ms, update task 261 words never used |
| Upload (blind) | Block 0: **`2.31, transfer started; running image saved to the previous slot (9858 ms)`**. The previous slot held B, so A was saved this time. Blocks 64 and 128: `block stored (32 ms)` and `(33 ms)`. Last block: **`2.04, image verified, previous image saved, PENDING written: restarting (1381 ms)`**, 362.4 s after block 0's answer. The uploader took 372.3 s to send (blind pace). `update: restarting into the bootloader` came 2.0 s later (00:05:54.5 UTC) |
| Install | The uploader logged `bridge reconnected` (tanoclo, 00:06:15.4 Z), 20.9 s after the restart line. That matches 20.6-20.8 s from the restart to session ready on 2026-11-19. The candidate's boot log was lost (RTT host note below) |
| Confirm | **`update: running image c18a7cda337a1a42... confirmed (boot state CONFIRMED) after 60 s with the server session ready and the radio up`**. Read from the RTT up buffer in RAM with OpenOCD `read_memory` (reads only). In the same buffer: `ip: 60 s: address 10.42.0.43`, the placeholder MAC's lease |
| Boot log (warm reset, `reset run`, cause `SOFTWARE PIN`) | `image: 171984 bytes at 08010000`; **`identity: record seq 1, version 1: serial ..., EUI-64 set, no Ethernet MAC (placeholder MAC, src/net.c)`**; network record seq 1, channel 16; radio RX on channel 16, transmitter enabled on this channel only; **`eth: MAC 02:7a:d0:b1:00:01, the placeholder: the identity record (version 1) holds none`**, and `eth: configured: MAC 02:7a:d0:b1:00:01, half duplex ...`; **`update: running c18a7cda337a1a42 (171984 B); boot state confirmed tries 0 cand c18a7cda337a1a42 prev 7a3ce39741d809f6; no update pending`**. Session ready at attempt 1, 8.3 s after the first RTT line (10.4 s after the reset command) |
| Traffic, RTT (after the second warm reset, to 00:16:32 UTC) | 6 `tx:` lines, **all attempt 1, delivered**: 4 short trains (21 wake-ups) and 2 full trains of 248 (the first downlink after the boot, and a broadcast). `acks prepared 5 sent 5 too late 0`. Airtime at most 2316 ms in the last hour, of 30000 ms |
| Stop rules | Every 60 s line after the install: too late 0; refused for airtime 0 frames and 0 acks; tx refused 0; overflow 0, reinit 0, spi err 0; flash work gave up 0. Stack words never used (radio/net/hk/update), lowest: **532/844/332/261**, the same as under A. Heap minimum 8560 bytes. No stop rule was hit |

RTT host times are stamped to 1 ms and good to about 10 ms with 10 ms polling. The `(<n> ms)` figures are the firmware's own.

**On air, sanity check only [V].** Receive-only: `rtl_sdr -f 865924158 -s 2400000 -g 8.7` through `tools/sdr/timedcap.py` for 150.1 s, from 0.3 s before the restart line to just after the first warm reset. No samples were lost (720371712 bytes against the host span) and no bytes were clipped. The view covers channels 10-18; channel 26 is outside it. `demod.py --channels 10-18` found **no CRC-OK frame on any channel** in view (7-15 sync hits per channel, all CRC failures: noise or weak frames). The new image sent no downlinks in that time (`downlinks to the radio 0`), and no uplink came for it to ACK. So the capture shows only that nothing of ours went out off channel 16. It is no ACK or offset measurement.

**RTT host note: the control block moved [V].** The `cb` of `src/rtt.c` is at `0x200082cc` in A and at `0x200082d4` in this image (`arm-none-eabi-nm`). OpenOCD searches for the control block only once (`tools/rtt.sh`), so after the restart it kept reading the old address and got garbage: format strings and stale RAM in `rtt.log` from 00:06:09 UTC. Its read-offset write-back to the old block landed in the new block's `up[0].size`, which read back as `0x0007fc00`. The firmware does not use that field: it writes with its own `UP_SIZE` (`src/rtt.c:9`, `src/rtt.c:47-55`). A first `rtt stop`/`rtt start` found the new block but cached that bad size. The fix: `rtt stop`, `rtt setup 0x200082d4 0x20 "SEGGER RTT"`, a warm reset, `rtt start` retried until `rtt channels` shows `Terminal 1024`, then a second warm reset with the reader attached beforehand. The first warm reset lost the lines from the Ethernet SPI line to the network line, because the reader attached about 1 s after the application's start. The second one gave the whole log. **For a later update to an image whose `cb` address differs:** run `rtt stop` before the restart, then set up the new address. Neither the install nor the confirmation was affected, since A's bootloader and the candidate both ran through to `CONFIRMED`. OpenOCD's stray write went to RAM that the new image had already set up as its RTT control block **[I]**.

**What this means for #39 [V]/[I].** The identity v2 code runs on the bench unit with a version 1 record exactly as designed (`design.md`, "Implementation: Ethernet MAC, identity record v2 (#39)"): it reports "no Ethernet MAC", uses the placeholder `02:7a:d0:b1:00:01` for the netif and the ENC28J60, and keeps the DHCP lease `10.42.0.43`. The self-update path took an image of a different size (171984 bytes) and confirmed it. Still open: the owner confirms that the MAC in `~/ib01-backups/original-bridge-mac.txt` is this unit's own; then provisioning the version 2 record (slot B, seq 2) and checking the MAC on the bench network.

**State the bench was left in (2026-11-20, 00:16 UTC) [V]:** image `c18a7cda337a1a42` (`6da2352`, `FW_VERSION=3`) running from internal flash. Boot state `CONFIRMED`, tries 0, cand `c18a7cda337a1a42`, prev A (`7a3ce39741d809f6`). The previous slot holds A, and the candidate slot holds the new image. Bootloader `eb2f44fb…` unchanged. Identity record version 1 seq 1 in slot A, slot B blank (not provisioned), placeholder MAC. Overlay on zone 1 not touched. RTT logger and OpenOCD stopped at 00:16:32 UTC; SDR capture finished. Cables still connected.

## P8 rows 1-10 image (#33): first flash (2026-11-20, 01:04-01:50 UTC) [V]

The comment-only P8 rows 1-10 (`cleanup-p8.md`, "Rows 1-10: result") and the comment-only follow-up `424420e` were installed over the air from `c18a7cda337a1a42`. Claude ran it over RTT (OpenOCD only read the target: no SWD write, no reset), the SDR and the local tanoclo, with no owner hand work. **Not provisioned:** the identity record stays at version 1 seq 1. Logs and capture are in `~/ib01-captures/p8r10/` (outside git).

**Image [V].** Built from a clean `git archive 424420e` export, with the four submodules copied from the main tree (at the commits `424420e` pins, no local changes) and the bench `config/local.cmake`. The main tree's `src/airtime.h` bench edit was not in the export (`AIRTIME_BUDGET_US 30000000u`; the minute log shows `of 30000 ms`). Built with `make FW_VERSION=4` (`RADIO_TX_ALLOWED` 1, `NET_SERVER_PORT` 988).
- The `fw-reviewer` verdict "safe to flash" (2026-11-20) covers `07a9e28`, whose application `.bin` is `508b33a1…6c61d9`. `git diff 07a9e28 424420e` changes only comments, in `src/network.h`, `src/pa3_adc.c` and `src/rng.c`.
- The same export built with `FW_VERSION=0` gives `508b33a1…6c61d9` byte for byte.
- The `FW_VERSION=4` build differs from that build in `objdump -d` in two places only. One is the descriptor's `fw_version` word at `0x0801020c`. The other is the update task's `task()`, where `fwupd_init()` is inlined (`src/fwupd_task.c:196`): the constant 4 takes one extra `movs`, where 0 reused a register that already held 0. After it, PC-relative literal offsets shift and one alignment `nop` drops.
- Application `.bin` **171984 bytes**, SHA-256 `68a24dfc…8c460fb93c`, so the id is **`68a24dfcd65549c4`**. `LOAD` at `0x08010000`. Bootloader `.bin` `eb2f44fb…bb9fa5290`, unchanged.
- The RTT `cb` is at `0x200082d4`, the same as in the running image (`arm-none-eabi-nm`). So the RTT reader (`tools/rtt.sh`, `rtt polling_interval 10`) stayed attached through the restart, as in the update A → B of 2026-11-19. No `rtt stop`/`rtt setup` was needed, and the candidate's whole boot log came through. OpenOCD logged 200 lines of `rtt: Up-channel 0 is not active` with no timestamps, most likely while the restart cleared the control block **[I]**. OpenOCD was shut down over its Tcl port at the end, without a reset.

| Step | Result |
| :--- | :--- |
| Before | `c18a7cda337a1a42` running, `CONFIRMED`. Minute log: too late 0, airtime 9895 ms in the last hour, of 30000 ms; stack words never used 532/844/332/261 |
| Upload (blind) | Block 0: **`2.31, transfer started; running image saved to the previous slot (9834 ms)`** (the previous slot held A). Blocks 64 and 128: `block stored (32 ms)`. Last block: **`2.04, image verified, previous image saved, PENDING written: restarting (1381 ms)`**, 362.4 s after block 0's answer. The uploader took 372.4 s to send (blind pace). `update: restarting into the bootloader` came 2.0 s later (01:10:51.9 UTC) |
| Install | From the restart line to the candidate's first RTT line: **12.5 s**. To session ready (attempt 1): **21.0 s**. The uploader logged `bridge reconnected` |
| Candidate boot log | `reset: RCC_CSR 14000002: SOFTWARE PIN`; `image: 171984 bytes at 08010000, VTOR 08010000`; **`identity: record seq 1, version 1: ... EUI-64 set, no Ethernet MAC (placeholder MAC, src/net.c)`**; network record seq 1, channel 16; radio RX on channel 16 (866324156 Hz), transmitter enabled on this channel only; **`eth: MAC 02:7a:d0:b1:00:01, the placeholder: the identity record (version 1) holds none`**; DHCP lease `10.42.0.43`; **`update: running 68a24dfcd65549c4 (171984 B); boot state testing tries 1 cand 68a24dfcd65549c4 prev c18a7cda337a1a42; running the candidate under test: confirmation due`**. These are the same lines as the #39 image's boot log above, apart from the ids, the boot state and the reset cause bits |
| Confirm | **`update: running image 68a24dfcd65549c4... confirmed (boot state CONFIRMED) after 60 s with the server session ready and the radio up`**, 60.1 s after session ready (01:12:13.0 UTC) |
| Stop rules (whole RTT log, from the candidate's start at 01:11:04 to 01:50:14 UTC, 39 minute blocks) | too late 0; refused for airtime 0 frames and 0 acks; tx fail 0, refused 0; radio overflow, stale, rearm, reinit and spi err all 0; flash work gave up 0. Acks prepared equals acks sent in every minute (14 in all). Stack words never used (radio/net/hk/update), lowest: **532/844/332/261**, the same as under the #39 image (stop rule 100). Heap minimum 8560 bytes. Airtime at most 8044 ms in the last hour, of 30000 ms. No stop rule was hit |
| Traffic, RTT | 20 `tx:` lines, 19 of them delivered: 13 unicast datagrams and 6 broadcasts. Full trains of 248 went out for the first downlink after the boot, for one retry and for the broadcasts; the other trains were short, of 21-29 wake-ups. All went at attempt 1 except one. **Datagram 5**, the first PUT's downlink 62 s after the previous one, logged `attempt 1: no ack, will retry, 22 wake frames, short train` and was delivered at attempt 2 with a full train |

RTT host times are stamped to 1 ms and good to about 10 ms with 10 ms polling. The `(<n> ms)` figures are the firmware's own.

**On air [V].** Receive-only: `rtl_sdr -f 865924158 -s 2400000 -g 8.7` through `tools/sdr/timedcap.py`, 850.1 s from 01:12:22.8 UTC (9.8 s after the confirmation) to 01:26:32.9 UTC. No samples were lost (4080533504 bytes against the host span, within 2 ms) and no bytes were clipped. The view covers channels 10-18; channel 26 is outside it. Traffic: 4 overlay PUTs on zone 1 of the local bench server, 254-259 s apart (at most one per 4 min), alternating 21.5 and 21.0 °C and ending on 21.0 (all 200; read back afterwards as MANUAL 21 °C). Decoding: `demod.py --channels 16 --csv`, then `ackgap.py --list`. **Resolution:** the gap runs from the Data frame's end (last CRC bit) to the ACK's preamble start. Both come from `demod.py` sync fits at 600 kS/s (1.667 us per sample, least squares over every bit-boundary zero crossing), good to well under 1 us. Envelope edges are half-amplitude crossings between 600 kS/s samples.

| Item | Result |
| :--- | :--- |
| **Bridge ACK** (image `68a24dfcd65549c4`) | Data end to ACK preamble start **225.5-236.6 us, median 225.9 us, mean 227.9 us (n=5)**. Envelope off to on 199.7-211.1 us, median 199.9 us. 4 of the 5 fall within 225.5-225.9 us; the 236.6 us one answers the device's 109-byte report. 5 CRC-OK uplinks, **5 ACKed**, none unanswered. Inside 225-240 us. Unchanged against step 4 of "Self-update (#24) ... result" (median 226.0 us, n=30) |
| Downlinks | 5 unicast Data frames, all ACKed by the device after 363.1-370.1 us (median 365.4 us), and 2 broadcasts (full trains). For datagram 5's attempt 1 (stream 0.84 s), the 22-wake-up train and the Data frame are CRC-OK on air at -14.5 dBFS, with rendezvous 0 on the last wake-up, and **no ACK from the device follows**. So the device did not answer that attempt; the bridge's retry path worked as designed **[I]**. This is the first attempt-1 miss in the bench runs since #24 |
| Beacon requests | 10 on channel 16 between 124 and 165 s, from two other senders (+8.4 to +8.6 kHz and +12.8 to +12.9 kHz). The RTT lists them as `beacon request`. The bridge sent no beacon |
| Other channels in view | `demod.py --channels 10-15,17,18` gave 490 sync hits and only 10 CRC-OK frames. All 10 are beacon requests on channel 11, from the same two senders (+8.4 to +8.6 kHz at -34 dBFS, +12.8 to +12.9 kHz at -21 dBFS), in the same 41 s as those on channel 16. This is the pattern of the two earlier runs. **Nothing of ours off channel 16** |
| Offset (#7) | Bridge +7459 Hz against the dongle at 866.3242 MHz (median of 862 wake-ups, 5-95 % +7189 to +8252 Hz), spare device +12756 Hz (median of its 15 frames): the bridge is **6.1 ppm** below the device (5.7-8.1 ppm in earlier runs) |

**What this means for #33 [V]/[I].** The P8 rows 1-10 image runs on the bench unit like the #39 image it replaced. It has the same boot log, the same stack and heap figures, and the same Enhanced ACK gap (median 225.9 us against 226.0 us). Its machine code is the reviewed `07a9e28` build's, apart from `FW_VERSION`. Once more, the self-update path installed and confirmed a new image starting from `CONFIRMED`. Rule 5 of `cleanup-p8.md` is met for rows 1-10.

**State the bench was left in (2026-11-20, 01:50 UTC) [V]:** image `68a24dfcd65549c4` (`424420e`, `FW_VERSION=4`) running from internal flash. Boot state `CONFIRMED` (the confirmation line; no reset since), cand `68a24dfcd65549c4`, prev `c18a7cda337a1a42`. The previous slot holds `c18a7cda337a1a42`, and the candidate slot holds the new image. Bootloader `eb2f44fb…` unchanged. Identity record version 1 seq 1 in slot A, slot B blank (not provisioned), placeholder MAC. Overlay on zone 1 MANUAL 21 °C. RTT logger and OpenOCD stopped; SDR capture finished. Cables still connected.

## P8 rows 11-20 image (#33): first flash (2026-11-20, 03:22-03:50 UTC) [V]

P8 rows 11-20 (`cleanup-p8.md`, "Rows 11-20: result"; main at `dabd7d1`) installed over the air from `68a24dfcd65549c4`. Claude ran it over RTT (OpenOCD only read the target: no SWD write, no reset), the SDR and the local tanoclo, with no owner hand work. **Not provisioned:** the identity record stays at version 1 seq 1. Logs and capture are in `~/ib01-captures/p8r20/` (outside git).

**Image [V].** Built from a clean `git archive dabd7d1` export, with the four submodules exported at the commits `dabd7d1` pins (the main tree's copies are at those commits, no local changes) and the bench `config/local.cmake`. The main tree's `src/airtime.h` bench edit was not in the export (`AIRTIME_BUDGET_US 30000000u`; the minute log shows `of 30000 ms`). Built with `make FW_VERSION=5` (`RADIO_TX_ALLOWED` 1, `NET_SERVER_PORT` 988, from `CMakeCache.txt`).
- The `fw-reviewer` verdict "safe to flash" (2026-11-20/21) covers `dabd7d1`, whose application `.bin` with `FW_VERSION=0` is `e7f292d1…b894`. The same export built with `FW_VERSION=0` gives `e7f292d1…b894` byte for byte.
- The `FW_VERSION=5` build differs from that build in `objdump -d` in two functions only, as for rows 1-10: the descriptor's `fw_version` word at `0x0801020c` (`image_desc`), and the update task's `task()`, where the constant 5 for the inlined `fwupd_init()` takes one extra `movs` and the PC-relative literal offsets after it shift. Every other function is identical, addresses included.
- Application `.bin` **171952 bytes** (32 bytes less than the rows 1-10 image: row 19), SHA-256 `ba8af1a3…ee04c61fd`, so the id is **`ba8af1a3e7720e44`**. `LOAD` at `0x08010000`. Bootloader `.bin` 6124 bytes, `eb2f44fb…bb9fa5290`, unchanged.
- The RTT `cb` is at `0x200082d4`, the same as in the running image (`arm-none-eabi-nm`), so the reader (`tools/rtt.sh`, `rtt polling_interval 10` over the Tcl port) stayed attached through the restart and the candidate's whole boot log came through. OpenOCD was shut down over its Tcl port at the end, without a reset.

| Step | Result |
| :--- | :--- |
| Before | `68a24dfcd65549c4` running. Minute log: too late 0, airtime 9006 ms in the last hour, of 30000 ms; stack words never used 532/844/332/261, heap minimum 8560 bytes |
| Upload (blind) | Block 0: **`2.31, transfer started; running image saved to the previous slot (9803 ms)`** (the previous slot held `c18a7cda337a1a42`). Blocks 64 and 128: `block stored (32 ms)`. Last block: **`2.04, image verified, previous image saved, PENDING written: restarting (1382 ms)`**, 362.4 s after block 0's answer. The uploader took 372.4 s to send (blind pace). `update: restarting into the bootloader` came 2.0 s later (03:30:07.3 UTC) |
| Install | From the restart line to the candidate's first RTT line: **12.5 s**. To session ready (attempt 1, WebSocket open after 1076 ms): **20.1 s**. The uploader logged `bridge reconnected` (03:30:27.4 Z) |
| Candidate boot log | `reset: RCC_CSR 14000002: SOFTWARE PIN`; `image: 171952 bytes at 08010000, VTOR 08010000`; **`clock: SYSCLK 96000000 Hz, APB1 48000000 Hz, APB2 96000000 Hz, from HSE 26 MHz crystal + PLL`**, `HSE ready after 0 ms`, `RCC_CR 03035483 PLLCFGR 2440180d CFGR 0000100a FLASH_ACR 00000703 PWR_CR 0000c000`; **`identity: record seq 1, version 1: ... EUI-64 set, no Ethernet MAC (placeholder MAC, src/net.c)`**; network record seq 1, channel 16; radio calibration 1 try, PLL locked; RX on channel 16 (866324156 Hz), transmitter enabled on this channel only; **`eth: MAC 02:7a:d0:b1:00:01, the placeholder: the identity record (version 1) holds none`**; DHCP lease `10.42.0.43`; **`update: running ba8af1a3e7720e44 (171952 B); boot state testing tries 1 cand ba8af1a3e7720e44 prev 68a24dfcd65549c4; running the candidate under test: confirmation due`**. Against the rows 1-10 candidate's log (numbers masked, `diff`): only the image size, the ids, the frame counter, the PA3 figures and the order of the asynchronous `eth: link up` line differ. Row 19 touched the clock bring-up; the clock lines are the same |
| PA3 start-up test | `passed: 1024 readings, range 673-960, longest run 1, APT max 10 of 512, **1 read timeouts**`. The first non-zero count among the 14 distinct start-up test lines in the RTT logs under `~/ib01-captures/`. A timeout is one ADC read without EOC within `EOC_WAIT_US` 1000 us (`src/pa3_adc.c:24,52`), retried up to `MAX_TIMEOUTS` 8 (`src/entropy.c:56,99-107`). The test runs in the net task (`src/net.c:453`) while the radio task, at a higher priority (`src/app.c:21-22`), does its bring-up, so a preemption longer than 1 ms between `SWSTART` and the EOC check counts as a timeout **[I]**. Row 19 changed only the SysTick and HSI helpers before the scheduler starts (`cleanup-p8.md`, row 19); the ADC and TIM2 code is unchanged **[I]**. Not a stop rule; watch for it in later boots |
| Confirm | **`update: running image ba8af1a3e7720e44... confirmed (boot state CONFIRMED) after 60 s with the server session ready and the radio up`**, 60.5 s after session ready (03:31:27.9 UTC) |
| Stop rules (whole RTT log, from the candidate's start at 03:30:19 to 04:19:20 UTC, 49 minute blocks) | too late 0; refused for airtime 0 frames and 0 acks; tx fail 0, refused 0; radio overflow, bad len, stale, rearm, reinit, spi err, status glitch and irq storm all 0; flash work gave up 0, longest flash cycle 83465 us. Acks prepared equals acks sent in every minute (15 in all). Stack words never used (radio/net/hk/update), lowest: **532/844/332/261**, the same as under the rows 1-10 image (stop rule 100). Heap minimum 8560 bytes. Airtime at most 10184 ms in the last hour, of 30000 ms (the count restarts at boot). No fault, panic or watchdog line. No stop rule was hit |
| Traffic, RTT, during the capture (to 03:47:19 UTC) | 9 `tx:` lines, **all attempt 1, delivered**: 7 unicast datagrams and 2 broadcasts. Full trains of 248 for the first downlink after the boot and for the 2 broadcasts; the other 6 trains were short, of 21-25 wake-ups. `no ack` 0. No beacon request |
| Traffic, RTT, after the capture (to 04:19:20 UTC, no PUTs) | 14 more `tx:` lines (8 unicast, 4 broadcasts), all delivered. **Datagrams 10 and 11** (04:11:21 UTC, 4 s apart) logged `attempt 1: no ack, will retry` after short trains of 26 and 21 wake-ups and were delivered at attempt 2 with full trains; the next three went at attempt 1 with 21. Not on air (the capture had ended), so whether the device missed the train or its ACK was lost is not known; the retry path worked as designed, as for datagram 5 under the rows 1-10 image **[I]**. 20 `beacon request` lines at -61 to -62 dBm from two other senders, in two 42 s bursts (03:48:26-03:49:08 and 04:10:26-04:11:08 UTC); no beacon was sent |

RTT host times are stamped to 1 ms and good to about 10 ms with 10 ms polling. The `(<n> ms)` figures are the firmware's own.

**On air [V].** Receive-only: `rtl_sdr -f 865924158 -s 2400000 -g 8.7` through `tools/sdr/timedcap.py`, 1020.0 s from 03:30:19.4 UTC (the candidate's first RTT line came 0.4 s later) to 03:47:19.4 UTC. No samples were lost (4896325632 bytes against the host span, within 11 ms) and no bytes were clipped. The view covers channels 10-18; channel 26 is outside it. Traffic: 4 overlay PUTs on zone 1 of the local bench server, 252.6-259.8 s apart (at most one per 4 min), alternating 21.5 and 21.0 °C and ending on 21.0 (all 200). Decoding: `demod.py --channels 16 --csv`, then `ackgap.py --list`. **Resolution:** the gap runs from the Data frame's end (last CRC bit) to the ACK's preamble start. Both come from `demod.py` sync fits at 600 kS/s (1.667 us per sample, least squares over every bit-boundary zero crossing; zero-crossing jitter 0.1-0.3 us per frame), good to well under 1 us. Envelope edges are half-amplitude crossings between 600 kS/s samples.

| Item | Result |
| :--- | :--- |
| **Bridge ACK** (image `ba8af1a3e7720e44`) | Data end to ACK preamble start **225.9-231.2 us, median 226.1 us, mean 226.8 us, std 1.8 us (n=7)**. Envelope off to on 199.7-206.6 us, median 200.0 us. 6 of the 7 fall within 225.9-226.3 us; the 231.2 us one answers the uplink after the second PUT. 7 CRC-OK uplinks, **7 ACKed**, none unanswered. Each of the 4 PUT downlinks was followed 0.28-0.29 s later by the device's 51-byte uplink, ACKed at 226.3, 231.2, 225.9 and 226.0 us. Inside 225-240 us. Unchanged against the rows 1-10 image (median 225.9 us, n=5) and step 4 of "Self-update (#24) ... result" (median 226.0 us, n=30) |
| Downlinks | 7 unicast Data frames, all ACKed by the device after 355.4-390.3 us (median 369.9 us), and 2 broadcasts (full trains): the 9 `tx:` lines of the RTT |
| Beacon requests | None on channel 16 in the capture; the bridge sent no beacon |
| Other channels in view | `demod.py --channels 10,11,12,13,14,15,17,18` over the whole 1020 s: 548 sync hits (67-72 per channel), **no CRC-OK frame on any of them** (noise or weak frames). Unlike the three earlier runs, the two other senders' beacon requests on channel 11 fell outside the capture, as did theirs on channel 16 (RTT above). **Nothing of ours off channel 16** |
| Offset (#7) | Bridge +7233 Hz against the dongle at 866.3242 MHz (median of 884 wake-ups, 5-95 % +7135 to +7979 Hz), spare device +12629 Hz (median of its 14 frames): the bridge is **6.2 ppm** below the device. Both drifted down by about 800 Hz together over the run (the dongle warming up **[I]**); taken pair by pair (each device uplink against the bridge frame just before it) the difference is 5.9-6.1 ppm (5.7-8.1 ppm in earlier runs) |

**What this means for #33 [V]/[I].** The P8 rows 11-20 image runs on the bench unit like the rows 1-10 image it replaced. It has the same boot log, including the clock line after the row 19 change to the clock bring-up, the same stack and heap figures, and the same Enhanced ACK gap (median 226.1 us against 225.9 us). Its machine code is the reviewed `dabd7d1` build's, apart from `FW_VERSION`. Once more, the self-update path installed and confirmed a new image starting from `CONFIRMED`. Rule 5 of `cleanup-p8.md` is met for rows 11-20. One PA3 read timeout at boot is new and most likely a preemption **[I]**. Two attempt-1 misses after the capture, both delivered at attempt 2, match the one seen under the rows 1-10 image; they are a link event, not a change in the image, since rows 11-20 leave the radio code's machine code unchanged apart from `__LINE__` immediates (`cleanup-p8.md`, "Rows 11-20: result") **[I]**.

**State the bench was left in (2026-11-20, 04:19 UTC) [V]:** image `ba8af1a3e7720e44` (`dabd7d1`, `FW_VERSION=5`) running from internal flash. Boot state `CONFIRMED` (the confirmation line; no reset since), cand `ba8af1a3e7720e44`, prev `68a24dfcd65549c4`. The previous slot holds `68a24dfcd65549c4`, and the candidate slot holds the new image. Bootloader `eb2f44fb…` unchanged. Identity record version 1 seq 1 in slot A, slot B blank (not provisioned), placeholder MAC. Overlay on zone 1 MANUAL 21 °C (the last PUT, answered 200; not read back). RTT logger and OpenOCD stopped; SDR capture finished. Cables still connected.

## Bench network

Tested 2026-11-11 without changing anything.

| Piece | Root? | Notes |
| :--- | :--- | :--- |
| Network | No | Wired connection `enp196s0f0` in NetworkManager **shared** mode. Verified with the bridge connected (2026-11-11) **[V]**: link up at **10 Mb/s half duplex**, laptop at `10.42.0.1/24`, NAT to Wi-Fi |
| Server address | No | Firmware connects to `10.42.0.1` directly, with that IP in tanoclo's TLS certificate. A DNS name would need root: `/etc/NetworkManager/dnsmasq-shared.d` is root-only **[V]** |
| Docker | **Once, done** | Owner ran `sudo systemctl enable --now docker` (2026-11-11). Daemon active and enabled, server 29.9.3, usable without sudo **[V]** |
| tanoclo port 988 | No | Ports below 1024 are privileged **[V]**, but Docker publishes them |
| Firewall | No | firewalld, ufw, nftables and iptables services all inactive **[V]** |
| ST-Link | No | udev rules with `uaccess` for 0483:3748 and 0483:374b are installed **[V]** |
| RTL-SDR | No | RTL2832U with R820T tuner found and opened by `rtl_test` as a normal user **[V]**; DVB driver blacklisted in `/usr/lib/modprobe.d/rtlsdr.conf` **[V]** |

**DHCP and DNS on the link (#13, 2026-11-14) [V]:** NetworkManager's dnsmasq serves the link: range `10.42.0.10-10.42.0.254`, lease 1 h, DNS and gateway `10.42.0.1` (its command line in `pgrep -a dnsmasq`; the lease the bridge got). Started with `--no-hosts`, it forwards other names to the laptop's upstream resolvers and answers DHCP clients' host names: `tado-bridge` resolves to the bridge's lease. The lease file `/var/lib/NetworkManager/dnsmasq-enp196s0f0.leases` is root-only, but `journalctl` shows `dnsmasq-dhcp` DISCOVER/OFFER/REQUEST/ACK lines without root. `nmcli device disconnect enp196s0f0` removes only the laptop's address and keeps the PHY carrier up, so it does not simulate a cable pull. No `dig` on the laptop; a 30-line Python query does the cross-check.

**Duplex [V]/[I]:** the ENC28J60 is 10BASE-T only and does not auto-negotiate, so the laptop falls back to half duplex. The link comes up even with the MCU halted, because the PHY starts on its own. The P3 driver must run the ENC28J60 in half duplex to match, or both sides must be forced to full duplex; a mismatch works but drops packets.

**ST-Link 3V3 supply, revisited (2026-11-16, #35) [V]:** under the owner's temporary exception (`README.md`, "Project decisions") the bench unit ran from the ST-Link 3V3 for entropy checks 2 and 3. VDDA measured 2.666-2.732 V by VREFINT. The ENC28J60 failed on every retry in one boot (ERXST `ffff`) and worked in another (session ready on attempt 2): marginal, not usable for Ethernet. VDDA is below the 2.7 V that x32 flash programming needs, so never flash on it. The exception is finished and the USB-only rule is back in force; the final state is USB, production image c2b9b4d verified.

**Supply with Ethernet connected [V]:** 3.20 V at the ST-Link with the PHY linked and the MCU halted. **Conflict (2026-11-14) [V]:** the board itself measures only 2.74-2.83 V by VREFINT in the same state, so the ST-Link figure is not the board voltage ("Power risk" above). The current draw that matters comes when the bridge transmits (P3). With the owner-changed supply the board reads 3.28-3.29 V with the PHY linked (core halted for the measurement, so not during a transmission; 2026-11-14) **[V]**.

**With Ethernet plugged in, a reset lets the stock firmware reach the tado cloud** through the laptop's NAT, and transmit on channel 26. The stock firmware has been replaced by the P1 image (2026-11-11 **[V]**); keep it that way.

**tanoclo in Docker on the link (#27, 2026-11-14) [V]:** tanoclo's `docker-compose.yml` publishes 988, 3111, 80 and 443 on all interfaces (`tanoclo/docker-compose.yml:34-51`), so a local override outside both repos binds them to `10.42.0.1` only. Nothing under `../tanoclo` is changed (`git status` clean).

- Override: `~/tanoclo-bench/docker-compose.override.yml`. Start with `cd ../tanoclo && docker compose -p tanoclo-bench -f docker-compose.yml -f ~/tanoclo-bench/docker-compose.override.yml up -d --build` (stop with `down`; add `-v` to drop the database). It uses `ports: !override` for `node` (988, 3111) and `nginx` (80, 443), all as `10.42.0.1:N:N`. The containers have `restart: always` (`tanoclo/docker-compose.yml`), so they come back after a reboot, still bound to `10.42.0.1`; Docker fails to publish if the address is missing.
- **The base compose file does not start as is [V]:** it bind-mounts `./ws-server` over `/app/ws-server` (`tanoclo/docker-compose.yml:33`), which hides the image's `node_modules` (`tanoclo/Dockerfile.node:3-10`), and the node container loops with `Cannot find module 'uWebSockets.js'`. The override drops that mount (`volumes: !override`) and uses the files baked into the image. Also, `nginx` mounts `./ssl`, which does not exist in the repo (`tanoclo/docker-compose.yml:48`); the override mounts the bench certificate directory there instead, so Docker does not create a root-owned `ssl/` in the tanoclo tree.
- The node container downloads an OTA web bundle and APK from `raw.githubusercontent.com` at start-up (log lines `[ota-sync]`); that uses the laptop's internet, not the bridge's.

**How TLS reaches the bridge [D]:** port 988 is a plain Node `tls.createServer` in `ws-server/lib/proxy-manager.js:110-113`, listening via a `net` proxy on `wsPort` (default 988, `tanoclo/ws-server/lib/config.js:47`) and forwarding to an internal uWebSockets server on 19880 that serves `/hw/v2` (`tanoclo/ws-server/server.js:283,402`; proxy `tanoclo/ws-server/lib/proxy-manager.js:146-147`). It takes its key and certificate from `SSL_KEY_PATH` and `SSL_CERT_PATH` (default `ws-server/certs/tanoclo_key.pem`, `tanoclo_cert.pem`, `tanoclo/ws-server/lib/config.js:50-51`); a per-client-IP certificate map `certs/ip_certs.json` is optional (`tanoclo/ws-server/lib/config.js:15-31`). No client certificate is requested. The server also reads `TADO_ROOT_CA_PATH` at start-up (`tanoclo/ws-server/server.js:80`, `tanoclo/ws-server/lib/config.js:52`); it only matters for the tado-cloud proxy mode (`tanoclo/ws-server/lib/proxy-manager.js:198`), which is off. The stock procedure clones tado's root CA and signs a leaf for `tanoclo.tado.lan` with a DNS SAN only (`tanoclo/patch_ib_firmware/clone_chain.sh:97-150`); our firmware does not patch the stock image, so the bench uses its own CA with an IP SAN. Design: TLS 1.2, P-256 ECDSA (`docs/design.md:155`).

**Bench certificate [V]:** all in `~/tanoclo-bench/certs/` (outside git; `bench-ca.key` is mode 600).

| Item | Value |
| :--- | :--- |
| Bench CA | `bench-ca.pem`, `CN=tado-bridge-firmware bench CA`, self-signed, P-256, CA:true pathlen 0, valid to 2046-11-09. SHA-256 fingerprint `E9:E9:0E:A4:59:40:56:AD:7D:CA:85:FF:BB:AD:F3:3C:96:4F:42:0A:8F:86:43:6E:0B:90:0B:DA:43:9F:18:9E` |
| Server leaf | `tanoclo_cert.pem` / `tanoclo_key.pem`, `CN=10.42.0.1`, **SAN `IP:10.42.0.1`**, P-256, serverAuth, signed directly by the bench CA, valid to 2046-11-09. Copies as `tanoclo.fullchain` / `tanoclo.key` for nginx |
| Trust anchor for P4 (#15) | **`bench-ca.pem`** (public part only; embed as a build or config value, `docs/design.md:156`). The firmware must verify the IP SAN `10.42.0.1`, not a DNS name. The leaf is sent without the CA; the chain is leaf plus anchor. Pinning the leaf instead would also work, but then every re-issue needs a new firmware |
| `tadoRootCA.cer` | A copy of `bench-ca.pem`, only so the server finds the file it reads at start-up |

The certificate validity runs to 2046, so a bridge with no real-time clock cannot fail on dates other than by having a wrong clock **[I]**; the firmware has no time source yet.

**Verification (2026-11-14) [V]:**

| Check | Result |
| :--- | :--- |
| `ss -ltnp` | Listeners only on `10.42.0.1:988`, `:3111`, `:80`, `:443`; none on `0.0.0.0` or `[::]` |
| TLS to `10.42.0.1:988` | `openssl s_client -CAfile bench-ca.pem -verify_ip 10.42.0.1`: `Verification: OK`, TLS 1.3 `TLS_AES_256_GCM_SHA384`, ecdsa_secp256r1_sha256; the leaf shows `IP Address:10.42.0.1` |
| TLS 1.2 as the firmware will use | `-tls1_2 -cipher ECDHE-ECDSA-AES128-GCM-SHA256`: negotiated, `Verification: OK` |
| `10.42.0.1:443` (nginx) | `Verification: OK` with the same certificate |
| Wi-Fi address `10.13.1.210` | 988, 3111, 80, 443: connection refused |
| Containers | `tanoclo-ws` healthy, `tanoclo-db` and `tanoclo-nginx` up |

**The bridge on `/hw/v2` (#15, 2026-11-14) [V]:** the WebSocket upgrade, auth and time from the bridge work, with the certificate and its IP SAN verified by the firmware. The bridge is registered with one `devices` row in the bench database. The database previously had no homes, users or devices. The row, how to remove it, and the `log_level` setting: `build.md`, P4 TLS. For packet captures on port 988, a throwaway sidecar in tanoclo's network namespace works without root on the host, with the captures kept out of git (`build.md`, P4 TLS). The bridge's TCP peer is Node inside the container itself; the host has no established socket for it.

Not yet tested: the `tun0` (192.168.124.6) interface, which is not covered by the bind but gets nothing because Docker publishes on `10.42.0.1` only **[I]**.

**NAT gives the bridge internet access.** Our firmware never contacts tado, but the stock firmware would try to reach the tado cloud if it ran with Ethernet plugged in. It is no longer on the board since P1 (2026-11-11), so this only matters after a restore. P0 runs with Ethernet unplugged anyway (`tanoclo/patch_ib_firmware/README.md:247`).
