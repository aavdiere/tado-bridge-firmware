#!/usr/bin/env python3
"""Close look at one stretch of an rtl_sdr capture around one frequency.

Receive-only. For a signal burstscan.py could not explain: finds the pulses
in the window and, per pulse, gives its edges, length, level, carrier, the
instantaneous-frequency spread and slope (tone, sweep or FSK) and its -10 dB
spectral width. Optionally writes a spectrogram PNG.

    python3 -I tools/sdr/burstzoom.py --file ~/ib01-captures/x.cu8 --fc 867923700 \\
        --f 868490000 --t0 5.00 --t1 5.15 --png ~/ib01-captures/x_zoom.png

Method: mix --f to 0 Hz, low-pass +-(--bw / 2) with a 255-tap FIR at the
capture rate (2.4 MS/s; delay removed, so times refer to the raw sample
index), keep the full rate for edges (0.417 us per sample). Envelope = power
smoothed over --smooth-us. Pulse = run more than --on-db over the window's
median power, gaps shorter than --merge-us closed. Edges: first and last
sample at half the pulse's peak power (-3 dB), linearly interpolated, so
edge times are good to about the smoothing length. Instantaneous frequency
= phase difference per sample, averaged over 10 us, over the pulse's middle
80 %. Spectrum: zero-padded FFT of the pulse, peak and -10 dB width.

Captures and PNGs stay in ~/ib01-captures/, never in git.
"""
import argparse
import sys

import numpy as np
from scipy import signal

sys.dont_write_bytecode = True


def read_iq(path, fs, t0, t1):
    s, e = max(0, int(t0 * fs)), int(t1 * fs)
    with open(path, "rb") as f:
        f.seek(2 * s)
        b = f.read(2 * (e - s))
    x = np.frombuffer(b, dtype=np.uint8).astype(np.float32) - 127.4
    return s, (x[0::2] + 1j * x[1::2]).astype(np.complex64)


def runs(mask):
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", required=True)
    ap.add_argument("--fc", type=float, required=True)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--f", type=float, required=True, help="frequency to centre on, Hz")
    ap.add_argument("--bw", type=float, default=250e3, help="filter width, Hz")
    ap.add_argument("--t0", type=float, required=True)
    ap.add_argument("--t1", type=float, required=True)
    ap.add_argument("--on-db", type=float, default=6.0)
    ap.add_argument("--smooth-us", type=float, default=20.0)
    ap.add_argument("--merge-us", type=float, default=100.0)
    ap.add_argument("--min-us", type=float, default=50.0)
    ap.add_argument("--png")
    a = ap.parse_args()

    fs = a.fs
    pad = 0.002
    s0, z = read_iq(a.file, fs, a.t0 - pad, a.t1 + pad)
    n = np.arange(len(z))
    z = z * np.exp(-2j * np.pi * (a.f - a.fc) / fs * (n + s0)).astype(np.complex64)
    h = signal.firwin(255, a.bw / 2, fs=fs)
    z = signal.lfilter(h, 1, z)[127:]
    t = (s0 + np.arange(len(z))) / fs
    keep = (t >= a.t0) & (t < a.t1)
    z, t = z[keep], t[keep]

    k = max(1, int(a.smooth_us * 1e-6 * fs))
    p = np.convolve(np.abs(z) ** 2, np.ones(k) / k, mode="same")
    floor = np.median(p)
    pdb = 10 * np.log10(p / floor)
    on = pdb > a.on_db
    m = int(a.merge_us * 1e-6 * fs)
    r = runs(on)
    merged = []
    for s, e in r:
        if merged and s - merged[-1][1] < m:
            merged[-1][1] = e
        else:
            merged.append([s, e])
    pulses = [(s, e) for s, e in merged if (e - s) / fs * 1e6 >= a.min_us]

    print(f"# {a.file}: {a.t0:.4f}-{a.t1:.4f} s around {a.f / 1e6:.4f} MHz, filter +-{a.bw / 2e3:.0f} kHz, "
          f"{fs / 1e6:.1f} MS/s ({1e6 / fs:.3f} us/sample), smoothing {a.smooth_us:.0f} us, "
          f"edges at -3 dB of each pulse's peak; floor {10 * np.log10(floor):.1f} dB (raw)")
    print("#  start s      len ms  gap ms   peak dB  carrier kHz  if-std kHz  slope kHz/ms  "
          "if p5..p95 kHz   -10dB width kHz")
    prev = None
    rows = []
    for s, e in pulses:
        pk = p[s:e].max()
        half = pk / 2
        a_i = s + np.argmax(p[s:e] >= half)
        b_i = e - 1 - np.argmax(p[s:e][::-1] >= half)

        def interp(i, j):
            if j < 0 or j >= len(p) or p[i] == p[j]:
                return t[i]
            return t[i] + (half - p[i]) / (p[j] - p[i]) * (t[j] - t[i])
        t_on = interp(a_i, a_i - 1)
        t_off = interp(b_i, b_i + 1)
        L = b_i - a_i
        mid = slice(a_i + L // 10, b_i - L // 10)
        seg = z[mid]
        fi = np.angle(seg[1:] * np.conj(seg[:-1])) * fs / (2 * np.pi)
        w = max(1, int(10e-6 * fs))
        fi = np.convolve(fi, np.ones(w) / w, mode="valid") if len(fi) > w else fi
        tt = np.arange(len(fi)) / fs
        slope = np.polyfit(tt, fi, 1)[0] / 1e6 if len(fi) > 10 else float("nan")  # kHz per ms
        nf = 1 << int(np.ceil(np.log2(max(len(seg), 1) * 8)))
        S = np.abs(np.fft.fftshift(np.fft.fft(seg * np.hanning(len(seg)), nf))) ** 2
        fr = np.fft.fftshift(np.fft.fftfreq(nf, 1 / fs))
        sel = np.abs(fr) < a.bw / 2
        S, fr = S[sel], fr[sel]
        car = fr[np.argmax(S)]
        wide = fr[S > S.max() / 10]
        width = wide.max() - wide.min()
        gap = (t_on - prev) * 1e3 if prev is not None else float("nan")
        prev = t_on
        rows.append((t_on, t_off, car))
        print(f"  {t_on:10.6f}  {(t_off - t_on) * 1e3:7.3f}  {gap:7.3f}  {10 * np.log10(pk / floor):6.1f}  "
              f"{car / 1e3:+9.2f}{np.std(fi) / 1e3:8.2f}    {slope:+9.2f}    "
              f"{np.percentile(fi, 5) / 1e3:+6.1f}..{np.percentile(fi, 95) / 1e3:+6.1f}   {width / 1e3:8.2f}")
    if not pulses:
        print("  no pulse above the threshold")
    else:
        cars = np.array([c for _, _, c in rows])
        print(f"# {len(pulses)} pulses; carrier median {(a.f + np.median(cars)) / 1e6:.5f} MHz "
              f"(offset {np.median(cars) / 1e3:+.2f} kHz from --f), relative to the dongle's uncalibrated clock")

    if a.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        nfft = 512
        f_, t_, Sx = signal.spectrogram(z, fs=fs, nperseg=nfft, noverlap=nfft // 2, return_onesided=False,
                                        window="hann", mode="psd")
        Sx = np.fft.fftshift(Sx, axes=0)
        f_ = np.fft.fftshift(f_)
        sel = np.abs(f_) < a.bw / 2
        fig, ax = plt.subplots(2, 1, figsize=(12, 7), sharex=True,
                               gridspec_kw=dict(height_ratios=[2, 1]))
        db = 10 * np.log10(Sx[sel] + 1e-12)
        ax[0].pcolormesh(t_ + t[0], (a.f + f_[sel]) / 1e6, db, shading="auto",
                         vmin=np.median(db), vmax=np.median(db) + 25, cmap="viridis")
        ax[0].set_ylabel("MHz")
        ax[0].set_title(f"{a.f / 1e6:.4f} MHz +-{a.bw / 2e3:.0f} kHz, nfft {nfft} "
                        f"({fs / nfft / 1e3:.2f} kHz, {nfft / fs * 1e6:.0f} us)")
        ax[1].plot(t, pdb, lw=0.5)
        ax[1].set_ylabel("dB over median")
        ax[1].set_xlabel("s from capture start")
        fig.tight_layout()
        fig.savefig(a.png, dpi=110)
        print(f"# wrote {a.png}")


if __name__ == "__main__":
    main()
