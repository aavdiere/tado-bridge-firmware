#!/usr/bin/env python3
"""Alternating two-frequency capture with one RTL-SDR, for a transmit test.

Receive-only on the SDR. One RTL-SDR cannot cover the test channel 10
(865.1245 MHz) and the protected channel 26 (868.3237 MHz) at once: the span
is 3.3 MHz, the dongle gives 2.4 MS/s without drops. So this script keeps one
librtlsdr session streaming and retunes between two centre frequencies:

  A: 864.724451 MHz (channel 10 at +400 kHz; channels 3-13 in view)
  B: 867.923670 MHz (channel 26 at +400 kHz; channels 19-29 in view)

Hop policy (frames from the bench bridge are 8 ms long, at least 5.0 s apart):
stay on A until a strong burst on channel 10 ends, then go to B 0.1 s later
for --dwell-b seconds, then back to A. With no frame on A for --max-a seconds
(before the sender is armed, after it stops) go to B for --idle-b seconds.
So every test frame is seen on A, and channel 26 is watched most of the rest
of the time. After each retune, --settle seconds of samples are dropped.

Output (keep outside git, it holds stock-network traffic):
  <out>.A.cu8, <out>.B.cu8   kept samples per centre frequency, back to back
  <out>.segments.csv         one row per kept stretch: where it sits in its
                             file and in the stream (samples since start)
  stdout                     hops, bursts per channel (live, 427 us rows), safety

Live burst detector: as tools/sdr/burstscan.py (256-point Hann FFTs, 4 rows
averaged = 427 us, power within +-70 kHz of each channel centre, threshold
above that channel's running median).

Safety (only once armed, which --arm-after does over OpenOCD telnet 4444):
a burst on B whose peak exceeds --b-max dB (absolute, same scale), or a burst
on A on a channel other than 10, not overlapping a channel-10 burst, within
15 dB of our own frames' peak, writes 0 to the arm word and halts the MCU.
"""
import argparse
import collections
import ctypes
import queue
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
ARM_ADDR = 0x20007990
ARM_MAGIC = 0x54583130
FC = {"A": 864724451, "B": 867923670}


def chf(n):
    return CH0 + n * SP


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
    ap.add_argument("--seconds", type=float, default=360)
    ap.add_argument("--gain", type=int, default=197, help="tuner gain, tenths of dB")
    ap.add_argument("--dwell-b", type=float, default=3.4)
    ap.add_argument("--idle-b", type=float, default=2.5)
    ap.add_argument("--max-a", type=float, default=8.0)
    ap.add_argument("--settle", type=float, default=0.08)
    ap.add_argument("--thresh", type=float, default=12.0)
    ap.add_argument("--arm-after", type=float, default=None, help="arm the sender at the first A window after this many s")
    ap.add_argument("--b-max", type=float, default=None, help="safety: absolute peak dB on B above which to stop")
    ap.add_argument("--stopfile", default=None)
    ap.add_argument("--our-min", type=float, default=40.0, help="dB over floor for a channel-10 burst to count as our frame")
    ap.add_argument("--rtt-log", default=None,
                    help="RTT log with host-time prefix; every 'testtx: seq NN: sent' must match a channel-10 burst")
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
    inburst = {}  # (tag, ch) -> [start_t, peak, peak_abs, rows, clip]
    t0_host = None

    state = "A"
    hop_settle_idx = -1  # buffers with idx < this after a hop are dropped
    hop_start_idx = 1 << 60
    pending_state = None
    t_enter = 0.0
    hop_at = None
    dwell_until = None
    seg_open = False
    armed = False
    arm_pending = False
    safety = threading.Event()
    our_peaks = []
    ch10_rows_recent = collections.deque(maxlen=64)  # stream times of rows with a channel-10 burst
    stats = {"A": 0, "B": 0, "drop": 0}

    def do_hop(to, t, why):
        nonlocal state, hop_settle_idx, hop_start_idx, t_enter, seg_open, pending_state
        hop_start_idx = count[0]
        h0 = time.time()
        lib.rtlsdr_set_center_freq(dev, FC[to])
        hop_settle_idx = count[0] + int(round(a.settle * FS / BUF_SAMPLES)) + 1
        pending_state = to
        log("HOP t=%.4f -> %s (%s), retune %.1f ms, drop buffers %d..%d" % (
            t, to, why, (time.time() - h0) * 1e3, hop_start_idx, hop_settle_idx - 1))

    def arm():
        r = ocd("mww 0x%08x 0x%08x" % (ARM_ADDR, ARM_MAGIC))
        h = time.time()
        rb = ocd("mdw 0x%08x" % ARM_ADDR)
        log("ARM host=%.3f reply=%r readback=%r" % (h, r, rb))

    def stop_tx(reason):
        if safety.is_set():
            return
        safety.set()
        log("SAFETY STOP: %s" % reason)
        log("SAFETY disarm: %r" % ocd("mww 0x%08x 0" % ARM_ADDR))
        log("SAFETY halt: %r" % ocd("halt"))

    rtt_sent = []  # (host time of the RTT line, seq), appended by the tail thread
    our_host = []  # host times of our channel-10 bursts

    def tail_rtt():
        with open(a.rtt_log) as f:
            f.seek(0, 2)
            while True:
                line = f.readline()
                if not line:
                    time.sleep(0.05)
                    continue
                if "testtx: seq" in line and ": sent (" in line:
                    h, rest = line.split(" ", 1)
                    rtt_sent.append((float(h), rest.split("seq ")[1][:2]))
                    log("RTT %s" % line.strip())
                elif "testtx:" in line:
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
            if not seg_open:
                segf.write("%s,%d,%d,%d,%.6f\n" % (tag, FC[tag], nsamp[tag], idx * BUF_SAMPLES, harr))
                seg_open = True
            files[tag].write(raw)
            nsamp[tag] += len(raw) // 2
            stats[tag] += 1

            P = power[tag]
            rows = P.rows(raw)
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
                        if key not in inburst:
                            inburst[key] = [tr, v - floor, v, 0, clip]
                        b = inburst[key]
                        b[3] += 1
                        if v - floor > b[1]:
                            b[1], b[2] = v - floor, v
                        b[4] = max(b[4], clip)
                        if tag == "A" and n == 10:
                            ch10_rows_recent.append(tr)
                    elif key in inburst:
                        bs, pk, pka, nr, cl = inburst.pop(key)
                        dur = nr * rt * 1e3
                        log("BURST %s ch%02d t=%.4f host=%.3f dur=%.2fms peak=+%.1fdB abs=%.1fdB clip=%.4f" % (
                            tag, n, bs, t0_host + bs, dur, pk, pka, cl))
                        if tag == "A" and n == 10 and dur >= 4 and pk >= a.our_min:
                            our_peaks.append(pka)
                            our_host.append(t0_host + bs)
                            if state == "A" and pending_state is None and hop_at is None:
                                hop_at = tr + 0.10
                        if armed and tag == "B" and a.b_max is not None and pka > a.b_max:
                            threading.Thread(target=stop_tx, args=(
                                "burst on B ch%02d abs %.1f dB > b-max %.1f at t=%.4f" % (n, pka, a.b_max, bs),)).start()
                        if armed and tag == "A" and n != 10 and our_peaks:
                            near10 = any(abs(x - bs) < 0.02 or (bs - 0.002 <= x <= bs + dur / 1e3 + 0.002)
                                         for x in ch10_rows_recent)
                            if not near10 and pka > np.median(our_peaks) - 15:
                                threading.Thread(target=stop_tx, args=(
                                    "strong burst on A ch%02d abs %.1f dB, not during a channel-10 frame, t=%.4f" % (
                                        n, pka, bs),)).start()

            # every frame the firmware reports sent must be one we saw on channel 10
            tend = t + BUF_SAMPLES / FS
            while checked < len(rtt_sent) and t0_host + tend > rtt_sent[checked][0] + 1.5:
                hr, sq = rtt_sent[checked]
                m = [x for x in our_host if hr - 0.5 <= x <= hr + 0.2]
                log("CHECK seq %s rtt host=%.3f: %s" % (
                    sq, hr, "channel-10 burst at host=%.3f (%.3f s before the log line)" % (m[-1], hr - m[-1])
                    if m else "NO channel-10 burst"))
                if not m:
                    threading.Thread(target=stop_tx, args=(
                        "seq %s reported sent but not seen on channel 10" % sq,)).start()
                checked += 1

            # hop policy, on stream time
            if pending_state is None and hop_start_idx == 1 << 60:
                if state == "A":
                    if (a.arm_after is not None and not armed and not arm_pending and tend >= a.arm_after
                            and tend - t_enter > 0.3):
                        arm_pending = True
                        armed = True
                        log("ARMING at t=%.4f host=%.3f" % (tend, time.time()))
                        threading.Thread(target=arm).start()
                    if hop_at is not None and tend >= hop_at:
                        hop_at = None
                        dwell_until = tend + a.dwell_b
                        do_hop("B", tend, "after channel-10 frame")
                    elif hop_at is None and tend - t_enter > a.max_a:
                        dwell_until = tend + a.idle_b
                        do_hop("B", tend, "no frame for %.1f s" % a.max_a)
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
        log("END (%s) buffers A %d B %d dropped-after-hop %d; stream %.3f s, host %.3f s since start, %d left in queue" % (
            end_reason, stats["A"], stats["B"], stats["drop"], count[0] * BUF_SAMPLES / FS,
            hnow - t0_host if t0_host else 0, q.qsize()))


if __name__ == "__main__":
    main()
