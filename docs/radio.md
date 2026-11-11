# 868 MHz radio

Paths are relative to the workspace root. `wiki/` means `Tado_Hardware_Teardown.wiki/`.

Status tags: **[V]** measured on real hardware by a repo author, **[D]** stated in a repo or wiki, **[I]** our own inference.

**Scope limit:** security was reviewed only for what the firmware needs as coordinator of its own network. `wiki/Tado-868-MHz-Link-Protocol.md` section 11 was not read. Key values are left in their source documents.

## Stack [D]

CoAP / UDP / IPv6 link-local / 6LoWPAN IPHC / 802.15.4 AES-CCM* / 802.15.4-2015 LE CSL / 868 MHz GFSK (`wiki/Tado-868-MHz-Link-Protocol.md:80-96`).

**Single-hop star, no routing protocol** (`wiki/Tado-868-MHz-Link-Protocol.md:98-102`). There is no mesh to implement, even though tanoclo's docs say "mesh". The MAC is software on a plain GFSK radio (`:107`). Stock is probably Contiki (`uip_ds6_nbr` in `tanoclo/docs/coap_tlv.md:617`, `packetbuf` in `tanoclo/docs/emulated_devices.md:80`, RFC 4861 defaults in `freedo/docs/PROTOCOL.md:1379`) **[I]**.

## PHY [V]

| Parameter | Value | Source |
| :--- | :--- | :--- |
| Channels | 0-49, `863.125 MHz + n × 199.951 kHz` | `wiki/RF-PHY-Parameters.md:4-7`, `tanoclo/docs/rf_protocol.md:51-59` |
| Default channel | 26 (868.3237 MHz); may differ per install | `wiki/RF-PHY-Parameters.md:21-23` |
| Modulation | GFSK, 25.39 kHz deviation | `wiki/RF-PHY-Parameters.md:6-8` |
| Bit rate | 49,987.79 bps | `wiki/RF-PHY-Parameters.md:9` |
| RX bandwidth | 135.416 kHz | `wiki/RF-PHY-Parameters.md:5` |
| Preamble | 4 bytes, 640 µs | `wiki/RF-PHY-Parameters.md:12,32` |
| Sync | `D3 91`, 30/32 bits | `wiki/RF-PHY-Parameters.md:10-11` |
| Whitening | None | `wiki/RF-PHY-Parameters.md:14` |

At power-on stock calibrates across channels 0-49, then selects 26 (`wiki/RF-PHY-Parameters.md:19`).

## Stock CC110L register table [V]

Burst write from `IOCFG2` (0x00) to `TEST0` (0x2E), from a logic capture (`wiki/CC110-Register-Configuration.md:43-95`).

| Addr | Reg | Val | Addr | Reg | Val | Addr | Reg | Val |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| 00 | IOCFG2 | 04 | 10 | MDMCFG4 | AA | 20 | WORCTRL | FB |
| 01 | IOCFG1 | 2E | 11 | MDMCFG3 | F8 | 21 | FREND1 | 56 |
| 02 | IOCFG0 | 01 | 12 | MDMCFG2 | 13 | 22 | FREND0 | 10 |
| 03 | FIFOTHR | 03 | 13 | MDMCFG1 | 22 | 23 | FSCAL3 | E9 |
| 04 | SYNC1 | D3 | 14 | MDMCFG0 | F8 | 24 | FSCAL2 | 2A |
| 05 | SYNC0 | 91 | 15 | DEVIATN | 40 | 25 | FSCAL1 | 00 |
| 06 | PKTLEN | 80 | 16 | MCSM2 | 07 | 26 | FSCAL0 | 1F |
| 07 | PKTCTRL1 | 04 | 17 | MCSM1 | 3F | 27 | unused | 41 |
| 08 | PKTCTRL0 | 05 | 18 | MCSM0 | 08 | 28 | unused | 00 |
| 09 | ADDR | 00 | 19 | FOCCFG | 16 | 29 | reserved | 59 |
| 0A | CHANNR | 00, later 1A | 1A | BSCFG | 6C | 2A | reserved | 7F |
| 0B | FSCTRL1 | 18 | 1B | AGCCTRL2 | 43 | 2B | reserved | 3F |
| 0C | FSCTRL0 | 00 | 1C | AGCCTRL1 | 40 | 2C | TEST2 | 81 |
| 0D | FREQ2 | 21 | 1D | AGCCTRL0 | 91 | 2D | TEST1 | 35 |
| 0E | FREQ1 | 32 | 1E | WOREVT1 | 87 | 2E | TEST0 | 09 |
| 0F | FREQ0 | 76 | 1F | WOREVT0 | 6B | | | |

No PATABLE write was seen in the capture, so stock appears to run at the reset default (`wiki/CC110-Register-Configuration.md:97`).

Corrections to the wiki's descriptions, confirmed in the CC110L datasheet (`docs/datasheets/cc110l.txt:1234,2914,3607-3610`):
- `MCSM1=0x3F` means TXOFF goes to **RX**, not "stay in TX" (`:72`). This agrees with `freedo/docs/PROTOCOL.md:1558`.
- `IOCFG2=0x04` drives **GDO2**, not "GD0" (`:49`).
- `PKTLEN=0x80` is the maximum packet length in variable-length mode, so it is used, not "not used" (`:55`).
- The "packet length 16 + 2" in `wiki/RF-PHY-Parameters.md:15` is sniffer configuration; the link is variable length.

**FIFO strategy:** stock interrupts on GDO0 at an RX FIFO threshold of 16 bytes or end of packet (`IOCFG0=0x01`, `FIFOTHR=0x03`). Freedo drains frames over the 64-byte FIFO the same way, on real hardware (`freedo/firmware/cc1101_bridge/README.md:72-73`).

## Frames [D]

Layouts: `wiki/Tado-868-MHz-Link-Protocol.md:249-355`.

| Frame | FCF | Notes |
| :--- | :--- | :--- |
| Wake-up | MPFC, 1 byte, type 5 | PAN, short dst, Rendezvous Time IE, Header Termination IE |
| Unicast Data | `69 EC`, `79 EC` with Frame Pending | 8-byte dst, 5-byte compressed src, ciphertext |
| Broadcast Data | `49 E8` | dst `FFFF`, 8-byte src, explicit security header |
| Enhanced ACK | `42 EE` | 29 bytes, **not encrypted**, carries CSL phase and period |

Constants: PAN `0xABCD`, network suffix `31 07 c5 1b 00`, CSL period `0x0C35` = 3125 units, IEs `0x1A` CSL, `0x1D` Rendezvous, `0x7F` Termination (`wiki/Tado-868-MHz-Link-Protocol.md:522-528`).

## Data frame plaintext [D]

`[c5 1b 00][04][counter: 4 bytes LE] | 6LoWPAN + CoAP | [inner CRC: 2]` (`freedo/tado/coap.go:16`, `freedo/tado/build_coap.go:115`, `freedo/tado/frame.go:18`).

The first eight bytes look like the rest of an 802.15.4 header (address tail, security control, counter) that happens to fall inside the encrypted part **[I]**. tanoclo labels offsets 3-8 differently (`tanoclo/docs/rf_protocol.md:172-182`); follow freedo, whose frames are validated on air.

On-air size: 4 preamble + 4 sync + 1 length + frame + 2 CRC bytes, at 160 µs per byte **[I]**.

## Coordinator behaviour [V]

From `freedo/docs/PROTOCOL.md`, measured from captures of a commercial coordinator unless noted.

- **Wake-up trains:** only the coordinator sends them. 21 frames over 79 ms, or 254 frames over 1.003 s; step 4.0 ms. Data starts 9-24 ms after the last wake-up frame (`:1431-1437`).
- **Enhanced ACK:** see conflict below (`:659-660`, `:1438`). Its FCS: `:1305-1313`.
- **Retransmission:** same MAC seq, next frame counter (`:1317-1321`). Frame Pending rule: `:1326-1330`.
- **Router Advertisements:** every 200-600 s. Devices stop reporting after about 20 minutes without one, then send Router Solicitations (`:1378-1383`). Solicited RA behaviour: `:1389-1407`.
- **RA contents:** option 25 with netmask, gateway, DNS (`:1409-1413`).
- **Frequency trim:** freedo's CC1101 module needed a static FSCTRL0 trim (`:1670-1718`). Stock writes `FSCTRL0=0`.
- **Fragmentation:** 6LoWPAN FRAG1/FRAGN (`tanoclo/docs/rf_protocol.md:265-376`).
- **Device firmware transfer:** `POST d/fw`, Block1 at 128 bytes (`freedo/docs/PROTOCOL.md:1536-1540`). **Never run on air** (`:1545`).

## Link security and pairing

Follow freedo and tanoclo (project decision).

**Frame protection [D]**
- AES-128-CCM, 4-byte MIC, 13-byte nonce = first 13 header bytes, 16-byte AAD (`tanoclo/docs/rf_protocol.md:130-134`, `freedo/docs/PROTOCOL.md:655-656`).
- Frame counter is an application field in the plaintext, not part of the nonce. Devices accept lower counters, so there is no anti-replay enforcement; freedo still keeps it monotonic across restarts (`freedo/docs/PROTOCOL.md:1288-1299`). A broadcast takes the last unicast counter plus one (`:1291-1292`).
- Enhanced ACKs are unencrypted (`wiki/Tado-868-MHz-Link-Protocol.md:385`).

**Keys [D]** (`tanoclo/docs/pairing.md:77-82`)
- **Pairing key:** one static key shared by all devices; value in `tanoclo/docs/rf_protocol.md:161-163`.
- **Network key:** one per home. The new firmware generates it, since existing devices are re-paired.
- **Factory key:** per device, held only by the cloud. Not available and not needed.

**Pairing flow** (`tanoclo/docs/pairing.md:9-17`, `freedo/docs/PROTOCOL.md:623-739`)
1. Server opens the window with `POST .../pair`. Freedo caps windows at 10 minutes (`:700`).
2. Joiner broadcasts Enhanced Beacon Requests and Router Solicitations under the pairing key (`:641-643`).
3. During the window, a frame failing the network key is retried under the pairing key, and that sender is tagged to use it (`:692-705`).
4. **Beacon answer must come from the bridge firmware**, 2.4-7.4 ms after the request, with no wake-up train (`:657-658`, `:721-724`). Layout `:651-656`.
5. Each Router Advertisement is sent twice in the window: network key with Frame Pending (`59e8`), then pairing key (`49e8`) (`:668-672`).
6. `d/pair` goes to the joiner under the pairing key: CON POST, Content-Format 42, single-use 8-byte option 2048, one TLV. It jumps the downlink queue (`:623-633`, `:707-713`).
7. Use tag `0x12` (network key, length 16). Tag `0x07` needs the cloud-only per-device key (`:630-633`).
8. Device answers 2.04 under the new network key within 30 ms, then `auth/token`, RS, `time`, config, and starts reporting (`:635-637`, `tanoclo/docs/pairing.md:102-112`).

**Exposure:** the network key crosses the air under a key every valve shares, so keep windows short and the band quiet (`freedo/docs/PROTOCOL.md:737-738`).

**Validation limits:** freedo's firmware responder path during pairing is "not validated on air" (`:732`). The pairing frames themselves are rebuilt byte for byte from captures (`:644-645`).

## Conflicts

| Topic | Positions | Resolution |
| :--- | :--- | :--- |
| Link security | Level 4 ENC, no MIC, counter in nonce (wiki `:381-385`, `:429-432`); CCM with 4-byte MIC, nonce from header (`tanoclo/docs/rf_protocol.md:130-134`, `freedo/docs/PROTOCOL.md:655-656,1289`) | **Resolved:** follow freedo and tanoclo |
| Frame version | `tanoclo/docs/rf_protocol.md:98` says 2003; wiki `:195` says 2-2015 | **Resolved:** 0xEC69 bits 12-13 decode to 2, so 2015 |
| `0x0E82` "bridge short address" | `tanoclo/docs/rf_protocol.md:440` | **Resolved [I]:** it is the Rendezvous Time IE descriptor (`0x1D<<7 \| 2`), not an address |
| `0x3F80` "CSL period, ~5 s" | `tanoclo/docs/rf_protocol.md:442,448` | **Resolved [I]:** it is the Header Termination IE (`0x7F<<7`). Real period is 3125 units, about 1 s |
| Wake-up to Data gap | 9-24 ms after the last wake-up frame (`freedo/docs/PROTOCOL.md:1431-1437`) | **Open.** Measured on the stock network 2026-11-11 with `tools/sdr/demod.py` **[V]**: Data starts 0.4-3.8 ms after the last wake-up frame ends (median 1.4 ms); train steps 3.94-3.99 ms or 4.40-4.45 ms (`test-bench.md`, "Stock network reference"). Possibly freedo measured from the start of the last wake frame or across a missed frame **[I]**. Follow the stock measurement; recheck when our own trains run (P5) |
| Train shape | 10 frames, 25 ticks (`tanoclo/docs/rf_protocol.md:446`); 21 or 254 frames, 12.5 units, 4 ms step (`freedo/docs/PROTOCOL.md:1435-1436`) | **Resolved:** follow freedo (measured). The user also observed the 1 s wake train in their own RF measurements of the stock bridge (2026-11-11). A wake-up frame takes about 3.7 ms on air, so 4 ms is back-to-back; tanoclo's 8 ms probably reflects a sniffer catching every other frame **[I]** |
| Checksums | One CRC-16/CMS FCS, big-endian (wiki `:140-152`); software FCS CRC-16/CCITT reflected inside the frame plus radio CRC (`freedo/docs/PROTOCOL.md:1305-1308`) | **Resolved: two layers.** Outer: CRC-16/CMS, which is the radio's own CRC (`PKTCTRL0.CRC_EN`), appended in hardware (`freedo/tado/build_mac.go:13-14`, `freedo/tado/mac.go:18`, dissector `packet-tado.c:278-279`). Inner: software CRC-16/CCITT reflected, poly `0x8408`, init 0, little-endian (`freedo/tado/build_mac.go:104-111`, `tanoclo/docs/rf_protocol.md:194-198`). The inner one is encrypted in Data frames and in clear on Enhanced ACKs. The wiki describes only the outer one |
| Source address tail | Fixed suffix elided (wiki `:241-247`); 3 bytes carried encrypted (`tanoclo/docs/rf_protocol.md:105`) | **Resolved: both true.** The tail is sent encrypted at the start of the plaintext and always equals the fixed suffix `c5 1b 00` (`freedo/tado/coap.go:16`). See "Data frame plaintext" |
| ACK timing | Ends 6-7 ms after, 0-1 ms gap (`freedo/docs/PROTOCOL.md:659-660`); 5-8 ms after (`:1438`) | **Resolved [I]:** both measure end of Data to end of ACK. The ACK takes about 6.4 ms on air (40 bytes at 160 µs), so the real gap is 0-1 ms and 5-8 ms is capture jitter. **Measured on the stock network (2026-11-11) [V]:** the coordinator's ACK starts 1.50-1.99 ms after the Data frame ends, a device's 0.35-0.39 ms (`test-bench.md`, "Stock network reference"). Freedo's 0-1 ms gap fits the device ACKs. **Target (owner decision, 2026-11-11): the ACK starts within 1 ms of the end of the Data frame.** The owner reports that devices on the stock network show sync issues today, so stock's 1.5-2 ms is not the reference. Approach: prebuild the ACK while the Data frame is still arriving (see "ACK prebuild" below) |
| Retry gap | 2-6 s (`freedo/docs/PROTOCOL.md:1321`); exactly 2 or 3 s (`:1440`) | **Provisional:** different captures, both from a commercial coordinator. Retry after 2-3 s, up to three attempts, keeping the MAC seq and advancing the counter |
| Beacon PAN | `0xFFFF` (`tanoclo/docs/rf_protocol.md:438`); `0xABCD` (wiki `:231`) | **Resolved:** wake-up frames use `0xABCD` (`freedo/tado/mac.go:23`, dissector `packet-tado.c:71`). `0xFFFF` appears on a joiner's Beacon Request (`freedo/tado/beacon.go:17`). Data frames carry no PAN field; tanoclo's "PAN" there is the first two destination address bytes |
| Payload encoding | JSON/TLV8 (wiki `:454`); u16 FID TLV (`tanoclo/docs/coap_tlv.md:183`) | Prefer tanoclo; it is what the server parses |
| Pairing protocol | HomeKit Accessory Protocol over CoAP (wiki `:456`); tanoclo documents its own pairing flow | **Resolved by user decision:** follow freedo and tanoclo; the HomeKit chip is not needed |
| `FSCTRL1` | `0x18` (wiki capture); `0x06`, "Tado firmware value" (`freedo/cc1101/cc1101.go`) | **Resolved by user decision:** use `0x06` |
| Radio part | CC2544 (`tanoclo/docs/websocket.md:301,304`); CC110L elsewhere | **Resolved:** CC110L, seen on the board and SPI bus |

## Code to reuse

| Repo | Licence | Use |
| :--- | :--- | :--- |
| `freedo` | Apache-2.0 | `firmware/cc1101_bridge/cc1101_bridge.ino` (2287 lines, ESP32 + CC1101): closest reference for FIFO and CSL timing. `cc1101/`, `tado/` (Go): logic reference, needs porting to C |
| `tanoclo` | MIT | `esphome/tado_emulator`, `tado_sniffer` (C++, SX1276): frame handling reference |
| `Tado_Hardware_Teardown` and wiki | No licence file | Use the facts; copy no code |

The CC110L is a simpler CC1101. Freedo's register use carries over: see `datasheets/cc110l-vs-cc1101.md`.

## ACK prebuild (P5 plan) [I]

The Enhanced ACK is 29 bytes and unencrypted (`wiki/Tado-868-MHz-Link-Protocol.md:335-356`). Almost all of it is known before the Data frame ends:

| Field | Bytes | Source | Known when |
| :--- | :-- | :--- | :--- |
| Length, FCF `42 EE` | 1 + 2 | constant | always |
| Seq | 1 | seq of the Data frame | after its 3rd byte |
| Dst EUI-64 | 8 | the Data frame's source address | after its header, about 20 bytes in |
| Src EUI-64 | 8 | our own EUI-64 | always |
| CSL IE descriptor, CSL period `0x0C35` | 2 + 2 | constant | always |
| CSL phase | 2 | our schedule, computed for the predicted ACK start time | after the header |
| Header termination IE | 2 | constant | always |
| Inner CRC-16/CCITT | 2 | software, over the above | once seq, dst and phase are in |
| Outer CRC-16 | 2 | radio hardware (`PKTCTRL0.CRC_EN`) | sent by the chip |

Plan:
1. Keep the constant parts in RAM per paired device, so only seq, phase and the inner CRC are computed per frame.
2. As soon as the RX FIFO threshold delivers the Data header, check that it is a unicast Data frame addressed to us, then write the whole ACK into the TX FIFO. The CC110L has separate 64-byte RX and TX FIFOs (`datasheets/cc110l.txt:1173,1485`), so this happens during reception.
3. At the end of the frame (GDO0 falling edge), check CRC_OK and send a single `STX`. RX to TX takes about 30 µs (`datasheets/cc110l-vs-cc1101.md`). On a bad CRC or a full inbound queue (`radio-runtime.md`), send `SFTX` instead and no ACK.
4. The chip sends its preamble and sync word (about 1.28 ms at 50 kbps) before the first FIFO byte. That gives extra slack, but it does not count towards the 1 ms target, which is measured to the start of the preamble with `tools/sdr/demod.py`.

Expected start of the ACK: about 0.1-0.3 ms after the Data frame ends, depending on how fast the end-of-frame interrupt reaches the `STX`. It must also not wait behind flash access on SPI1 (`radio-runtime.md`: no flash access during an ACK window).
