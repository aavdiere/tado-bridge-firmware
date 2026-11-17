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

P5 changes parts of this section: the channel, `IOCFG2`, the receive loop's timing and the transmit guard. See "P5 link implementation" at the end; everything else here still holds.

`src/radio.c` (task logic) and `src/cc110l.c` (register access, transmit guard). Run on the bench unit 2026-11-12 (bench RTT capture, 300 s, 2026-11-12, kept outside the repo); results below, **[V]** unless marked.

- RX on channel 26 = 868323668 Hz. **Earlier image (first P2 run, `FREQEST` read after the packet, before in-packet sampling; the final-image run is in the bench result below).** Five 60 s windows: 285/127/226/242/239 frames, CRC OK 283/126/226/242/238, so **1115 of 1119 CRC OK (99.6%)**. Wake frames 188/62/150/149/149. Overflow 0, bad length 0, stale 0, rearm 0, reinit 0, SPI errors 0, status glitch 0.
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

**Log.** One line per frame with tick time, length, CRC, RSSI, `lqi(rsvd)`, in-packet `FREQEST` and the first three MAC bytes (frame control and sequence number) only; no addresses or payload. Wake-up frames (frame type 5) are summarised per train (count, CRC OK, RSSI range, in-packet count, mean in tenths of Hz, min..max `FREQEST`). Every 60 s: `radio: 60 s: freqest in packet N, none N (stale sample dropped N), sync rssi differs N`; frames, CRC OK and bad, wake frames, overflows, bad lengths, stale frames, re-arms, re-inits, SPI errors, and the flash ID checks. The cloud LED toggled on each CRC-OK frame (P2 test wiring); since P4 (#15) it shows the server session instead.

**Health check every 10 s.** `IOCFG0` must still read `06`; otherwise the radio lost its configuration (brown-out or reset) and is re-initialised. `MARCSTATE` IDLE is re-armed.

**Transmit guard (receive-only image).** `RADIO_TX_ALLOWED` is 0 and `#error`s if set. Every SPI header byte goes through one check in `src/cc110l.c`:
- Strobes: only `SRES`, `SCAL`, `SRX`, `SIDLE`, `SFRX`, `SNOP`. `STX`, `SFSTXON` (and `SXOFF`, `SPWD`, `SFTX`) call `fw_panic`. The check covers strobe headers with either R/W bit, since both execute the strobe (`cc110l.txt:1453-1456`). `radio_strobe()` is the only strobe function.
- No TX FIFO or `PATABLE` access, and no write outside `0x00-0x2E`, including a burst that would run past `0x2E`.
- Any write of `MCSM1` with RXOFF_MODE or TXOFF_MODE set to FSTXON or TX (`cc110l.txt:3929-3955`), and any `PKTCTRL0` other than FIFO mode with variable length (since 2026-11-17 also refused: fixed, infinite and the reserved length mode, `datasheets/cc110l.txt:3673-3684`), is refused.
- Every chip status byte is checked for STATE TX or FSTXON (`cc110l.txt:1381-1405`); the radio task confirms with `MARCSTATE` and, if the chip really is in a TX state, strobes `SIDLE` and stops the board (`fw_panic`, watchdog reset). The 10 s health check does the same.
The CC110L has no automatic acknowledgement and no wake-on-radio (`datasheets/cc110l-vs-cc1101.md`), so with `STX`, `SFSTXON` and the MCSM1 TX paths refused it has no way to transmit.

**Reset.** CSn low 10 µs, high 50 µs, then `SRES` with `CHIP_RDYn` (SO low) awaited before and after, bounded at 5 ms (`cc110l.txt:2398-2407`). SCLK idles low during this, where the datasheet's power-up sequence sets it high; the chip is already past its automatic power-on reset and `SRES` resets it fully either way [I], as in freedo (`.ino:582-596`). The GDO interrupts stay off from reset until the table is loaded, because `IOCFG0`'s reset value outputs a 135 kHz clock (`cc110l.txt:2894-2896`). `PARTNUM`/`VERSION` are logged; the datasheet gives `00`/`17`, the version "subject to change" (`cc110l.txt:4419-4426`).

## P5 link implementation (#17) [I]

Host-tested, built; the TX-off image was reviewed (`fw-reviewer`, "safe to flash"), flashed and verified (2026-11-17); the first test frames went out with a temporary sender, since reverted (`test-bench.md`, "First transmit test" [V]). The committed image cannot transmit: `RADIO_TX_ALLOWED` is 0 (`src/cc110l.h`), and no network key is ever set (`link_set_key()` has no caller until #37 and #19), so no ACK is prepared and nothing is queued for the air. Since #37 the key is set at boot from a valid network record in flash (`design.md`, "Implementation: network and counter records (#37)"); no network is created until the owner opens the first pairing window (#19, "Pairing (#19)" below), so on the bench there is still no key.

**Modules.** Hardware-independent and tested on the host: `src/mac.c`, `src/ccm.c`, `src/lowpan.c`, `src/txgate.c`, `src/csl.c`, `src/relay.c`, `src/msgq.c`, `src/link.c`. Hardware side: `src/radio.c` (task, interrupts, the coordinator's `csl_port_*` functions) and `src/cc110l.c` (the only code that can key the transmitter). Net task side: `src/session.c` moves frames between the server and the two static 4 KB queues of `src/link.c`.

**Changes to the P2 radio setup.**

| Item | P2 | P5 | Why |
| :--- | :--- | :--- | :--- |
| Channel | 26, receive-only | 10 (`RADIO_CHANNEL`) | The test network's channel (`test-bench.md`); the bridge no longer listens on the stock channel |
| `IOCFG2` | `00` RX FIFO threshold | `07` packet received with CRC OK (`datasheets/cc110l.txt:2919`) | Its rising edge is the end of a good frame: the interrupt that sends the Enhanced ACK |
| FIFO pump | On the threshold interrupt, else every 2 or 100 ms | Every tick (1 ms) from the sync edge to the end of the frame, else every 100 ms or sooner if the coordinator needs it | No threshold interrupt any more; 1 ms is 6 bytes of the 64-byte FIFO |
| Work that can wait (flash ID, health check, minute log) | Any time | Only while no packet is arriving and no transaction holds the air | Nothing may sit between a frame and its ACK |

**Enhanced ACK path.** Differs from the plan in `radio.md`, "ACK prebuild", in two points: the ACK bytes go into the TX FIFO in the interrupt, not during reception, and `STX` is strobed from IDLE, not from RX. From RX, `STX` only works if the channel is clear (`MCSM1` CCA_MODE 3, `datasheets/cc110l.txt:2253-2262`), which right after a packet it may not be; from IDLE no assessment applies.

1. Task, while the Data frame arrives: once its 16 header bytes are out of the FIFO, `ack_consider()` (`src/radio.c`) builds the ACK. Conditions: identity and key set, the frame is a unicast Data frame addressed to our EUI-64, the coordinator is not in a train or ACK window, the uplink queue has room for the largest message, the chip's frequency registers read back as written, the GDO interrupts are on.
2. Hand-over, with the GDO interrupts masked: `cc110l_ack_prepare()` runs the transmit gate (`src/txgate.c`) on the ACK and the header it answers, and arms it only if no GDO0 edge has come since the frame was seen arriving and GDO0 is still high. So the next CRC-OK edge is that frame's.
3. Interrupt on the CRC-OK edge (`exti1_isr`, `cc110l_ack_fire_from_isr()`): `SIDLE`, `SFTX`, the 30 bytes, then `STX` in a second access. A new sync edge or the end of frame handling drops an ACK that was not sent.

Latency, estimate only, nothing measured yet: interrupt entry a few µs; about 36 bytes at 6 MHz, 60-80 µs to the `STX`; IDLE to TX 75 µs (`datasheets/cc110l.txt`, Table 5-11). The preamble should start **0.15-0.3 ms** after the CRC-OK edge, against the 0-1 ms target and the 0.35-0.39 ms of stock devices (`test-bench.md`). The interrupt can be held off by one SPI1 access of the radio task, at most a 64-byte FIFO read, about 0.1 ms; the net task's P-256 steps do not mask it. To be measured with `tools/sdr/demod.py` (#18).

**Transmit guard, from the bottom up.** Rules in `src/txgate.h`.

- `guard_header()` in `src/cc110l.c` refuses `STX`, `SFTX` and the TX FIFO except inside the transmit functions, and `SFSTXON`, `SXOFF`, `SPWD` and the `MCSM1`/`PKTCTRL0` values that transmit by themselves always (as in P2).
- **Output power.** `PATABLE` is written only by `cc110l_set_pa()`, one byte to `PATABLE[0]`, never a burst (`PATABLE[1]`), and `FREND0` must keep `PA_POWER = 0` (`datasheets/cc110l.txt:2799-2805`). It accepts only the reset value `0xC6` (9.6 dBm wire-wound, 8.5 dBm multi-layer inductors, `cc110l.txt:2834-2839,2868-2873`) and the 868 MHz settings documented at 0 dBm and below (`cc110l.txt:2816-2832,2852-2865`), read back after the write. The PA code is not monotonic: `0xC0`-`0xC5` are numerically below `0xC6` but give 10-12 dBm, so a numeric "at most 0xC6" would let them through; the check is a list. Bring-up level `RADIO_PA_BRINGUP = 0x26`, -10 dBm with wire-wound inductors (`cc110l.txt:2828`; -10 dBm is `0x27` with multi-layer ones, `:2862`; the IB01's variant is not known). Stock keeps `0xC6` (`radio.md:53`). Nothing is sent until the setting has read back since the last reset, and it is read back again before every frame and ACK and in every health check. The receive-only image now writes `PATABLE` too (a lower power, no transmission).
- `STX` is strobed in two places only: `cc110l_tx_start()` (task) and `cc110l_ack_fire_from_isr()` (interrupt). Both need `RADIO_TX_ALLOWED`, a full register table written since the last reset, and the gate's verdict on the carrier those registers give: at least 999 kHz from channel 26, on the channel plan, channel 10 only.
- The chip's own frequency registers are read back and compared before every frame the task sends, and before every ACK is prepared; the interrupt refuses an ACK whose read-back is older than 50 ms. A chip that reset holds 800 MHz, not the table.
- Frame rules: an Enhanced ACK only as the byte-for-byte answer to a unicast Data frame addressed to our EUI-64; other frames only with our source address and of a class a coordinator sends; a beacon only flagged as pairing, on the test channel, as the answer to a Beacon Request.
- One transmission at a time: a new frame is refused while one of ours is on the air, and the interrupt sends no ACK then.
- **Airtime (duty cycle).** Channel 10 lies in the 865-868 MHz band: 25 mW e.r.p., at most 1 % duty cycle (36 s per hour) **[I]**: ERC Recommendation 70-03, Annex 1, band h1.4, recalled, not checked against a copy in this repository; our listen-before-transmit is not the regulated LBT+AFA alternative. `src/airtime.c` keeps a rolling window of 61 one-minute buckets (at least the last 60 minutes) with a budget of **30 s per hour**. The driver charges every frame before it is keyed: `cc110l_tx_start()` (wake-up and Data frames, Router Advertisements, everything the coordinator sends) and `cc110l_ack_prepare()` (an Enhanced ACK, charged when prepared, so the interrupt never touches the budget). A frame that does not fit is refused (`CC_TX_DUTY`), never queued; the coordinator fails that attempt and drops the datagram after its 3 attempts. Charge per frame: (11 + length) × 161 µs + 100 µs, above the real 160.03 µs per byte plus 75 µs IDLE to TX. Logged every minute: airtime of the last hour, frames and ACKs refused for airtime. **Reset (owner decision, 2026-11-17):** the window starts empty at every boot; no record is kept over a reset, and this is not a use case to cover. A reset loop that transmits after each boot could therefore exceed the limit.
- `src/radio.c` sends nothing while the GDO interrupts are off (storm guard or failed bring-up), and the TX-state watch of P2 now stops the board only for a TX state outside the airtime of a frame we started.

**Host tests of the driver.** `tests/host/test_cc110l.c` compiles `src/cc110l.c` with the switch set for that one translation unit and runs it against a simulated chip (`tests/host/cc110l_model.h`) that logs every `STX` with the carrier its own registers give. `tests/host/test_cc110l_off.c` runs the driver as built. `make test`: 1475660 checks in 9 programs (with `tests/host/test_airtime.c`), 0 failed (2026-11-17, after the review fixes) **[V]** on the host.

**Open, for the review and the bench.**

- **Status bytes against the early `SIDLE` [I].** The interrupt strobes `SIDLE` some 10-20 µs after the CRC-OK edge. If the chip appends the two status bytes later than that, the frame never completes in the FIFO and is dropped as stale after 60 ms although it was acknowledged. The datasheet does not say. On the bench: `stale` must stay 0 in the minute log while `acks sent` counts.
- **`GDO2` after a flush [I].** `IOCFG2 = 07` de-asserts "when the first byte is read from the RX FIFO" (`datasheets/cc110l.txt:2919`). Whether `SFRX` also de-asserts it is not stated; if not, the first good frame after a flush gets no ACK, and the device repeats it.
- **Listen before transmit: sync words and RSSI (after the first review).** Busy if a GDO0 edge comes during the 3.968 ms listen, or the RSSI is at or above -85 dBm at its start, at its end, or at the radio task's loop pass in between (about one) **[I]**. The RSSI register updates continuously in RX until a sync word freezes it (`datasheets/cc110l.txt:2113-2115`). The threshold is about 19 dB above the sensitivity at 868 MHz, 38.4 kBaud (-104 dBm, `cc110l.txt:600-603`) and below the -41 to -72 dBm of the frames seen in P2; the minute log prints the listens found busy by RSSI and the highest RSSI of the minute's listens, to set it from bench data. What the stock coordinator listens for is not known (#20).
- **Calibration and PLL lock (#11, after the first review).** `MCSM0 = 08` stays (no automatic calibration: it would add 0.8 ms to IDLE to TX, `datasheets/cc110l.txt` Table 5-11, which an Enhanced ACK cannot wait for). `cc110l_calibrate()` in `src/cc110l.c` is the only path that grants the lock: from IDLE, up to 3 × `SCAL`, wait for IDLE, `FSCAL1 != 0x3F` (`cc110l.txt:2756-2761`), then the carrier registers are read back; the radio task then compares registers `0x00-0x18` as the health check does. Three tries without lock latch the transmitter off until the next reset of the chip (the next bring-up); the receiver keeps running. Every `SCAL` strobe, configuration write and reset clears the lock. Policy **[I]**: the datasheet asks for calibration "regularly" with no interval (`cc110l.txt:2740-2752`), so the task recalibrates every 5 minutes at a quiet moment (`RECAL_PERIOD_MS` in `src/radio.c`, about 1 ms deaf receiver), and a calibration older than 12 minutes (`CC_CAL_MAX_AGE_US`) stops the transmitter. Whether 5 minutes is needed, or enough, is to be seen on the bench: `FSCAL3-1` are logged at every calibration.
- **Radio task stack:** 640 words. Deepest paths by `-fstack-usage` are about 1.0-1.2 KB (a log line from the coordinator) **[I]**; measure the unused words on the bench.
- **ACK interrupt failures (after the first review).** If the second access (the `STX`) cannot be opened after the first access made the chip IDLE, the interrupt opens a third: `SIDLE`, `SFTX`, `SRX`, keeping the RX FIFO, and has the radio task confirm RX with `MARCSTATE` (a status byte cannot, since IDLE is also reported for transitional states, `datasheets/cc110l.txt:1381-1385`). If that access fails too, or the chip is not in RX, the task aborts and re-arms at once (`cc110l_tx_abort()`, then `SIDLE`, `SFRX`, `SRX`), and re-initialises if that fails; the frame is dropped and its device repeats it. Counted as "acks failed" and "re-armed" in the minute log. A status byte that wrongly shows a transmit state at the `SIDLE` costs only that ACK: the interrupt strobes `SRX` and the frame is still taken.
- **Own-frame window (fixed after the first review).** `cc110l_tx_expected()` compared an end time as a signed difference and cleared its flag only when called; in a quiet network nothing calls it after the last ACK, and from 35.8 to 71.6 minutes later it read "on the air" again, so the TX-state watch would have taken a stray TX state for ours. It now keeps start and length and compares an unsigned elapsed time; the task calls it every loop pass; and the task clears the flag only while no ACK is prepared, so the interrupt cannot set it between the task's check and its store.

## Pairing (#19) [I]

Built and host-tested 2026-11-17; not reviewed, not flashed, nothing measured on air. The committed image cannot pair: `RADIO_TX_ALLOWED` is 0 (`src/cc110l.h:24`), and without a target named at build time the pair button opens no window. Flow and frames follow freedo (`radio.md`, "Link security and pairing"; `freedo/docs/PROTOCOL.md:623-739`).

**Modules.** `src/pair.c` (hardware independent, ported from freedo `tado/beacon.go`, `BuildPairCoAP` in `tado/build_resources.go:453-485`, `tado/pair_window.go` and `pairAnswer` in `firmware/cc1101_bridge/cc1101_bridge.ino:923-974`, Apache-2.0 notice in the file): the window, the Beacon Request check, the Enhanced Beacon, the `d/pair` message, the limits. `src/link.c` runs it in the radio task; `src/radio.c` owns the trigger and the beacon's timing; `src/network.c` creates the network or reads its key back.

**Trigger and arming.** A window opens only when all of these hold (`pairing_trigger()`, `src/radio.c:1671`):

| Gate | Where |
| :--- | :--- |
| The owner holds the pair button (PC15) for 1 s; holding it again closes an open window | `src/app.c:73-83`, `NOTE_PAIR` |
| A target device is named at build time: `PAIR_TARGET_EUI64` in the gitignored `config/local.cmake` (EUI-64 in the order of `BRIDGE_EUI64`, `00 1b c5` first; CMake refuses another prefix or the bridge's own EUI-64) | `cmake/CMakeLists.txt:57-77`, `config/local.cmake.example` |
| The transmitter is built in (`RADIO_TX_ALLOWED` 1, set only in a local bench image after a `fw-reviewer` verdict) | `src/cc110l.h:24` |
| The flash was read at boot (`flash_boot()`), the bridge has an identity, the GDO interrupts are on and the PLL locked | `src/radio.c:1671-1697` |
| A network: the active one, its key read back from its record by seq (`network_key()`, `src/network.c:87`); or, only if the boot found no network record at all, a new one created **before** the window opens (`network_create()`, which now refuses unless `network_absent()`, `src/network.c:32,98`; any failure blocks a second attempt until the next boot) | fw-reviewer items 1-3 (issue #19) |

The trigger runs at a quiet moment of the radio task (no packet arriving, no coordinator transaction), never while a window is open: creating a network blocks the task for up to about 0.8 s (DRBG reseed, sector erases), so it cannot overlap a window or a transaction. The key and random bytes the trigger used are wiped right after the window opens (item 4).

"No network record at all" (`store_net_probe()`, `src/store.c:212`): neither slot starts with the record's magic. The magic is the commit mark, programmed last (`design.md`, "Slotted record, as built"), so a first write cut by a power failure leaves none, and a damaged or refused record (for example one for channel 26) keeps its magic: the bridge then refuses to create a network over it, because devices may still be paired to it. Host test: the probe after a cut at every step of a first write (`tests/host/test_store.c`).

**The window** (`src/pair.h`). 120 s, closed earlier by the target's 2.04, by the button, by a new key (`link_set_key()`) or by no key. While open it holds the pairing key's schedule and a copy of the network key; closing wipes both. Per window at most 16 beacons, 3 Router Advertisement pairs and 3 `d/pair` datagrams (each with the coordinator's 3 attempts). Only the target is answered: a Beacon Request from any other address is dropped before it is decrypted; a Data frame or broadcast that fails the network key is tried under the pairing key only if its source is the target (`src/link.c:163-232`).

**Exchange.**
1. Window opens: the first Router Advertisement pair is due 1 s later, before any frame of the joiner, as on the commercial bridge (`freedo/docs/PROTOCOL.md:673-675`).
2. The target's Enhanced Beacon Request (27 bytes, sealed under the pairing key, MAC command `07`) gets an Enhanced Beacon (32 bytes) with the bridge's next seq and frame counter (`csl_take_seq_counter()`, `src/csl.c:339`, which also keeps a busy transaction from handing that counter out again), built while the request is taken (`on_beacon_request()`, `src/link.c:241`). Host test: freedo's request and beacon byte for byte (`tests/host/test_pair.c`, vectors from `freedo/tado/beacon_test.go:62,90`).
3. Every Router Advertisement of the window goes twice: under the network key with Frame Pending (`59 e8`), then under the pairing key (`49 e8`), consecutive seq and counter, each behind its own 1 s train (`src/link.c:313`, `CSL_JOB_PENDING`; `freedo/docs/PROTOCOL.md:667-672`). The target's Router Solicitation under the pairing key asks for one (10 s hold-off between advertisements as before).
4. `d/pair` goes `PAIR_DPAIR_DELAY_MS` (3 s) after the target's first pairing-key frame that is not a Beacon Request (its Router Solicitation comes 1.5-2.8 s after the beacon, `freedo/docs/PROTOCOL.md:664-665`), so that the advertisement pair it asked for goes first; 12 s after the first beacon if nothing else comes. It is a Data frame under the pairing key from UDP 5683 to 5683: `40 02 mid | b1 'd' | 04 'pair' | 11 2a | e8 06 e7 session(8) | ff 12 10 key(16)`, 43 bytes, MID from `0x4000-0x5FFF` (below the server's `0x7000-0xFFFF`, `tanoclo/ws-server/lib/coap-transport.js:39-44`), a new 8-byte session value per datagram from the DRBG. Host test: freedo's bytes (`freedo/tado/build_resources_test.go:196-203`). If its attempts fail, the target's next pairing-key frame brings another, up to 3.
5. The target's 2.04 ACK to that MID under the network key closes the window and is not relayed to the server (`src/link.c:189`); from then on its frames go to the server as any device's.

**Key exposure.** The network key crosses the air under the public pairing key (`radio.md`, "Exposure"). In RAM it is held, besides the link's AES schedule, only while needed: the window's copy (wiped on close), the `d/pair` job body and the frame sealed from it (wiped when the job ends or the pool is reset, `src/csl.c`), the driver's frame copy (wiped once the frame has left the chip or was abandoned, `tx_wipe()`, `src/cc110l.c:690`), and the builder's stack buffers (wiped after queuing). A `d/pair` already on the air when the window closes is not recalled: its transaction ends normally, at most about 1 s later.

**Transmit gate.** The beacon rule compared all 8 bytes of our address at offset 13, but a beacon is sealed from byte 16 on (`freedo/tado/beacon.go:17-23`), so every real beacon was refused. It now checks the 3 clear bytes, PAN `cd ab`, that the answered request went to `ff ff` / `ff ff`, that the beacon's destination is the requester and not us, the pairing flag and channel 10 (`src/txgate.c:149`). The driver passes the request's header to the gate through `cc110l_tx_answer()` (`src/cc110l.c:782`); `cc110l_tx_start()` has none, so a beacon cannot go out on the frame path.

### Beacon answer timing (open question 28)

Target: the beacon's preamble starts 2.4-7.4 ms after the request ends (`freedo/docs/PROTOCOL.md:657-658`). Path (`beacon_answer()`, `src/radio.c:1600`):

| Step | Time after the request's end | Source |
| :--- | :--- | :--- |
| GDO0 falls at the end of the request; EXTI0 stamps `gdo0_fall_us` | 0, later by at most one SPI1 critical section (about 0.1 ms) | `src/radio.c` interrupts |
| Radio task (highest priority) woken by the edge: last FIFO bytes, `link_on_frame()`: CCM* check of the request (6 AES blocks), beacon built and sealed (6 AES blocks) | well under 0.5 ms **[I]** | |
| Check: exactly one GDO0 edge since the request's length byte was read (`rx.edges_at_len`, `src/radio.c:933`), so the stamp is the request's own end | | `src/radio.c:796` |
| Spin to `PAIR_BEACON_TX_AT_US` = 3.7 ms; any GDO0 edge, GDO0 high, a frame of ours on the air or a coordinator transaction makes it busy: no beacon; RSSI at or above -85 dBm at the end: busy | 3.7 ms | `src/pair.h:88` |
| Later than `PAIR_BEACON_TX_LATEST_US` = 6.5 ms (task blocked, e.g. a 10 ms flash program): no beacon | | `src/pair.h:89` |
| `cc110l_tx_answer()`: RSSI read, register read-back, gate, airtime, SIDLE/SFRX/SFTX, 33 FIFO bytes at 6 MHz, STX | +0.1-0.15 ms **[I]** | |
| IDLE to TX, no calibration | +75.2 µs | `datasheets/cc110l.txt:2542` |
| **Preamble starts** | **about 3.9-4.0 ms** | |

Margins: 1.5 ms after the earliest and 3.4 ms before the latest start the device accepts; the commercial bridge listens 3.968 ms before its beacon (`freedo/firmware/cc1101_bridge/cc1101_bridge.ino:228-230`). A beacon not sent (end not seen live, busy, late, refused) costs one request: the device asks again 8 s later (`freedo/docs/PROTOCOL.md:662`). Nothing is logged before the beacon is on the air; then `pair: beacon seq NN to the target sent: STX N us after the request ended`. The minute log counts beacons sent and not sent by reason.

Effect on the Enhanced ACK (#18): none on its path. The spin checks for a sync edge every 100 µs and gives up at once, so a Data frame arriving meanwhile is pumped and its ACK prepared as before; during the beacon itself the radio is in TX.

**Measuring it.**
1. SDR, primary (`rf-bench`): `tools/sdr/demod.py --channels 10 --summary` reports the request-to-beacon gap (`test-bench.md`, "Demodulator"); expect 3.8-4.1 ms, accept 2.4-7.4 ms. With `hopcap.py`, nothing on channel 26.
2. RTT: the `pair: beacon ... STX N us` line; expect about 3.8 ms. It uses the bridge's own clock and the GDO0 stamp, so it misses the interrupt latency and the 75 µs to the air.
3. Scope, fallback: PB8 (pairing LED, on while the window is open) falls right after the beacon's STX strobe (`send_frame()`, `src/radio.c:1420`). A second probe on GDO0 (CC110L pin, MCU PA0): it falls at the end of the request and, in TX, rises when the beacon's sync word has gone out (`IOCFG0 = 06`). Gap request end to STX = PB8 fall minus GDO0 fall; preamble start = PB8 fall + 75 µs; cross-check: the beacon's GDO0 rise comes 75 µs + 8 bytes x 160 µs = about 1.36 ms after PB8 falls.

**Airtime.** A window costs up to about 2 s per advertisement pair and 1 s per `d/pair` attempt without an anchor: at most about 15 s with every limit reached, half the 30 s per hour budget (`src/airtime.h`). A second window in the same hour can run into the budget; refused frames are counted in the minute log (`tx_duty`).

**Sizes** (2026-11-17, `make size`): committed build (transmitter off, no target) text 150964, bss 80412 bytes, against 147348 and 79864 before #19. Bench variant with a dummy target and `RADIO_TX_ALLOWED` 1 (built outside the tree, not committed): text 157236, bss 80432. Host tests: `make test` 3116202 checks in 11 programs, 0 failed **[V]** on the host.

**Open, for the review and the bench.**
- **Not measured:** the beacon's timing on air and freedo's own "not validated on air" for the firmware responder (`freedo/docs/PROTOCOL.md:732`); whether the spare device takes the beacon, the advertisement pair and `d/pair` from this bridge.
- **`d/pair` order [I]:** the delay after the target's Router Solicitation stands in for freedo's operator, who sends `d/pair` by hand after seeing the joiner. The commercial bridge sent its `d/pair` on a 21-frame train on the joiner's anchor (`freedo/docs/PROTOCOL.md:688-690`); ours uses the 1 s train unless the joiner's Enhanced ACK gave an anchor.
- **No server trigger:** tanoclo opens pairing with `POST d/I/IB0000FT0100<digits>/pair` (`server-api.md`, "Bridge handles itself"); the bridge does not handle it, and does not report the joiner to the server (`...found`). Only the button opens a window. Whether the server may open one is an owner decision.
- **tanoclo and the new device:** its `auth/token` and reports are relayed; whether tanoclo accepts a device it has no row for is untested.
