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
- `tools/sdr/burstzoom.py`: per-pulse close look at one stretch of a capture (edges, carrier, tone/sweep/FSK, -10 dB width, optional PNG), for bursts `burstscan.py` cannot explain.

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
4. Owner: hold the bridge's pair button for 1 s, then put the spare device into pairing mode (the window lasts 10 minutes, the device's own about 2). The pairing LED (PB8) flashes while the window waits, is lit while the spare device is pairing (from the beacon until its 2.04, a failure or 30 s without progress), flashes again afterwards, and goes off when the window closes after 10 minutes; a pairing does not close it (owner spec 2026-11-17). The first network is created at that moment if the flash holds none (this erases the stock configuration sector at `0x102000`, owner decision 2026-11-17).
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
| **ACK during a handshake** | **Not measured: no device Data frame fell in any handshake window** (handshake -0.3 s to +0.2 s). See "Why no ACK during a handshake" below |
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

**What this means for #16 and #18 [I].** The ACK starts 226 us after the Data frame in normal operation, also right after server and TLS record traffic: a quarter of the 1 ms target, faster than stock devices (about 0.38 ms) and the stock coordinator (1.5-2.0 ms). The target holds. The second condition of #18, "also during a TLS handshake", is still unmeasured on air; only the design argues for it. #18 stays open until a device frame is caught during a handshake.

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
