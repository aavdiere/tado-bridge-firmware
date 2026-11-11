#!/usr/bin/env python3
"""Burst-aware activity scan of the tado 868 MHz channels.

Reads unsigned 8-bit IQ from stdin (rtl_sdr output), measures the power in a
120 kHz band around each tado channel inside the tuned span, and reports per
channel: median (noise) power, peak power, number of bursts and total active
time. Receive-only. Nothing is written to disk.

    rtl_sdr -f <centre Hz> -s 2400000 -g 30 -n <samples> - \
        | python3 -I tools/sdr/chanscan.py --fc <centre Hz>

Channel plan: 863.125 MHz + n x 199.951 kHz, n = 0..49 (docs/radio.md).
"""
import argparse
import sys

import numpy as np

CH0_HZ = 863.125e6
CH_STEP_HZ = 199951.0
N_CHANNELS = 50


def channel_hz(n):
    return CH0_HZ + n * CH_STEP_HZ


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fc", type=float, required=True, help="tuned centre frequency, Hz")
    ap.add_argument("--fs", type=float, default=2.4e6, help="sample rate, S/s")
    ap.add_argument("--nfft", type=int, default=256)
    ap.add_argument("--half-band", type=float, default=60e3, help="Hz either side of channel centre")
    ap.add_argument("--threshold", type=float, default=10.0, help="dB above channel median counted as a burst")
    ap.add_argument("--usable", type=float, default=0.8, help="fraction of the span used (edges roll off)")
    a = ap.parse_args()

    freqs = np.fft.fftshift(np.fft.fftfreq(a.nfft, 1.0 / a.fs)) + a.fc
    chans = [n for n in range(N_CHANNELS) if abs(channel_hz(n) - a.fc) < a.fs / 2 * a.usable]
    # Skip bins next to DC, where the dongle has a spike.
    dc = np.abs(freqs - a.fc) < 2 * a.fs / a.nfft
    bins = {n: np.where((np.abs(freqs - channel_hz(n)) < a.half_band) & ~dc)[0] for n in chans}
    win = np.hanning(a.nfft).astype(np.float32)

    frames_per_block = 512
    block_bytes = a.nfft * 2 * frames_per_block
    traces = {n: [] for n in chans}
    stdin = sys.stdin.buffer
    while True:
        buf = stdin.read(block_bytes)
        if len(buf) < block_bytes:
            break
        x = (np.frombuffer(buf, dtype=np.uint8).astype(np.float32) - 127.5) / 127.5
        iq = (x[0::2] + 1j * x[1::2]).reshape(frames_per_block, a.nfft)
        spec = np.abs(np.fft.fftshift(np.fft.fft(iq * win, axis=1), axes=1)) ** 2
        for n in chans:
            traces[n].append(10 * np.log10(spec[:, bins[n]].sum(axis=1) + 1e-12))

    frame_s = a.nfft / a.fs
    if not chans or not traces[chans[0]]:
        print("no data", file=sys.stderr)
        return 1
    total_s = len(np.concatenate(traces[chans[0]])) * frame_s
    print(f"centre {a.fc/1e6:.4f} MHz, {total_s:.1f} s analysed, frame {frame_s*1e6:.0f} us, threshold +{a.threshold:.0f} dB")
    print(f"{'ch':>3} {'MHz':>9} {'median dB':>9} {'peak dB':>8} {'bursts':>7} {'active ms':>10}")
    for n in chans:
        p = np.concatenate(traces[n])
        med = float(np.median(p))
        on = p > med + a.threshold
        bursts = int(np.count_nonzero(on[1:] & ~on[:-1]) + (1 if on[0] else 0))
        print(f"{n:>3} {channel_hz(n)/1e6:>9.4f} {med:>9.1f} {p.max():>8.1f} {bursts:>7} {on.sum()*frame_s*1e3:>10.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
