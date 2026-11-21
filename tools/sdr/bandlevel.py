#!/usr/bin/env python3
"""Mean and peak level of one or more tado channels over time windows of a capture.

Receive-only, offline. For a question such as "did channel 26 rise while the
bridge sent a train on channel 16 during a B visit?": per window and channel,
the IQ is mixed to the channel, low-passed to +-(--bw / 2) (255-tap FIR at the
capture rate), and the power is averaged over --bin-ms bins. Printed per
window: the median and the maximum bin in dBFS (0 dBFS = a full-scale tone
of the u8 samples), and the share of bins more than --over-db above the
reference window's median (the first window unless --ref is given).

    python3 -I tools/sdr/bandlevel.py --file run.B.cu8 --fc 867923670 \\
        --channels 26,28 --win ref:58.0:58.4 --win train:60.07:60.51

Timing resolution: one bin (--bin-ms, default 0.5 ms); levels are relative
to the dongle's gain, comparable only within one capture. Windows are file
times of the given .cu8 (map hopcap stream times with run.segments.csv).
"""
import argparse
import sys

import numpy as np
from scipy import signal

sys.dont_write_bytecode = True


def chan_hz(n):
    return 863.125e6 + n * 199.951e3   # docs/radio.md, channel plan


def read_iq(path, fs, t0, t1):
    s, e = max(0, int(t0 * fs)), int(t1 * fs)
    with open(path, "rb") as f:
        f.seek(2 * s)
        b = f.read(2 * (e - s))
    x = np.frombuffer(b, dtype=np.uint8).astype(np.float32) - 127.4
    return (x[0::2] + 1j * x[1::2]).astype(np.complex64) / 127.6


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--file", required=True)
    ap.add_argument("--fc", type=float, required=True)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--channels", required=True, help="comma-separated channel numbers")
    ap.add_argument("--win", action="append", required=True, help="name:t0:t1 (file seconds)")
    ap.add_argument("--ref", default="", help="name of the reference window (default: the first)")
    ap.add_argument("--bw", type=float, default=100e3)
    ap.add_argument("--bin-ms", type=float, default=0.5)
    ap.add_argument("--over-db", type=float, default=6.0)
    a = ap.parse_args()

    chans = [int(c) for c in a.channels.split(",")]
    wins = []
    for w in a.win:
        name, t0, t1 = w.split(":")
        wins.append((name, float(t0), float(t1)))
    taps = signal.firwin(255, (a.bw / 2) / (a.fs / 2))
    nb = int(round(a.bin_ms * 1e-3 * a.fs))

    levels = {}
    for name, t0, t1 in wins:
        x = read_iq(a.file, a.fs, t0 - 0.001, t1 + 0.001)
        n = np.arange(len(x))
        for ch in chans:
            off = chan_hz(ch) - a.fc
            if abs(off) > a.fs / 2:
                off -= np.sign(off) * a.fs        # where an out-of-view channel folds in
            y = signal.lfilter(taps, 1.0, x * np.exp(-2j * np.pi * off / a.fs * n).astype(np.complex64))
            y = y[255 + int(0.001 * a.fs) - 127:]
            p = np.abs(y[: len(y) // nb * nb]) ** 2
            p = p.reshape(-1, nb).mean(axis=1)
            levels[(name, ch)] = 10 * np.log10(p + 1e-12)
    ref = a.ref or wins[0][0]
    print(f"# {a.file}: fc {a.fc / 1e6:.6f} MHz, +-{a.bw / 2e3:.0f} kHz per channel, "
          f"{a.bin_ms} ms bins; over = bins > {a.over_db} dB above the '{ref}' median")
    print("# window       t0        t1     ch  median_dBFS  max_dBFS  over_%")
    for name, t0, t1 in wins:
        for ch in chans:
            v = levels[(name, ch)]
            r = np.median(levels[(ref, ch)])
            print(f"{name:<10} {t0:9.3f} {t1:9.3f}  {ch:3d}  {np.median(v):10.1f}  {v.max():8.1f}  "
                  f"{100 * np.mean(v > r + a.over_db):6.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
