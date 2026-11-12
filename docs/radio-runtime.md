# Radio runtime behaviour

How to drive the CC110L after init. Source is freedo's working ESP32 + CC1101 firmware: `freedo/firmware/cc1101_bridge/cc1101_bridge.ino` (`.ino` below) and its `README.md`. Validated on air by freedo on a CC1101, not on an IB01.

The stock SPI sequence after init has not been decoded. See `open-questions.md` #1.

Status tags: **[D]** stated in code or docs, **[I]** our own inference.

## Reset and init [D]

1. Manual power-on reset, then `SRES` (`.ino:582-596`).
2. Drive both GDO pins low and read them back to confirm the chip accepted writes (`.ino:599-603`). After reset `IOCFG0` outputs a 135 kHz clock that would flood the interrupt (`.ino:143-146`).
3. Load the register table (`radio.md`), with `FSCTRL1=0x06`, then calibrate and enter RX.

## Receive [D]

- **Interrupts (P2 implementation: GDO0 on both edges, see below):** GDO0 stamps end of frame with a timestamp, GDO2 signals RX FIFO threshold. A periodic tick (8 ms in freedo) also pumps the FIFO, because an overflow raises no interrupt. The FIFO overflows about 11 ms after a frame starts (`README.md:71-77`).
- **Reading the FIFO** (`rxPump`, `.ino:772-816`): read the length byte first, then expect `1 + len + 2` bytes in total. While still receiving, **leave one byte in the FIFO**. Read everything only when the rest of the frame is there.
- **Overflow or bad length:** flush and re-arm.
- **Status bytes:** the last appended byte holds `CRC_OK` in bit 7 (`.ino:700`).
- **`FREQEST`:** freedo reads it when the frame completes, before any strobe or TX (`README.md:49-53`). On the CC110L with `MCSM1 = 3F` that is too late; P2 reads it in the packet (below, #11).
- **Errata:** read `RXBYTES`, `TXBYTES` and `MARCSTATE` until two reads agree (`README.md:99-101`).
- A frame with a bad CRC flushes the FIFO (`README.md:82`).

## Transmit [D]

`ccSendFrame`, `.ino:639-678`.

1. Send `SIDLE`, `SFRX`, `SFTX` in **one** SPI access (`.ino:648-649`). The flush has to happen before TX, because TX ends straight back in RX and the reply arrives about 192 µs later (`README.md:103-108`).
2. Write `[len][payload]` to the FIFO, up to 64 bytes. The radio appends the CRC.
3. Strobe `STX`.
4. For longer frames, top up the FIFO while checking `TXBYTES` for underflow (`.ino:656-675`).
5. Done when `MARCSTATE` has left TX and TX_END and `TXBYTES` is 0. It never shows IDLE, because `MCSM1` returns to RX (`.ino:611-625`).
6. On failure: `SFTX`, then `SIDLE`, `SFRX`, `SRX` (`.ino:575-579,631-635`).

## Answering an uplink [D]

`ccAutoAck`, `.ino:699-725`. Conditions: CRC OK, FCF `69 EC` or `79 EC`, destination is us. It answers **before** decrypting, so no AES is needed in the ACK path.

| Bytes | Content |
| :--- | :--- |
| 0-2 | `42 EE`, seq copied from the frame |
| 3-10 | Sender's 5 address bytes + `C5 1B 00` |
| 11-18 | Coordinator EUI-64 |
| 19-20 | `04 0D`, CSL IE |
| 21-22 | CSL phase, LE |
| 23-24 | `35 0C`, period 3125 |
| 25-26 | `80 3F`, termination IE |
| 27-28 | Inner CRC over bytes 0-26, LE |

Only the phase and CRC vary with time; the rest can be prepared in advance **[I]**.

If the inbound queue is full, send no ACK, so the device retransmits instead of losing data (`README.md:86-88`).

## Sending to a sleeping device [D]

State machine IDLE, RX, WAKE, ACK, FOLLOW (`README.md:63`, `.ino:1402-1473`).

1. **Schedule** on the device's next sample, using its stored anchor.
2. **Wake train:** repeat `25 <seq> CD AB <dst short> 82 0E <rendez LE> 80 3F` (`.ino:1357-1358`). `seq` is the seq of the Data frame that follows. `rendez` is time remaining in 320 µs units (`freedo/tado/mac.go:24`). Broadcast uses dst `FF FF`.
3. **End** on a wake frame with `rendez` 0, then send the Data frame immediately (`.ino:1366-1373`).
4. **ACK window:** check seq and addresses, then refresh the anchor from the ACK's GDO0 timestamp. Send no ACKs of our own in this window, because that would cover the target's ACK (`README.md:110-117`).
5. **Follow window:** optional, for a reply straight after the ACK.

The wiki says the wake seq increments per frame (`wiki/Tado-868-MHz-Link-Protocol.md:267`). Follow freedo, which matches its captures (`freedo/docs/PROTOCOL.md:1433`).

## Anchors [D]

`README.md:137-156`.

- Sampling period nominally 1 s.
- Guard starts at 40 ms each side of the sample and grows by 30 ppm of time since the last refresh, up to half a period.
- Short train about 79 ms when the anchor is fresh; full 1 s train when it is not known.
- 4 missed ACKs in a row drop the anchor.
- Use the GDO0 stamp only if it is later than the Data frame.
- No drift measurement is needed; device clocks measured -6 to +18 ppm.

## Listen before transmit [D]

The commercial bridge uses a random backoff of 0-6 × 640 µs and a 3.97 ms listen before the first frame of a job. Freedo leaves it out, because adding it cut answered jobs from 81.7% to 43.9%, cause unknown (`README.md:125-135`). **Decision (user, 2026-11-11): implement it as stock does**, then measure delivery on the prototype against freedo's finding.

## Porting to the F411 [I]

- Freedo splits radio and serial across two ESP32 cores. The F411 has one core, so radio work belongs in interrupts or a high-priority task.
- Radio and flash share SPI1. Flash access must never run during a train, ACK window or ACK transmission.
- Freedo's host-to-radio split goes away: the ring, queue and CCM all run on the F411.

## P2 receive-only implementation (#9) [I]

`src/radio.c` (task logic) and `src/cc110l.c` (register access, transmit guard). Run on the bench unit 2026-11-12 (bench RTT capture, 300 s, 2026-11-12, kept outside the repo); results below, **[V]** unless marked.

- RX on channel 26 = 868323668 Hz. Five 60 s windows: 285/127/226/242/239 frames, CRC OK 283/126/226/242/238, so **1115 of 1119 CRC OK (99.6%)**. Wake frames 188/62/150/149/149. Overflow 0, bad length 0, stale 0, rearm 0, reinit 0, SPI errors 0, status glitch 0.
- RSSI from about -41 to -72 dBm.
- Runtime: FreeRTOS heap free 25560 (min 25560) of 32768; unused stack words radio/net/hk 308/489/342 of 512.
- **`FREQEST` fix (#11, still open for the per-unit trim decision):** read after the packet it gave 0 or -1 on almost every frame (452 of 453), against a sender spread seen by the SDR of 5.1 kHz across devices and 8.1 kHz including the coordinator (about 3 and 5 units) (`test-bench.md`, "Stock network reference"). Cause **[I]**: with `MCSM1 = 3F` the chip returns RX_END and RX_RST to RX after every packet (`datasheets/cc110l.txt:4481-4482`), which re-arms RSSI (`cc110l.txt:2113-2115,2485-2488`); the datasheet does not say when `FREQEST` is held (`cc110l.txt:1695-1698,4429-4434`), so that it is reset too is inference, supported by the bench data. **Conflict:** freedo reads `FREQEST` at end of frame on a CC1101 and gets good values (`freedo/docs/PROTOCOL.md:1670-1685`; `freedo/firmware/cc1101_bridge/cc1101_bridge.ino:770-813`); the CC110L die differs in this. `FOC_BS_CS_GATE` is not the fix (`cc110l.txt:1684-1686`; `freedo/docs/PROTOCOL.md:1695-1697`); `FOCCFG` stays `0x16`, no register change.
- **Fix:** EXTI0 (GDO0, `IOCFG0 = 06`) triggers on both edges; the ISR counts edges and classifies by pin level (high = sync, low = end or discard). On sync the radio task, after the FIFO pump, takes stable reads of `FREQEST` (0xF2) and RSSI (0xF4) and keeps them only if GDO0 stayed high with no edge during the reads. The sample attaches to the next completed frame; it is dropped (counted "stale sample dropped") if more than 2 GDO0 edges passed since it was taken (its own end plus the next sync), and cleared on flush, re-init and the next sync. The sampled RSSI is cross-checked against the appended RSSI byte (counted only).
- **Bench result [V] (2026-11-12, channel 26, receive-only, OpenOCD flash verified, 300 s each):** comparison run with both read points logged: 1706 frames, 1700 CRC OK, in-packet sample on 1706, none 0, sync rssi differs 0; the after-packet read stayed 0 or -1 on every frame. In-packet `FREQEST` by frame control bytes: `49 e8` n=270 mean -1.03; `69 ec` n=97 mean -3.04; `42 ee` (Enhanced ACK) n=104 mean -3.50; `0b e8` n=13 mean -1.15; overall 0 to -6 units (about 8 kHz at 1586.9 Hz/unit), matching the 8.1 kHz spread the SDR measured across all senders, coordinator +5.2 kHz to devices +13.3 kHz (`test-bench.md:123`). Wake trains each -5..-4, mean about -4.8 (-7.6 kHz). Per-sender attribution is not possible because the log omits addresses. Final image (comparison code removed): 1602 frames, 1598 CRC OK (99.75%), freqest in packet 1602, none 0, stale sample dropped 0, sync rssi differs 0, all error counters 0, flash ID ok; wake trains -5..-4, mean -4.5..-4.8. Radio stack 282 words never used (308 before); heap free 25560. "sync rssi differs 0" supports that the RSSI frozen at sync equals the appended RSSI byte on this die.
- **Still open (#7, #11):** these are offsets of the stock senders relative to this bridge. The bridge's own crystal offset and the per-unit trim decision need our own transmissions (P5). Pins and SPI settings: `hardware.md`, "SPI1: radio and flash".

**Channel.** `RADIO_RX_CHANNEL` = 26 (868.3237 MHz), the stock network's channel, so frames from real devices arrive; receive-only there is allowed (`test-bench.md` rule 3). `RADIO_PROTECTED_CHANNEL` = 26 is defined now for the P5 transmit guard. `_Static_assert`s recompute base 863.125 MHz, spacing 199.951 kHz, channels 10 and 26, IF, data rate, bandwidth and deviation from the table with the datasheet formulas (`datasheets/cc110l.txt:3700-3711,3724-3736,3755-3778,3869-3906`).

**Register table.** The stock burst write `0x00..0x2E` (`radio.md`, "Stock CC110L register table"), as one 47-byte burst, with four changes:

| Reg | Stock | P2 | Why |
| :-- | :-- | :-- | :-- |
| `IOCFG2` | `04` RX overflow | `00` RX FIFO at or above threshold | Wake for long frames, as freedo (`freedo/cc1101/cc1101.go:47`) |
| `IOCFG0` | `01` threshold or end of packet | `06` sync word until end of packet | Rising edge = sync (FREQEST/RSSI sample), falling edge = end of every packet, CRC good or bad; also falls on RX FIFO overflow and length-filtered packets (`cc110l.txt:2916-2918`) |
| `CHANNR` | `00`, later `1A` | `1A` (`RADIO_RX_CHANNEL`) | |
| `FSCTRL1` | `18` | `06` | Owner decision (`radio.md`, Conflicts) |

`FIFOTHR = 03` keeps the stock 16-byte RX threshold, which leaves 48 bytes, about 7.7 ms at 160 µs per byte, before an overflow. `MCSM1 = 3F`: RXOFF_MODE and TXOFF_MODE both RX (`cc110l.txt:3929-3955`). `MCSM0 = 08`: no automatic calibration, so the firmware strobes `SCAL` once after loading the table and checks `FSCAL1 != 0x3F` for PLL lock (`cc110l.txt:2756-2759`). The table is read back after the burst and compared, except the four "Not used" addresses; any mismatch keeps the radio in IDLE and retries in 10 s.

`freedo` and `cc110l-vs-cc1101.md` prefer `IOCFG0 = 07` (CRC-OK packet) for the CSL end-of-frame timestamp. P2 uses `06` instead, so CRC failures are seen and counted; `06` also gives the end-of-frame edge for P5. Revisit in P5.

**Receive flow.** Interrupts only wake the radio task (`design.md`). The task blocks on a notification with a timeout (2 ms while a frame is being assembled, else 100 ms), so its watchdog check-in never stops. The pump follows freedo's `rxPump` (above): stable `RXBYTES` reads, length byte first, one byte left in the FIFO until the packet is complete (`cc110l.txt:2593-2597`), `FREQEST` and RSSI sampled in the packet right after sync (after the FIFO pump), not after it (#11). Overflow, a length of 0 or above `PKTLEN = 0x80`, or a frame stuck for 60 ms: `SIDLE`, wait for IDLE, `SFRX`, `SRX` (`cc110l.txt:1449-1451,1501`). **Deviation from freedo:** a bad CRC does not flush the FIFO. The chip still wrote exactly `1 + len + 2` bytes, so the FIFO stays aligned, and a flush would need `SIDLE`, which would cut a following packet that may already be arriving.

**Status bytes.** RSSI from the first appended byte, `RSSI_dBm = signed/2 - 74` at 868 MHz (`cc110l.txt:2130-2144`). `CRC_OK` is bit 7 of the second; bits 6:0 are reserved on the CC110L, where a CC1101 has LQI (`cc110l.txt:1747-1757`). The log prints those bits as `lqi(rsvd)` to see whether this die fills them. **[V]** (2026-11-12) they are filled: 43-48 on CRC-OK frames, so this die appears to fill an LQI-like value. `FREQEST` is printed raw and in Hz, at `26 MHz / 2^14 = 1586.9 Hz` per unit (`cc110l.txt:4429-4434`).

**Log.** One line per frame with tick time, length, CRC, RSSI, `lqi(rsvd)`, in-packet `FREQEST` and the first three MAC bytes (frame control and sequence number) only; no addresses or payload. Wake-up frames (frame type 5) are summarised per train (count, CRC OK, RSSI range, in-packet count, mean in tenths of Hz, min..max `FREQEST`). Every 60 s: `radio: 60 s: freqest in packet N, none N (stale sample dropped N), sync rssi differs N`; frames, CRC OK and bad, wake frames, overflows, bad lengths, stale frames, re-arms, re-inits, SPI errors, and the flash ID checks. The cloud LED toggles on each CRC-OK frame (P2 test wiring).

**Health check every 10 s.** `IOCFG0` must still read `06`; otherwise the radio lost its configuration (brown-out or reset) and is re-initialised. `MARCSTATE` IDLE is re-armed.

**Transmit guard (receive-only image).** `RADIO_TX_ALLOWED` is 0 and `#error`s if set. Every SPI header byte goes through one check in `src/cc110l.c`:
- Strobes: only `SRES`, `SCAL`, `SRX`, `SIDLE`, `SFRX`, `SNOP`. `STX`, `SFSTXON` (and `SXOFF`, `SPWD`, `SFTX`) call `fw_panic`. The check covers strobe headers with either R/W bit, since both execute the strobe (`cc110l.txt:1453-1456`). `radio_strobe()` is the only strobe function.
- No TX FIFO or `PATABLE` access, and no write outside `0x00-0x2E`, including a burst that would run past `0x2E`.
- Any write of `MCSM1` with RXOFF_MODE or TXOFF_MODE set to FSTXON or TX (`cc110l.txt:3929-3955`), and any `PKTCTRL0` other than FIFO mode, is refused.
- Every chip status byte is checked for STATE TX or FSTXON (`cc110l.txt:1381-1405`); the radio task confirms with `MARCSTATE` and, if the chip really is in a TX state, strobes `SIDLE` and stops the board (`fw_panic`, watchdog reset). The 10 s health check does the same.
The CC110L has no automatic acknowledgement and no wake-on-radio (`datasheets/cc110l-vs-cc1101.md`), so with `STX`, `SFSTXON` and the MCSM1 TX paths refused it has no way to transmit.

**Reset.** CSn low 10 µs, high 50 µs, then `SRES` with `CHIP_RDYn` (SO low) awaited before and after, bounded at 5 ms (`cc110l.txt:2398-2407`). SCLK idles low during this, where the datasheet's power-up sequence sets it high; the chip is already past its automatic power-on reset and `SRES` resets it fully either way [I], as in freedo (`.ino:582-596`). The GDO interrupts stay off from reset until the table is loaded, because `IOCFG0`'s reset value outputs a 135 kHz clock (`cc110l.txt:2894-2896`). `PARTNUM`/`VERSION` are logged; the datasheet gives `00`/`17`, the version "subject to change" (`cc110l.txt:4419-4426`).
