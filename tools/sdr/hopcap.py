#!/usr/bin/env python3
"""Alternating two-frequency capture with one RTL-SDR, for the pairing test.

Receive-only on the SDR, and never writes to the bridge: no memory write over
SWD, no arm word. One RTL-SDR cannot cover the test channel 16 (866.3242 MHz)
and the protected channel 26 (868.3237 MHz) at once: the span is 2.0 MHz, the
dongle gives 2.4 MS/s without drops, and docs/test-bench.md uses only +-0.8 MHz
of a view for decoding. So this script keeps one librtlsdr session streaming
and retunes between two centre frequencies, each 400 kHz below its channel to
keep the dongle's DC spike out of the band:

  A: 865.924158 MHz (channel 16 at +400 kHz; channels 9-19 in view, decoded
     10-18; the spare device's scan channels 11 and 16)
  B: 867.923670 MHz (channel 26 at +400 kHz; channels 19-29 in view, decoded
     20-28; the spare device's scan channels 21 and 26)

Hop policy, for the exchange after a beacon (Router Advertisement pairs, d/pair,
the device's 2.04): stay on A while channel 16 is busy. Go to B only once A has
been watched for at least --max-a s, and either channel 16 has been quiet for
--hold to --hold + --hold-window s since its last burst (so a hop never falls
in the 3 s before d/pair or between the trains of an advertisement pair, and B
is over before the spare device's next Beacon Request, every 8.0 s on
channel 16 while it scans: 5.5 + 2.0 + 0.11 s < 8.0 s), or it has been quiet
for --quiet-long s; stay on B for --dwell-b s, then back to A. With channel 16
quiet the cadence is 9 s on A, 2.1 s on B: against the 8.0 s scan the phase
moves 3.1 s per cycle, so at most one request in a row falls on B before the
first one seen locks the hops into the gaps (one B visit per 16 s).

A channel-16 row counts as a burst for the hold when it is --hold-db over the
channel's floor and not spill-over (spill-over: 15 dB or more below the
strongest channel in the same row). A -4 dBFS Beacon Request on channel 11,
or on channel 21 folded in from beyond the view's edge, puts +17 dB on
channel 16, which must not hold A: it would push the hop to B onto the
spare device's next request. With channel 16 busy without a break, hop anyway
after --max-a-hard s, so that channel 26 is still watched. After each retune
--settle s of samples are dropped (retune about 28 ms). Channel 26 cannot be
watched from A: it folds onto A's centre frequency (868.3237 - 2.4 MHz).

Output (keep outside git, it holds stock-network traffic):
  <out>.A.cu8, <out>.B.cu8   kept samples per centre frequency, back to back
  <out>.segments.csv         one row per kept stretch: where it sits in its
                             file and in the stream (samples since start)
  stdout                     hops, bursts per channel (live, 427 us rows),
                             alerts, RTT checks, a per-channel summary

Live burst detector: as tools/sdr/burstscan.py (256-point Hann FFTs at
2.4 MS/s, 4 rows averaged = 427 us per row, power within +-70 kHz of each
channel centre, threshold above that channel's running median). Burst start
and duration are good to one row (427 us); use demod.py on the .cu8 files for
microsecond timing.

Alerts (logged as ALERT; with --halt-on-alert also an OpenOCD `halt` over
telnet 4444, nothing else): a burst on B on channel 26 whose peak exceeds
--b-max dB (absolute, same scale); a burst on A on a channel other than 16,
not during a channel-16 burst, within 15 dB of the strongest channel-16 frames
(stock devices near the dongle trigger this too, so it is a pointer for the
offline check, not proof); a pair: beacon line in --rtt-log with no channel-16
burst while A was watched. Checks start --guard-after s into the stream.
"""
import argparse
import collections
import ctypes
import queue
import re
import socket
import sys
import threading
import time

import numpy as np

FS = 2400000
CH0 = 863124939.0
SP = 199951.172
NFFT = 256
AVG = 4
BUF_BYTES = 65536  # 32768 complex samples, 13.65 ms
BUF_SAMPLES = BUF_BYTES // 2
OUR_CH = 16
LEAK_DB = 15.0  # a row this far below the strongest channel in view is spill-over (as burstscan.py --leak-db)
PROT_CH = 26
# RTT lines that report a frame of ours on the air: the pairing beacon, and
# the first transmit test's sender (reverted, kept to read its old logs).
RTT_SENT = re.compile(r"pair: beacon seq ([0-9a-fA-F]+) to the target sent:|testtx: seq ([0-9a-fA-F]+): sent \(")


def chf(n):
    return CH0 + n * SP


# 400 kHz below each channel, rounded as in docs/test-bench.md
FC = {"A": 865924158, "B": 867923670}
assert all(abs(chf(n) - 400e3 - FC[k]) < 2 for k, n in (("A", OUR_CH), ("B", PROT_CH)))


def channels_in_view(fc):
    return [n for n in range(0, 50) if abs(chf(n) - fc) < 1.1e6]


class Power:
    """Per-row channel power in dB (arbitrary but fixed scale)."""

    def __init__(self, fc):
        self.chans = channels_in_view(fc)
        f = (np.arange(NFFT) - NFFT // 2) * FS / NFFT
        self.masks = [np.abs(f - (chf(n) - fc)) <= 70e3 for n in self.chans]
        self.win = np.hanning(NFFT).astype(np.float32)

    def rows(self, raw):
        x = np.frombuffer(raw, np.uint8).astype(np.float32) - 127.4
        iq = (x[0::2] + 1j * x[1::2]).astype(np.complex64)
        nrow = len(iq) // NFFT
        s = np.fft.fftshift(np.fft.fft(iq[: nrow * NFFT].reshape(nrow, NFFT) * self.win, axis=1), axes=1)
        p = (s.real ** 2 + s.imag ** 2)
        p = p[: (nrow // AVG) * AVG].reshape(nrow // AVG, AVG, NFFT).mean(axis=1)
        out = np.stack([p[:, m].sum(axis=1) for m in self.masks], axis=1)
        return 10 * np.log10(out + 1e-9)  # rows x channels


def ocd(cmd):
    s = socket.create_connection(("127.0.0.1", 4444), timeout=2)
    s.settimeout(0.3)
    data = b""

    def drain(t):
        nonlocal data
        end = time.time() + t
        while time.time() < end:
            try:
                d = s.recv(4096)
                if not d:
                    return
                data += d
            except socket.timeout:
                pass

    drain(0.3)
    data = b""
    s.sendall(cmd.encode() + b"\n")
    drain(0.6)
    s.close()
    return data.decode("utf-8", "replace").replace("\x00", "").replace("\r", "").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output path prefix (outside git)")
    ap.add_argument("--seconds", type=float, default=900)
    ap.add_argument("--gain", type=int, default=197, help="tuner gain, tenths of dB")
    ap.add_argument("--max-a", type=float, default=9.0,
                    help="least time on A before a hop to B, s")
    ap.add_argument("--hold", type=float, default=5.0, help="hop to B only after this long without a channel-16 burst, s")
    ap.add_argument("--hold-window", type=float, default=0.5,
                    help="after a channel-16 burst, hop only from --hold to --hold + this s later, so B ends before "
                         "the next request 8.0 s on")
    ap.add_argument("--quiet-long", type=float, default=9.0,
                    help="with channel 16 quiet this long (longer than the 8.0 s scan), hop at any time, s")
    ap.add_argument("--hold-db", type=float, default=25.0,
                    help="a channel-16 row holds A only this far over its floor and not spill-over, dB")
    ap.add_argument("--max-a-hard", type=float, default=60.0, help="hop to B after this long on A even if channel 16 is busy, s")
    ap.add_argument("--dwell-b", type=float, default=2.0, help="time on B per visit, s")
    ap.add_argument("--settle", type=float, default=0.08, help="samples dropped after each retune, s")
    ap.add_argument("--thresh", type=float, default=12.0, help="burst threshold over the channel's running median, dB")
    ap.add_argument("--our-min", type=float, default=40.0,
                    help="dB over floor for a channel-16 burst of 4 ms or more to count as a strong frame")
    ap.add_argument("--b-max", type=float, default=None, help="alert: absolute peak dB on B channel 26 above which to alert")
    ap.add_argument("--guard-after", type=float, default=0.0, help="start the alert checks this many s into the stream")
    ap.add_argument("--halt-on-alert", action="store_true",
                    help="on the first alert also halt the MCU (OpenOCD telnet 4444 `halt`; no memory write)")
    ap.add_argument("--stopfile", default=None)
    ap.add_argument("--rtt-log", default=None,
                    help="RTT log, each line prefixed with host time.time(); every 'pair: beacon seq NN to the target sent:' "
                         "line must match a channel-16 burst")
    a = ap.parse_args()

    lib = ctypes.CDLL("librtlsdr.so.0")
    lib.rtlsdr_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint32]
    for fn, at in [("rtlsdr_set_sample_rate", [ctypes.c_uint32]), ("rtlsdr_set_center_freq", [ctypes.c_uint32]),
                   ("rtlsdr_set_tuner_gain_mode", [ctypes.c_int]), ("rtlsdr_set_tuner_gain", [ctypes.c_int]),
                   ("rtlsdr_set_agc_mode", [ctypes.c_int]), ("rtlsdr_reset_buffer", []),
                   ("rtlsdr_cancel_async", []), ("rtlsdr_close", [])]:
        getattr(lib, fn).argtypes = [ctypes.c_void_p] + at
    CB = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint32, ctypes.c_void_p)
    lib.rtlsdr_read_async.argtypes = [ctypes.c_void_p, CB, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]

    dev = ctypes.c_void_p()
    if lib.rtlsdr_open(ctypes.byref(dev), 0) != 0:
        sys.exit("rtlsdr_open failed")
    lib.rtlsdr_set_sample_rate(dev, FS)
    lib.rtlsdr_set_tuner_gain_mode(dev, 1)
    lib.rtlsdr_set_tuner_gain(dev, a.gain)
    lib.rtlsdr_set_agc_mode(dev, 0)
    lib.rtlsdr_set_center_freq(dev, FC["A"])
    lib.rtlsdr_reset_buffer(dev)

    q = queue.Queue()
    count = [0]

    @CB
    def cb(buf, n, ctx):
        q.put((count[0], time.time(), ctypes.string_at(buf, n)))
        count[0] += 1

    th = threading.Thread(target=lib.rtlsdr_read_async, args=(dev, cb, None, 32, BUF_BYTES), daemon=True)
    th.start()

    def log(msg):
        print(msg, flush=True)

    files = {k: open(a.out + "." + k + ".cu8", "wb") for k in "AB"}
    nsamp = {"A": 0, "B": 0}
    segf = open(a.out + ".segments.csv", "w", buffering=1)
    segf.write("tag,fc,file_sample0,stream_sample0,host_arrival_first_buffer\n")
    power = {k: Power(FC[k]) for k in "AB"}
    floors = {k: collections.defaultdict(lambda: collections.deque(maxlen=300)) for k in "AB"}
    inburst = {}  # (tag, ch) -> [start_t, peak, peak_abs, rows, clip, own rows]
    nburst = collections.Counter()  # (tag, ch) -> bursts
    t0_host = None

    state = "A"
    hop_settle_idx = -1  # buffers with idx < this after a hop are dropped
    hop_start_idx = 1 << 60
    pending_state = None
    t_enter = 0.0
    dwell_until = None
    seg_open = False
    alerted = threading.Event()
    our_peaks = []
    our_rows_recent = collections.deque(maxlen=64)  # stream times of rows with a channel-16 burst
    last_our = -1e9  # stream time of the last channel-16 burst row
    a_cover = []  # [stream start, stream end] of kept stretches on A
    stats = {"A": 0, "B": 0, "drop": 0}

    log("CONFIG A fc %d (ch%02d at %+.0f Hz, channels %s), B fc %d (ch%02d at %+.0f Hz, channels %s)" % (
        FC["A"], OUR_CH, chf(OUR_CH) - FC["A"], power["A"].chans, FC["B"], PROT_CH, chf(PROT_CH) - FC["B"],
        power["B"].chans))
    log("CONFIG max-a %.1f s, hold %.1f s (rows >= +%.0f dB), max-a-hard %.1f s, dwell-b %.1f s, settle %.3f s, "
        "thresh %.1f dB, halt-on-alert %s" % (a.max_a, a.hold, a.hold_db, a.max_a_hard, a.dwell_b, a.settle, a.thresh,
                                              a.halt_on_alert))

    def do_hop(to, t, why):
        nonlocal hop_settle_idx, hop_start_idx, seg_open, pending_state
        hop_start_idx = count[0]
        h0 = time.time()
        lib.rtlsdr_set_center_freq(dev, FC[to])
        hop_settle_idx = count[0] + int(round(a.settle * FS / BUF_SAMPLES)) + 1
        pending_state = to
        log("HOP t=%.4f -> %s (%s), retune %.1f ms, drop buffers %d..%d" % (
            t, to, why, (time.time() - h0) * 1e3, hop_start_idx, hop_settle_idx - 1))

    def alert(reason, t):
        if t < a.guard_after:
            return
        log("ALERT: %s" % reason)
        if a.halt_on_alert and not alerted.is_set():
            alerted.set()
            threading.Thread(target=lambda: log("ALERT halt: %r" % ocd("halt"))).start()

    rtt_sent = []  # (host time of the RTT line, seq), appended by the tail thread
    our_host = []  # host times of strong channel-16 bursts

    def tail_rtt():
        with open(a.rtt_log) as f:
            f.seek(0, 2)
            while True:
                line = f.readline()
                if not line:
                    time.sleep(0.05)
                    continue
                m = RTT_SENT.search(line)
                if m:
                    h = line.split(" ", 1)[0]
                    rtt_sent.append((float(h), m.group(1) or m.group(2)))
                if "pair:" in line or "testtx:" in line:
                    log("RTT %s" % line.strip())

    if a.rtt_log:
        threading.Thread(target=tail_rtt, daemon=True).start()
    checked = 0

    end_reason = "seconds"
    try:
        while True:
            idx, harr, raw = q.get()
            if t0_host is None:
                t0_host = harr - BUF_SAMPLES / FS
                log("START host=%.6f (first buffer arrival minus one buffer), fs %d, gain %.1f dB" % (
                    t0_host, FS, a.gain / 10))
            t = idx * BUF_SAMPLES / FS  # stream time of the buffer's first sample
            if t > a.seconds:
                break
            if a.stopfile and idx % 64 == 0:
                try:
                    open(a.stopfile).close()
                    end_reason = "stopfile"
                    break
                except OSError:
                    pass
            if idx >= hop_start_idx and idx < hop_settle_idx:
                stats["drop"] += 1
                seg_open = False
                continue
            if idx < hop_start_idx:
                tag = state
            else:
                if pending_state is not None:
                    state = pending_state
                    pending_state = None
                    t_enter = t
                    for key in [k for k in inburst if k[0] != state]:
                        del inburst[key]
                hop_start_idx = 1 << 60
                tag = state
            tend = t + BUF_SAMPLES / FS
            if not seg_open:
                segf.write("%s,%d,%d,%d,%.6f\n" % (tag, FC[tag], nsamp[tag], idx * BUF_SAMPLES, harr))
                seg_open = True
                if tag == "A":
                    a_cover.append([t, tend])
            elif tag == "A":
                a_cover[-1][1] = tend
            files[tag].write(raw)
            nsamp[tag] += len(raw) // 2
            stats[tag] += 1

            P = power[tag]
            rows = P.rows(raw)
            rowmax = rows.max(axis=1)  # strongest channel per row: rows LEAK_DB below it are spill-over
            u8 = np.frombuffer(raw, np.uint8)
            clip = float(np.mean((u8 == 0) | (u8 == 255)))
            rt = NFFT * AVG / FS
            for j, n in enumerate(P.chans):
                fl = floors[tag][n]
                fl.append(float(np.median(rows[:, j])))
                floor = float(np.median(fl))
                key = (tag, n)
                for r, v in enumerate(rows[:, j]):
                    tr = t + r * rt
                    if v > floor + a.thresh:
                        own = v > rowmax[r] - LEAK_DB
                        if key not in inburst:
                            inburst[key] = [tr, v - floor, v, 0, clip, 0]
                        b = inburst[key]
                        b[3] += 1
                        b[5] += own
                        if v - floor > b[1]:
                            b[1], b[2] = v - floor, v
                        b[4] = max(b[4], clip)
                        if tag == "A" and n == OUR_CH and own and v - floor >= a.hold_db:
                            our_rows_recent.append(tr)
                            last_our = tr
                    elif key in inburst:
                        bs, pk, pka, nr, cl, no = inburst.pop(key)
                        dur = nr * rt * 1e3
                        nburst[key] += 1
                        log("BURST %s ch%02d t=%.4f host=%.3f dur=%.2fms peak=+%.1fdB abs=%.1fdB clip=%.4f%s" % (
                            tag, n, bs, t0_host + bs, dur, pk, pka, cl, "" if no else " leak"))
                        if tag == "A" and n == OUR_CH and dur >= 4 and pk >= a.our_min:
                            our_peaks.append(pka)
                            our_host.append(t0_host + bs)
                        if tag == "B" and n == PROT_CH and a.b_max is not None and pka > a.b_max:
                            alert("burst on B ch%02d abs %.1f dB > b-max %.1f at t=%.4f" % (n, pka, a.b_max, bs), bs)
                        if tag == "A" and n != OUR_CH and our_peaks and no:
                            near = any(abs(x - bs) < 0.02 or (bs - 0.002 <= x <= bs + dur / 1e3 + 0.002)
                                       for x in our_rows_recent)
                            if not near and pka > np.max(our_peaks[-50:]) - 15:
                                alert("strong burst on A ch%02d abs %.1f dB, not during a channel-%d burst, t=%.4f" % (
                                    n, pka, OUR_CH, bs), bs)

            # every beacon the firmware reports sent must be a burst on channel 16, if A was watched then
            while checked < len(rtt_sent) and t0_host + tend > rtt_sent[checked][0] + 1.5:
                hr, sq = rtt_sent[checked]
                m = [x for x in our_host if hr - 0.5 <= x <= hr + 0.2]
                lo, hi = hr - 0.5 - t0_host, hr + 0.2 - t0_host
                watched = any(s < hi and e > lo for s, e in a_cover)
                if m:
                    res = "channel-%d burst at host=%.3f (%.3f s before the log line)" % (OUR_CH, m[-1], hr - m[-1])
                elif watched:
                    res = "NO channel-%d burst while A was watched" % OUR_CH
                    alert("seq %s reported sent but not seen on channel %d" % (sq, OUR_CH), lo)
                else:
                    res = "not watched (on B)"
                log("CHECK seq %s rtt host=%.3f: %s" % (sq, hr, res))
                checked += 1

            # hop policy, on stream time
            if pending_state is None and hop_start_idx == 1 << 60:
                if state == "A":
                    on_a = tend - t_enter
                    quiet = tend - last_our
                    in_gap = a.hold <= quiet <= a.hold + a.hold_window  # before the next 8 s request
                    if on_a >= a.max_a and (in_gap or quiet >= a.quiet_long):
                        dwell_until = tend + a.dwell_b
                        do_hop("B", tend, "%.1f s on A, channel %d %s" % (
                            on_a, OUR_CH, "quiet %.1f s" % quiet if last_our > 0 else "no burst yet"))
                    elif on_a >= a.max_a_hard:
                        dwell_until = tend + a.dwell_b
                        do_hop("B", tend, "%.1f s on A, channel %d still busy" % (on_a, OUR_CH))
                elif state == "B" and tend >= dwell_until:
                    do_hop("A", tend, "dwell over")
    except KeyboardInterrupt:
        end_reason = "interrupt"
    finally:
        lib.rtlsdr_cancel_async(dev)
        th.join(timeout=3)
        lib.rtlsdr_close(dev)
        for f in files.values():
            f.close()
        segf.close()
        hnow = time.time()
        tot = stats["A"] + stats["B"] + stats["drop"]
        log("END (%s) buffers A %d B %d dropped-after-hop %d (A %.1f %%); stream %.3f s, host %.3f s since start, "
            "%d left in queue" % (end_reason, stats["A"], stats["B"], stats["drop"], 100.0 * stats["A"] / max(tot, 1),
                                  count[0] * BUF_SAMPLES / FS, hnow - t0_host if t0_host else 0, q.qsize()))
        log("SUMMARY bursts " + " ".join("%s:ch%02d=%d" % (k[0], k[1], v) for k, v in sorted(nburst.items())))


if __name__ == "__main__":
    main()
