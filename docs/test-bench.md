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
6. **Fresh network key** for the test network, generated as in `design.md`. Never try to obtain the stock network's key.
7. **No test device yet** (owner, 2026-11-11). Up to P4 no device is needed. Decide before P5, without taking devices from the stock network unless the owner chooses to.
8. **Listen-before-transmit stays on,** which also helps coexistence (`radio-runtime.md`).

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

## Bench network

Tested 2026-11-11 without changing anything.

| Piece | Root? | Notes |
| :--- | :--- | :--- |
| Network | No | The owner set the wired connection (`Wired connection 1`, `enp196s0f0`) to NetworkManager **shared** mode **[V]**. With a cable in, it gets `10.42.0.1/24` and NetworkManager's own DHCP and DNS, and NATs to Wi-Fi **[D]** (NetworkManager default; the address appears once a cable is in) |
| Server address | No | Firmware connects to `10.42.0.1` directly, with that IP in tanoclo's TLS certificate. A DNS name would need root: `/etc/NetworkManager/dnsmasq-shared.d` is root-only **[V]** |
| Docker | **Once** | User is in the `docker` group, but the daemon is not running **[V]**. Owner runs `sudo systemctl enable --now docker` |
| tanoclo port 988 | No | Ports below 1024 are privileged **[V]**, but Docker publishes them |
| Firewall | No | firewalld, ufw, nftables and iptables services all inactive **[V]** |
| ST-Link | No | udev rules with `uaccess` for 0483:3748 and 0483:374b are installed **[V]** |
| RTL-SDR | No | RTL2832U with R820T tuner found and opened by `rtl_test` as a normal user **[V]**; DVB driver blacklisted in `/usr/lib/modprobe.d/rtlsdr.conf` **[V]** |

**Keep the test server off the home LAN:** tanoclo's `docker-compose.yml` publishes 988, 3111, 80 and 443 on all interfaces (`tanoclo/docker-compose.yml:34-51`). Use a local override that binds them to `10.42.0.1` only. The override lives outside the tanoclo repo.

**NAT gives the bridge internet access.** Our firmware never contacts tado, but the stock firmware still on the board would try to reach the tado cloud if it ran with Ethernet plugged in. P0 runs with Ethernet unplugged anyway (`tanoclo/patch_ib_firmware/README.md:247`).
