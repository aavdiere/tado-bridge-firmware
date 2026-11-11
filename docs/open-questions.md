# Open questions for the IB01 firmware

Master list of what is still required. Details and citations are in `hardware.md`, `server-api.md` and `radio.md`. `wiki/` means `Tado_Hardware_Teardown.wiki/`.

## Status (2026-11-11)

Enough is known to start building. What remains is gathered during the build:

1. ~~Backup session~~ (#8, #9): done 2026-11-11, see `hardware.md` (P0 results).
2. **Prototype** (#16, #17, #19, #28) and on-board measurements (#7, #26 noise).
3. **Before building self-update** (#20): check the bootloader and image sizes fit the layout.
4. **Optional:** stock capture (section C); pair button pin at firmware stage.

## Todo

- [x] Found the pair button pin over SWD (2026-11-11): PC15, active high.
- [ ] ~~Firmware stage: find the pair button pin.~~ Build a small diagnostic firmware that watches all free input pins and reports which one changes; the user presses the pair button and we read the result. Quicker alternative needing no new firmware: with the stock firmware running and powered from the ST-Link, read the input registers over SWD before and while the button is held: `mdw 0x40020010` (GPIOA_IDR), `mdw 0x40020410` (GPIOB_IDR), `mdw 0x40020810` (GPIOC_IDR) (`datasheets/rm0383.txt:1730-1732,8179-8180`). Read-only. Candidate pins: 7 (NRST), 2, 4, 12, 29, 30-33, 38, 39, 40, 42, 43. The button switches 3V3 through a diode, so expect a bit to go to 1.
- [ ] Make a full backup before any flash (user): `read.sh` twice, compare, and store the dumps privately. See "Full backup" in `hardware.md`. Needs an ST-Link V2 and access to the test points.
- [x] Probed the PCB (2026-11-11): ENC28J60 INT on PB2, Ethernet SPI wiring, RESET on PC14 and all four LEDs confirmed. See `hardware.md`.

## A. Answerable at the desk

| # | Missing | Why it matters | How to get it |
| :-- | :-- | :-- | :-- |
| 1 | Stock runtime SPI sequence, as a check on our design | Freedo's working flow is in `radio-runtime.md`, so this is no longer a blocker | Optional: new logic capture of a running bridge. The two `.sal` files are byte-identical and cover power-on only; reading them needs Saleae Logic (not installed) |

## B. Needs a physical IB01

| # | Missing | Why it matters | How to get it |
| :-- | :-- | :-- | :-- |
| 7 | Crystal offset; is per-unit trim needed | Stock uses `FSCTRL0=0`, freedo's module needed trim | Measure on air (`freedo/docs/PROTOCOL.md:1668+`) |
| 8 | Which values the new firmware needs from the backup. Nothing must survive on the chip once a full backup exists, but the firmware still needs the serial and EUI-64 **[I]** | tanoclo identifies the bridge by serial | Read them from the backup (`tanoclo/patch_ib_firmware/extract_rf_key.js` documents the offsets) and provision them with the new firmware |
| 9 | Option bytes and read protection on this unit; confirm the restore path works | The guide reads shipped units over SWD, but the docs do not state the protection level | Read the option bytes during backup; see "Full backup" in `hardware.md` |

## C. Needs a stock bridge observed on the wire

Single method: run a stock bridge through tanoclo proxy mode with logging (`tanoclo/ws-server/lib/proxy-manager.js:204,214-215`). Needs a working bridge and a Tado account connection.

| # | Missing |
| :-- | :-- |
| 10 | Meaning of uplink `field_a/b/c`, and how stock picks them |
| 11 | Stock use of `auth/key`, how `0x0261` is used, what credential is sent |
| 12 | Option 2048 requirement; Option 7 `0xFFFF` meaning |
| 13 | Exact `found` URI; `d/rfkey` reply format; `d/reboot`, `d/identify`, `d/info` responses |
| 14 | Keepalive, reconnect and backoff timing; RF retries; message ID preservation |

Because tanoclo is the compatibility target and accepts zeros for #10, the firmware can be built before #10 and #11 are answered, but it would be matching tanoclo rather than stock.

## D. Design risks needing a prototype

| # | Risk | Note |
| :-- | :-- | :-- |
| 16 | Enhanced ACK turnaround over SPI; gap may be under 1 ms | Eased: the ACK is unencrypted and mostly static, so it can be prebuilt (`wiki/Tado-868-MHz-Link-Protocol.md:346-348`), and the radio's RX-to-TX switch is only about 30 µs (`datasheets/cc110l-vs-cc1101.md`) |
| 17 | RAM budget for TLS, WebSocket, IP and radio in 128 KB | Estimated 71 KB in `design.md`; to be measured. First runtime numbers from P1 (2026-11-11, **[V]**, early data point only): FreeRTOS heap 25632 of 32768 bytes free, unused task stack 489/489/352 of 512 words (radio/net/hk); no TLS, lwIP or radio buffers yet |
| 18 | Stack choice | **Approved (user, 2026-11-11):** FreeRTOS, lwIP, mbedTLS, own 6LoWPAN and MAC ported from freedo. See `design.md` |
| 19 | Radio deadlines versus Ethernet and flash work | Task design in `design.md`; to be proven on a prototype. Ethernet is on SPI2, so only flash shares the radio's bus |
| 20 | Bridge self-update and rollback; first flash path | **Approved (user, 2026-11-11) if the firmware fits:** check bootloader and image sizes against the layout before building. Images are unsigned. Open: how the uploader reaches tanoclo |
| 26 | Entropy source for the network key and TLS | Decision: ADC on unconnected pin PA3, conditioned as in `design.md`. Still needs a measurement of its readings on the board |
| 27 | Where the network key and frame counter are stored, with wear limits | **Approved (user, 2026-11-11)**, layout in `design.md` |
| 28 | Pairing beacon answer on real hardware | Must leave the bridge within 2.4-7.4 ms; freedo's on-chip path is "not validated on air" (`freedo/docs/PROTOCOL.md:732`) |

## E. Not reviewed or undocumented

| # | Missing | Pointer |
| :-- | :-- | :-- |
| 23 | Device firmware serving over `d/fw/rq` | FID tables are placeholders (`tanoclo/docs/coap_tlv.md:283-319`), no tanoclo handler. Can be deferred |

## Closed

| # | Was | Answer |
| :-- | :-- | :-- |
| 1 (part) | Stock CC110L register table and clock setup | Full table in `radio.md`, from `wiki/CC110-Register-Configuration.md`. MCU clock tree chosen: 96 MHz from the 26 MHz crystal, `hardware.md` Clock tree (#7) |
| 3 | FIFO strategy for frames over 64 bytes | GDO0 threshold interrupt at 16 bytes; see `radio.md` |
| 4 | CC110L pin clues | Pin table in `hardware.md` |
| 15 (part) | Docs versus code conflicts | Code wins; see `server-api.md` |
| n/a | Is mesh routing required? | No: single-hop star |
| 6 | MCU pin map | Complete: Ethernet, LEDs, factory reset PB10 (active low), pair button PC15 (active high). See `hardware.md` |
| 2 | Does freedo's CC1101 use apply to the CC110L? | Yes; see `datasheets/cc110l-vs-cc1101.md` |
| 29 | `FSCTRL1` value | Decision: use `0x06` (152 kHz IF), as freedo does. The wiki's capture shows `0x18` |
| 5 | Radio conflicts between sources | Resolved in the conflict table in `radio.md`. Retry gap is provisional. All still to be proven on the real IB01 |
| 25 | TX power, duty cycle, listen before transmit | Match stock: default PATABLE, 1 s wake trains (seen by freedo and in the user's RF measurements). **Decision (user, 2026-11-11): keep listen-before-transmit** as stock does it. Measure delivery on the prototype |
| 30 | Maximum number of devices | Decision: 16 for now |
| 21 | Radio security and pairing requirements | Documented; see "Link security and pairing" in `radio.md`. Follow-ups are #26-28 |
| 22 | Must existing devices stay paired? | No. Decision: they are re-paired, so the old network key need not be carried over |
| 24 | Does device pairing need the MFI337S3959? | No (user decision). Follow freedo and tanoclo; the wiki's HomeKit Accessory Protocol statement is not followed |
