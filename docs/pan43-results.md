# Own PAN ID (#43): results

Bench tests of 2026-11-23, channel 16 only (nothing was sent on channel 26). Captures and RTT logs are kept in `~/ib01-captures/pan43/`, not in git. Tags: **[V]** measured on hardware, **[D]** documented, **[I]** our inference. Background and plan: `open-questions.md` row 33, `README.md` "Project decisions".

## Result in one line

The spare device does not accept another PAN: it filters wake-up frames on PAN `0xABCD` whatever PAN its network was formed with [V]. The PAN stays fixed at `0xABCD` (owner, 2026-11-23). The record-sourced PAN code stays on main.

## Images

All from clean exports, airtime budget 30 s/h, `fw-reviewer` verdict "safe to flash" for each, reproduced bit-identical, bootloader `eb2f44fb` unchanged [D] (review verdicts, 2026-11-23):

| Image | Source | Test setting | Binary sha256 (start) |
| :-- | :-- | :-- | :-- |
| M | main `90ad9dc` | none (production) | `71352f50` |
| T1 | `670f3a0` on the test branch | `PAN43_WAKE_PAN=0x4e43`: wake trains for the spare also sent with PAN `0x4e43` | `03dc22b9` |
| T2 | `670f3a0` on the test branch | `RADIO_NET_PAN=0x4e43` (`2e30e6c`): new networks formed with PAN `0x4e43` | `6a900430` |

The test branch `pan43-test` (`2e30e6c`, `670f3a0`) was never pushed and is deleted; no test-only code is left in the tree.

## Test 1: wake trains with another PAN to a device paired on `0xABCD` (T1, flashed 18:33 UTC)

Spare paired on PAN `0xABCD` (record seq 3); 25 overlay PUTs 60 s apart.

- RTT totals [V]: trains with test PAN `0x4e43` ACKed 0 of 10; network-PAN trials 10 of 10; control (network PAN, attempt 2, about 2 s after a test miss, same short train) 7 of 10. The 3 control misses were each delivered on attempt 3 with a full train (248 wake frames).
- SDR (`t1.cu8`, 18:34:08.5 to 18:51:42.3 UTC; the owner unplugged the dongle before the end; covers 9 test trials), checked with `tools/sdr/pan43check.py` [V]: air matches RTT on every covered attempt. Each test train is complete (21-22 frames), all PAN bytes `43 4e`, all to the spare's short address only; 197 such frames = 21 + 8 x 22; none to `ff ff` or another address; no beacons. No Enhanced ACK and no CRC-bad burst within 30 ms after any test-PAN Data frame. Network-PAN ACKs 353-390 us after the Data frame; wake step 4050.5 us median.
- Conclusion [V]: the spare filters wake-up frames on PAN.
- Open [I]: control 7 of 10 against network 10 of 10 is either ordinary short-train loss (one normal short train also missed, seq 18, late ACK about 80 ms) or an after-effect of the ignored train; 10 trials cannot tell.
- The spare's RSSI at the bridge changed from -43 to -34.5 dBm at 18:51:18 (path change; its level at the dongle stayed steady) [I]. Not the cause: misses occurred at both levels.

## Test 2: network formed with PAN `0x4e43` (T2, flashed 19:01 UTC)

- Owner factory reset (PB10) at 19:02; log "no network record". Pairing window by server POST at 19:03:35; "network: created on channel 16 with PAN 4e43", record seq 1. Spare PAIRED at 19:04:03 (took the key, acked); POST found 2.03 in 6 ms; window closed by DELETE [V].
- Pairing-time `d/pair` delivered; spare uplinks were ACKed and reached the server [V]. So joining works: the spare takes the network from the Enhanced Beacon and does not check its PAN.
- After the window, downlinks by wake trains with PAN `0x4e43`: about 1 of 12 datagrams delivered (attempt 3, right after an uplink), the rest "no ack" on all 3 attempts, mostly full trains. Airtime reached about 26 s of 30 s/h and then frames were refused ("tx failed, 0 wake frames"); the budget worked as designed [V].
- Conclusion [I, strong]: the device keeps waking only on PAN `0xABCD` whatever PAN its network was formed with; this matches test 1.

## Restore (image M flashed 19:10)

- M accepted the `0x4e43` record ("record seq 1, channel 16, PAN 4e43: key set") [V]. Owner factory reset again; new network seq 1, channel 16, PAN `0xABCD` at 19:15; spare PAIRED 19:18 (POST found 2.03 in 4 ms) [V].
- 4 PUTs: 17 of 17 downlinks delivered on attempt 1 with a short train; ACKs prepared = sent; airtime 6.5 s/h. One "mic fail 1" right after the re-pair, not repeated [V].
- Rollback caveat [D] (`fw-reviewer`): images older than `90ad9dc` refuse a record with a PAN other than `0xABCD` and stay silent. Moot now, the record is `0xABCD` again.

## Decisions (owner, 2026-11-23)

Keep fixed PAN `0xABCD` (plan step 2); the random PAN of step 3 is not built. Keep the three commits on main although the PAN stays `0xABCD`; close #43 (`README.md`, "Project decisions").

## What stays in the code

- `7fc5f13`: wake-up frames and the beacon take the PAN from the network record (`src/mac.h`, `src/mac.c`, `src/csl.c`, `src/pair.c`, `src/link.c`).
- `3222b87`: transmit gate rule 7, `TXGATE_BAD_PAN`: a wake-up frame or Enhanced Beacon must carry our network's PAN, none goes out without a network (`src/txgate.c:174,255`, `src/txgate.h:35-38,122`). Data, ACK and broadcast-data frames have no PAN field (`radio.md:141`).
- `90ad9dc`: the store accepts any usable PAN, that is not `0xFFFF` or `0xFFFE` (`mac_pan_usable()`, `src/store.c:377,415`, `src/store.h:143`); `network.c` hands the PAN to the gate (`src/network.c:69`). New networks are always formed with `0xABCD` (`src/network.c:169`).

## Effect on the C2 finding (P7.1 review)

Our broadcast wake trains to `ff ff` still wake stock CSL devices on the shared PAN. A separate PAN would have spared them, but the devices do not accept another PAN, so that fix is not available [V]/[I]. The finding stays accepted as it was for the P7.1 runs, and any later channel-26 use needs the owner's approval first (`CLAUDE.md`).
