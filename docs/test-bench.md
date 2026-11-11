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

**Power risk [I]:** the ENC28J60 draws much more current while transmitting than the MCU and radio. A clone ST-Link's 3.3 V regulator may sag under that load. Watch for resets or brown-outs from P3 onward. If they appear, the fallback is a current-limited 3.3 V bench supply with the ST-Link wired without its 3V3. That fallback is a decision for the owner.

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

- **Do not let the stock firmware run on the bench.** It ran briefly when the ST-Link first powered the board. Since P0 the core is halted (`DHCSR` S_HALT set).
- Re-plugging the ST-Link or a reset starts the stock firmware again. Halt it at once (`openocd ... -c init -c halt -c shutdown`) until P1 replaces it.
- The first P1 flash replaces the stock firmware for good; the backup and the proven restore bring it back if needed.

## Bench tools

- `docs/tools/read_ib01_system_areas.tcl`: option bytes, OTP, unique ID (read-only).
- `tools/bench/io_check.tcl`: drives the LEDs in a fixed order and logs button presses as pin changes; peripheral registers only, undone by the final reset. Keeps the stock firmware halted.
- `tools/sdr/chanscan.py`, `tools/sdr/frames.py`: channel activity and tado frame decoding.

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

**Supply with Ethernet connected [V]:** 3.20 V at the ST-Link with the PHY linked and the MCU halted. The current draw that matters comes when the bridge transmits (P3).

**With Ethernet plugged in, a reset lets the stock firmware reach the tado cloud** through the laptop's NAT, and transmit on channel 26. Keep the core halted until P1 replaces the stock firmware.

**Keep the test server off the home LAN:** tanoclo's `docker-compose.yml` publishes 988, 3111, 80 and 443 on all interfaces (`tanoclo/docker-compose.yml:34-51`). Use a local override that binds them to `10.42.0.1` only. The override lives outside the tanoclo repo.

**NAT gives the bridge internet access.** Our firmware never contacts tado, but the stock firmware still on the board would try to reach the tado cloud if it ran with Ethernet plugged in. P0 runs with Ethernet unplugged anyway (`tanoclo/patch_ib_firmware/README.md:247`).
