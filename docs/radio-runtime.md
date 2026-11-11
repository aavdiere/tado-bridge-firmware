# Radio runtime behaviour

How to drive the CC110L after init. Source is freedo's working ESP32 + CC1101 firmware: `freedo/firmware/cc1101_bridge/cc1101_bridge.ino` (`.ino` below) and its `README.md`. Validated on air by freedo on a CC1101, not on an IB01.

The stock SPI sequence after init has not been decoded. See `open-questions.md` #1.

Status tags: **[D]** stated in code or docs, **[I]** our own inference.

## Reset and init [D]

1. Manual power-on reset, then `SRES` (`.ino:582-596`).
2. Drive both GDO pins low and read them back to confirm the chip accepted writes (`.ino:599-603`). After reset `IOCFG0` outputs a 135 kHz clock that would flood the interrupt (`.ino:143-146`).
3. Load the register table (`radio.md`), with `FSCTRL1=0x06`, then calibrate and enter RX.

## Receive [D]

- **Interrupts:** GDO0 stamps end of frame with a timestamp, GDO2 signals RX FIFO threshold. A periodic tick (8 ms in freedo) also pumps the FIFO, because an overflow raises no interrupt. The FIFO overflows about 11 ms after a frame starts (`README.md:71-77`).
- **Reading the FIFO** (`rxPump`, `.ino:772-816`): read the length byte first, then expect `1 + len + 2` bytes in total. While still receiving, **leave one byte in the FIFO**. Read everything only when the rest of the frame is there.
- **Overflow or bad length:** flush and re-arm.
- **Status bytes:** the last appended byte holds `CRC_OK` in bit 7 (`.ino:700`).
- **`FREQEST`:** read when the frame completes, before any strobe or TX (`README.md:49-53`).
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
