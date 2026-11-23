# P7.1 results: channel 26 next to the stock network

Results of stage P7.1 (#20), moved out of `test-bench.md` (2026-11-23) to keep that file under the size limit of `tools/check-no-firmware.sh`. The plan, the abort rules and the commands are in `test-bench.md`, "P7.1 channel 26, plan".

## P7.1 step 1 baseline result (2026-11-21) [V]

Step 1 of `test-bench.md`, "P7.1 channel 26, plan", run by `rf-bench`. Receive-only: the SDR only receives, and nobody touched the bridge (no flash, no SWD write). The bridge ran image 9 on channel 16 with TX on, as the main session confirmed. Captures, CSV files and the baseline file are in `~/ib01-captures/p71/` (`baseline.cu8`, `baseline.frames.csv`, `baseline.summary.txt`, `baseline.score.txt`, `baseline.json`), outside git. No address is written here: senders are named by role only.

**Capture.** `rtl_sdr -f 867923670 -s 2400000 -g 8.7 - | python3 -I tools/sdr/timedcap.py ~/ib01-captures/p71/baseline.cu8 2160`. The R820T takes 8.7 dB as a valid step (`rtl_sdr -g` is in dB; `timedcap.py` only writes the stream, `tools/sdr/timedcap.py:1-12`). It ran from 14:12:30.6 to 14:48:31.4 UTC, 2160.1 s (36 min) of 2.4 MS/s IQ (10.37 GB). No samples were lost: the byte count matches the host clock to within ±29 ms over the run (USB buffering; `baseline.cu8.times.csv`). The final `Short write, samples lost` from `rtl_sdr` is the pipe closing at the end. A first try at 14:10 got `usb_open error -3`: the owner's desktop session was not active, so udev's `uaccess` ACL on the dongle was gone (`/usr/lib/udev/rules.d/10-rtl-sdr.rules`, `GROUP="rtlsdr", TAG+="uaccess"`; the user is not in group `rtlsdr`). The capture started by itself once the ACL came back. **For runs A1, B and A2:** start the SDR while the owner's session is active. A capture that is already running keeps its handle.

**Decode.** `python3 -I tools/sdr/demod.py --fc 867923670 --channels 26 --file ~/ib01-captures/p71/baseline.cu8 --redact --quiet --summary --csv ~/ib01-captures/p71/baseline.frames.csv`. Result: 9360 sync hits, 9161 CRC OK (97.9 %): wake-up 6713, broadcast 1356, Enhanced ACK 524, Data 549, Beacon Request 19 (a stock device scanning; no Enhanced Beacon). The stock timing matches `test-bench.md`, "Stock network reference":
- Uplinks, which the coordinator ACKs: Data end to ACK preamble 1438-2153 µs, median 1835 µs (n=325).
- Downlinks, which a device ACKs: 349-396 µs, median 379 µs (n=199).
- The coordinator's carrier and bit rate disagree as before: +4.5 kHz against -32 ppm.

Timing resolution: frame times come from `demod.py`'s least-squares sync fit at 600 kS/s (1.667 µs per sample), good to well under 1 µs (`test-bench.md`, "Demodulator"). Busy shares and listen windows are built from frame intervals (preamble start to last CRC bit), so they carry the same resolution. Energy that `demod.py` does not decode is not included (see the limits below).

**Scoring.** `tools/sdr/stockscore.py` (new, receive-only, prints no address) works per 10-minute block from capture start. It finds the coordinator as the most frequent source of Data frames after a rendezvous-0 wake-up (`tools/sdr/stockscore.py:286-289`). It counts the following:
- **down:** coordinator Data frames, ACKed when an Enhanced ACK with the same seq comes from the addressed device within 20 ms (`:331-339`).
- **up:** device Data frames, ACKed the same way by the coordinator.
- **dmg:** frames without a CRC-OK ACK but with a CRC-failed 29-byte hit (the ACK's length) in the ACK window. This is an ACK that was sent but arrived damaged at the SDR (`:344-353`).
- **retransmissions:** the same source, destination and seq within 30 s (`:358-364`). Every repeat in the capture fell either at 1.2-3.0 s or at 661 s or more, which is broadcast sequence-number wrap.
- **CRC failures**, and those that overlap a CRC-OK frame.
- **busy share.**
- **LBT busy rate:** the share of backoff-plus-listen windows `[T - (3.968 ms + k x 640 us), T]`, k uniform 0-6, that hold any part of a frame (exact over T, `:415-419`). It also gives the 3.968 ms listen alone (k = 0), the only part `src/csl.c:516,627-631` actually listens to.
- **LBT loop Monte Carlo:** busy means retry 20 ms later with a new backoff, and 10 busy listens in a row end the attempt (`src/csl.c:428-434`, `src/csl.h:38-43`; `tools/sdr/stockscore.py:180-206`).

| Block (stream s) | Coordinator Data ACKed by device | Device uplinks ACKed by coordinator | Retransmissions | CRC failures (overlapping a frame) | Busy | LBT windows busy (listen alone) | LBT attempts ended BUSY |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 0 (0-600) | 49/54 = 90.7 % (+3 dmg) | 79/80 = 98.8 % (+1 dmg) | 1 | 52 (2) | 2.28 % | 2.97 % (2.79 %) | 0.26 % |
| 1 (600-1200) | 64/73 = 87.7 % (+4 dmg) | 103/105 = 98.1 % | 2 | 60 (3) | 2.55 % | 3.31 % (3.12 %) | 0.13 % |
| 2 (1200-1800) | 48/51 = 94.1 % (+2 dmg) | 85/86 = 98.8 % | 1 | 57 (0) | 2.34 % | 3.05 % (2.87 %) | 0.28 % |
| 3 (1800-2160, 6 min) | 38/40 = 95.0 % | 58/60 = 96.7 % | 3 | 30 (2) | 2.63 % | 3.38 % (3.20 %) | 0.23 % |
| Whole capture, pooled | 199/218 = **91.3 %** | 325/331 = **98.2 %** | 7 = **1.94 per 10 min** | 56.3 per block (1.7) | 2.39 % | 3.11 % (2.93 %) | 0.22 % |

Over sliding 10-minute windows (30 s steps), the downlink share ranges from 86.1 % to 96.6 % and the uplink share from 96.8 % to 99.0 %. The most retransmissions in any 10-minute window is 4.

**Thresholds of abort rule 2** (from the pooled baseline, `baseline.json`):
- Block over when coordinator Data ACKed by device falls **below 76.3 %**.
- Block over when device uplinks ACKed by coordinator fall **below 83.2 %**.
- Block over when retransmissions exceed **3.9 per 10-minute block** (that is, 4 or more).
- Block over when **more than 3** CRC-failed frames in a block overlap a frame of ours.

**Other figures [V].**
- **Senders:** the coordinator plus 5 devices (6 distinct short addresses).
- **Downlinks** went to 3 devices: 49 of 50 ACKed, 51 of 51, and 99 of 117 (84.6 %). Most of the unACKed downlinks are to the third device. Nine of them have a damaged ACK-length hit in the ACK window. The coordinator retried each unACKed downlink later, with a new train and a new seq, 1-58 s after.
- **Stock wake-up trains:** 253 in 36 min, median 85 ms long, 1.25 % of the time. Six of them are `ff ff` broadcast trains of about 1.0 s each (about 10 an hour).
- **CRC failures:** 77 % of them are weak, below -50 dBFS (median -58.9 dBFS, SNR about 5 dB). That points to distant senders or noise sync hits rather than collisions **[I]**.
- **Anything of ours on channel 26: none.** No frame carries the bridge's address (learned from the channel-16 capture `~/ib01-captures/found41/run.A.frames.csv`, the source of the Data frames that follow its trains). No frame carries the spare device's address. No wake-up train has the bridge's step (4048 µs ± 15 µs; the stock steps are 3.94-3.99 ms and 4.40-4.45 ms). No sender has an unknown carrier offset.
- **Short-address check (D7): PASS.** None of the 6 stock short addresses (8972 occurrences, from wake-up frames and from every address in Data, ACK and Beacon Request frames) has the same SHA-256 as the spare device's short address. The spare's short address is bytes 6 and 7 of the stored `PAIR_TARGET_EUI64`, i.e. wire bytes 1 and 0 (`src/csl.c:577-579`, `cmake/CMakeLists.txt:129-136`, `tools/sdr/stockscore.py:123-136,502-520`). Neither value is printed.
- **A side finding** from the 2026-11-11 capture behind "Stock network reference" (`~/ib01-captures/ch26_300s.frames.csv`): the spare device's address was then on channel 26 as the source of 100 broadcasts in 300 s (one every 3 s). That capture had 6 devices; today's has 5 and no frame of the spare. **[I]** The spare was probably a member of the stock network until it was set aside as the spare (owner, 2026-11-16). The stock coordinator sent it nothing in these 36 min.

**What this means [I].**
- **Abort rule 2, retransmissions: the rule as written trips on the baseline itself.** At 1.94 per block, "more than double" means 4 in a block. The baseline already reached 4 in one 10-minute window, and its last 6-minute block had 3. With so few counts, a per-block doubling test cannot tell our effect from chance. **Owner decision 2026-11-23** (`README.md`, "Project decisions", "P7.1 retransmission rule"): a run is over if its retransmissions over the whole run exceed twice the baseline rate for that span (11.7 for 30 minutes), or if any 10-minute block has 6 or more (scaled for a short last block). `stockscore.py` applies this by default (`--retx-block 6`; the run test prints `RETX-RUN`). Scored against itself, the baseline is now within all thresholds (7 retransmissions over 2158 s against a run limit of 14.0) **[V]**. The two ACK shares and the CRC-over-ours test have margin: the lowest baseline windows are 10 and 14 percentage points above their thresholds.
- **LBT will see the channel busy at about 3 % of attempts:**
  - 2.9 % of single listens are busy, and 3.1 % of the backoff-plus-listen windows of the plan.
  - Retries average 1.07 listens per attempt.
  - About 0.2 % of attempts end BUSY after 10 busy listens. Nearly all of these fall inside the stock `ff ff` trains: 1 s of wake-ups 4 ms apart, longer than the 10 retries take (about 0.25 s).
  - So in runs A1 and A2 expect `listens busy` in about 3 % of listens and BUSY ends rarely, about 1 in 450 attempts. Run B's `busy listens ignored` should come out at about the same 3 %.
  - The real listen also flags RSSI at or above -85 dBm from energy that `demod.py` does not decode. The SDR cannot place that threshold (dBFS is uncalibrated), so the bridge's `listens busy by rssi` count is the check.
- Our 30 s per hour budget (0.83 %) is about a third of the stock network's 2.4 % busy share in this baseline (2.5-6.1 % in shorter earlier runs, `test-bench.md`, "Step 3 baseline").
- **Limits.** `demod.py` does not look for a sync word inside a frame it has already decoded. A stock frame that starts during one of ours is therefore missed, not counted as a collision (`tools/sdr/demod.py:162`). In the runs, the "CRC failures overlapping ours" count is a lower bound. The ACK shares are as heard at the SDR, so a damaged ACK ("dmg") counts as unACKed in the baseline and in the runs alike. The `tado` app's state during the baseline is for the owner to note; it is not part of this record.

**Scoring a later run (A1, B, A2) against this baseline.** Capture as above for at least the run's 30 minutes, then decode the same way. Score with the run's start time from the RTT log, a Unix time (`--start-host`), so the blocks line up with the owner's checks:

```
rtl_sdr -f 867923670 -s 2400000 -g 8.7 - | python3 -I tools/sdr/timedcap.py ~/ib01-captures/p71/runA1.cu8 1920
python3 -I tools/sdr/demod.py --fc 867923670 --channels 26 --file ~/ib01-captures/p71/runA1.cu8 \
    --redact --quiet --summary --csv ~/ib01-captures/p71/runA1.frames.csv > ~/ib01-captures/p71/runA1.summary.txt
python3 -I tools/sdr/stockscore.py --csv ~/ib01-captures/p71/runA1.frames.csv \
    --times ~/ib01-captures/p71/runA1.cu8.times.csv --start-host <run start, Unix s> \
    --ours-ref ~/ib01-captures/found41/run.A.frames.csv:16 --spare-cmake config/local.cmake \
    --baseline ~/ib01-captures/p71/baseline.json
```

Each block line ends in `ok` or in the names of the thresholds it is over (`DOWN-ACK`, `UP-ACK`, `RETX`, `CRC-OURS`). The block lines also give our frames, our trains and our `ff ff` trains per block, for rule 3 and the wake-train count. The tail gives the run pooled against the baseline, a `RETX-RUN` line if the run's retransmissions are over, and a verdict line. Decoding takes about a quarter of real time, about 8 minutes for a 32-minute capture.

## P7.1 runs A1 and B result (2026-11-23) [V]

Steps 2-5 of `test-bench.md`, "P7.1 channel 26, plan". Captures, CSV files, score outputs and RTT logs are in `~/ib01-captures/p71/` (`pair26.*`, `runA1.*`, `runB.*`, `runA.rtt.log`, `runB.rtt.log`), outside git. Senders by role or hash only. The owner checked the tado app at each 10-minute mark; no trouble was reported. Owner rule of 2026-11-23: no reply within 5 minutes of a reminder counts as ok.

**Step 2, image A.** Flashed over SWD and verified, `bfdd3a97` (the reviewed hash). RTT lost the radio task's boot lines twice; the re-pair below logged `replacing the record seq 1 for channel 16`, and before it the minute lines showed `tx frames 0` and `RA 0` (`runA.rtt.log`).

**Step 3, re-pair on channel 26 (10:18 UTC).** The window was opened from the server (localhost route, as in #41), and the spare device paired about 30 s later: `PAIRED: the target took the network key, acked`; `POST found` answered 2.03. The window was closed by DELETE after about 35 s. The spare's device row stayed as it was (home 1, zone 1, VA02). SDR (`pair26.ours.txt`): 2 Enhanced Beacons, each 4057-4058 us after a Beacon Request of the spare (rule: 2.4-7.4 ms); 10 stock Beacon Requests after the window closed got nothing.

**Scores against the baseline** (`stockscore.py`, owner's retransmission rule of 2026-11-23; per 10-minute block):

| Run | Block | Coordinator Data ACKed by device | Device uplinks ACKed | Retransmissions | CRC fails over ours | Stock busy | Result |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Baseline | pooled | 91.3 % | 98.2 % | 1.94 per block | - | 2.39 % | - |
| A1, LBT on | 0 | 45/48 93.8 % | 61/61 100 % | 0 | 0 | 2.09 % | ok |
| A1 | 1 | 49/53 92.5 % | 79/81 97.5 % | 2 | 0 | 2.03 % | ok |
| A1 | 2 | 46/53 86.8 % | 70/74 94.6 % | 4 | 0 | 2.10 % | ok |
| A1 | run | 91.4 % | 97.4 % | 6 (limit 12.4) | | | **within** |
| B, LBT off | 0 | 63/65 96.9 % | 89/98 90.8 % | 9 | 0 | 2.49 % | RETX |
| B | 1 | 97/100 97.0 % | 130/137 94.9 % | 7 | 0 | 3.21 % | RETX |
| B | 2 | 212/231 91.8 % | 291/297 98.0 % | 24 | 1 | 7.08 % | RETX |
| B | run | 93.9 % | 95.9 % | 40 (limit 11.7) | | | **OVER (RETX-RUN)** |

Sources: `runA1.score.txt` lines 4-13, `runA1.score.tail.txt` (the last 118 s, ok), `runB.score.txt` lines 4-14.

**Our side.**

| | A1 (LBT on) | B (LBT off) |
| :--- | :--- | :--- |
| Overlay PUTs (60 s apart, all 200; read back MANUAL 21.0 °C) | 30 | 30 |
| Downlinks to the spare (RTT `tx: datagram`) | 44, all delivered: 37 first attempt, 6 second, 1 third | 85, all delivered: 83 first attempt, 2 second |
| Spare's ACK to our Data, gap | 40 of 48, median 385.2 us (the 8 without are the RTT's `no ack`, nothing on air in their ACK windows) | 81, median 372.9 us |
| Our ACK to the spare's uplinks, gap | 40 of 40, median 223.3 us (222.7-223.6) | 81, median 223.3 us (222.8-233.8) |
| Wake-up trains, per-train median step | 52, 4049.7 us | 89, 4049.2 us |
| `ff ff` (broadcast RA) trains | 4 | 5 |
| Busy listens | 0 (`listens busy by rssi 0`) | 4 ignored (`busy listens ignored ... total 4`) |
| Highest airtime in the window | 22.0 s of 30 s, none refused | 15.5 s of 30 s, none refused |
| Answers to stock frames, rule-3 breaches | 0, 0 | 0, 0 |

Sources: `runA1.ours.txt`, `runA1.ackgap.ours.txt`, `runB.ours.txt`, `runA.rtt.log`, `runB.rtt.log`, `runA1.puts.log`, `runB.puts.log`. Channels 20-25, 27 and 28 in A1: no frame of ours. The RTT's `mic fail` count (about 1280 in A1) is the stock network's `49 e8` broadcasts under the stock key, from two senders about every 3 s each; our network shares PAN `0xABCD` (#43).

**Reading [I].** Run B trips on retransmissions only; the ACK shares and the CRC-over-ours test stay well inside. Stock traffic in B's block 2 was about four times the usual (231 coordinator Data frames, busy 7.1 %), so part of B's count follows the load. Per stock Data frame, though, B's retransmission rate is about 3-5 % in every block, against about 1.3 % in the baseline and in A1, so the excess very probably comes from transmitting without listening first. With LBT on, A1 shows no measurable effect on the stock network. Per abort rule 2, run A2 did not start.

**Bench state after the runs (12:36 UTC).** Image D (channel 16, `3d08cf92`, the reviewed hash, rebuilt from a clean export of `29510a9`) flashed over SWD and verified. Boot: `network: record seq 2 is for channel 26, this image for channel 16 only: not used, no key, nothing sent`; first minute line `tx frames 0`. The channel-26 record stays in flash: run A2, if the owner wants it, needs only image A again. Still to do for the end of P7.1: re-pair the spare device on channel 16 (owner: pairing mode), then 10 minutes of SDR on channel 26 (`test-bench.md`, "Back to channel 16", steps 2-3).

**Back to channel 16, result (2026-11-23) [V].** Owner decision the same day: P7.1 closes on A1 and B as they stand, A2 not run (`README.md`, "Project decisions", "P7.1 closed"). Step 1: image D as above. Step 2: the pairing window was opened from the server at 12:48 UTC: `network: replacing the record seq 2 for channel 26 with a new network on channel 16`, `record seq 3, channel 16: key set`; the spare device paired at the first beacon (`PAIRED` 12:48:5x, STX 3919 us after its request), `POST found` 2.03, window closed by DELETE; its uplinks reach the server again, with every ACK sent (`acks prepared 5 sent 5`), and its device row is unchanged (home 1, zone 1, VA02). Step 3: SDR on channel 26 from 12:48:32 UTC for 753 s (more than the 10 minutes the plan asks; the capture was stopped by the wait loop's timeout, not at 900 s as set), decoded live (`back16.frames.csv`): no frame with the bridge's address, no wake-up train of ours, no answer to any of 198 stock Data frames, rule-3 breaches 0 (`back16.ours.txt`). The one frame with the spare's address is its broadcast Beacon Request while in pairing mode (it scans every channel); nothing answered it. Step 4: the branch `p71-lbt-off` is deleted (and `p71-main`, already merged), and `RADIO_NET_CHANNEL` and `RADIO_ALLOW_CH26` stay (owner, 2026-11-23).
