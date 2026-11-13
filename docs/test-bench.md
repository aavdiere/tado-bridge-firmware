# Test bench

How the bench IB01 is connected, what Claude may do with it, and how the owner's other tado network is protected.

Owner decisions, 2026-11-11, unless noted. Status tags: **[V]** measured, **[D]** documented, **[I]** inference.

## Setup

| Link | Connection |
| :--- | :--- |
| SWD | ST-Link V2 on the test points: 3V3, GND, SWDIO, SWCLK (`hardware.md`) |
| Power | **ST-Link 3V3 only.** USB stays unplugged while the ST-Link is attached (`tanoclo/patch_ib_firmware/README.md:211`) |
| Ethernet | Bridge straight to the laptop's wired port `enp196s0f0`, on a private subnet of its own |
| RF | RTL-SDR dongle on the laptop, receiving 863-873 MHz |
| Server | tanoclo in Docker on the laptop, reachable only on the private Ethernet link |

**Power risk:** the ENC28J60 draws much more current while transmitting than the MCU and radio. A clone ST-Link's 3.3 V regulator may sag under that load. The fallback is a current-limited 3.3 V bench supply with the ST-Link wired without its 3V3. That fallback is a decision for the owner.

**The sag is real (2026-11-14) [V]:** with the ENC28J60 PHY active and the link up, the board's VDDA measures 2.67-2.83 V, below the chip's 3.10 V minimum. With the chip in power save it measures 3.09-3.21 V. The ST-Link reports 3.20 V in both cases, so its own reading does not show the sag. Out of range, the chip stops answering on SPI2 (`hardware.md`, "Cause: the supply sags below the ENC28J60's minimum"). This blocks P3.1 until the owner decides on the supply. Check it with `tools/bench/enc28j60_supply.tcl`.

## What Claude may do (owner, 2026-11-11)

Claude may flash the bench bridge and run tests on it, within these gates:

1. **P0 first.** No new firmware until the backup is done twice and compared, read protection is confirmed at level 0, and the restore has been rehearsed. Claude may run P0 itself once the hardware is connected.
2. **Never change option bytes** (read protection, write protection, brown-out level) without asking. A wrong read-protection change can erase the chip or lock it permanently (`design.md`).
3. **Flash with OpenOCD and verify** every write. Keep the backup images ready for a restore.
4. **Review first:** `fw-reviewer` must give a "safe to flash" verdict before radio transmit code (P5) or bootloader code (P7) goes onto the board.
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
5. **Pairing windows only on the test channel,** and keep them short. Stock devices only join while in pairing mode, but our Router Advertisements under the pairing key must not reach the stock channel.
   **Pair only a device named by its EUI-64.** A tado device nearby scans channels with Beacon Requests (scan below) and could otherwise join the test network. Freedo's pairing flow already supports naming the target (`freedo/docs/PROTOCOL.md:699-705`).
6. **Fresh network key** for the test network, generated as in `design.md`. Never try to obtain the stock network's key.
7. **No test device yet** (owner, 2026-11-11). Up to P4 no device is needed. Decide before P5, without taking devices from the stock network unless the owner chooses to.
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
- `tools/bench/enc28j60_supply.tcl`: board VDDA from the MCU's VREFINT and SPI2 read success per SCK, with the ENC28J60 PHY on, then in power save, then on again (#31). Restores every register it touches.
- `tools/sdr/chanscan.py`, `tools/sdr/frames.py`: channel activity and tado frame decoding.
- `tools/sdr/demod.py`: per-frame GFSK demodulator with sub-microsecond timestamps, carrier offset and MAC header fields (#26). Usage and accuracy below.
- `tools/sdr/burstscan.py`: power-versus-time scan of a whole capture; every energy burst per channel must be a decoded frame, spill-over from a stronger channel, or is listed with duration, centroid, width and symbol rate. Used to confirm that a receive-only image emits nothing.
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

**Test channel: 10 (865.1245 MHz), confirmed by the owner (2026-11-11).** It is quiet, 16 channels from the protected channel, and inside tado's own channel plan, which stock bridges also pick from. Regulatory limits per sub-band (duty cycle, listen-before-transmit) were not checked here **[I]**; re-check activity before first transmit.

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

Tune 400 kHz below the channel (`--fc` must equal `rtl_sdr -f`) to keep the dongle's DC spike out of the band. For the test channel 10 use `-f 864724451` and `--channels 10`. Captures, CSV files and printed payloads stay in `~/ib01-captures/`, never in git: they carry the stock network's addresses.

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

**Duplex [V]/[I]:** the ENC28J60 is 10BASE-T only and does not auto-negotiate, so the laptop falls back to half duplex. The link comes up even with the MCU halted, because the PHY starts on its own. The P3 driver must run the ENC28J60 in half duplex to match, or both sides must be forced to full duplex; a mismatch works but drops packets.

**Supply with Ethernet connected [V]:** 3.20 V at the ST-Link with the PHY linked and the MCU halted. **Conflict (2026-11-14) [V]:** the board itself measures only 2.74-2.83 V by VREFINT in the same state, so the ST-Link figure is not the board voltage ("Power risk" above). The current draw that matters comes when the bridge transmits (P3).

**With Ethernet plugged in, a reset lets the stock firmware reach the tado cloud** through the laptop's NAT, and transmit on channel 26. The stock firmware has been replaced by the P1 image (2026-11-11 **[V]**); keep it that way.

**Keep the test server off the home LAN:** tanoclo's `docker-compose.yml` publishes 988, 3111, 80 and 443 on all interfaces (`tanoclo/docker-compose.yml:34-51`). Use a local override that binds them to `10.42.0.1` only. The override lives outside the tanoclo repo.

**NAT gives the bridge internet access.** Our firmware never contacts tado, but the stock firmware would try to reach the tado cloud if it ran with Ethernet plugged in. It is no longer on the board since P1 (2026-11-11), so this only matters after a restore. P0 runs with Ethernet unplugged anyway (`tanoclo/patch_ib_firmware/README.md:247`).
