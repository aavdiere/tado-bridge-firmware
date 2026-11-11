#!/usr/bin/env python3
"""Power-versus-time scan of a whole rtl_sdr capture, with every burst checked
against the frames demod.py decoded.

Receive-only. Used to confirm that a bench unit emitted nothing: every energy
burst in the capture's span must be a decoded tado frame (CRC OK or not),
spill-over of a much stronger frame on another channel, or a signal that is
clearly not a CC110L (another symbol rate, another frequency, also present in
captures taken before our firmware existed). A stray STX on a CC110L with an
empty TX FIFO sends preamble until stopped (docs/radio-runtime.md), so any
burst longer than --long-ms is reported as well.

    python3 -I tools/sdr/demod.py --fc 867923700 --channels 20,21,22,23,24,25,26,27,28 \\
        --show-bad --quiet --file ~/ib01-captures/x.cu8 --csv ~/ib01-captures/x.frames.csv
    python3 -I tools/sdr/burstscan.py --fc 867923700 --file ~/ib01-captures/x.cu8 \\
        --frames ~/ib01-captures/x.frames.csv --save ~/ib01-captures/x.spec.npz

Method: non-overlapping Hann-windowed FFTs of --nfft points (2.4 MS/s, 256
points: 9.375 kHz bins, 107 us), averaged in groups of --avg (default 4:
427 us per row). Power per channel = sum of the bins within +-70 kHz of the
tado channel centre (docs/radio.md channel plan). A burst is a run of rows
more than --thresh dB above that channel's median (the noise floor; channels
are idle most of the time). Each row of a burst is explained when it lies
inside a decoded frame on that channel (envelope, +-1 row; CRC-failed sync
hits count, their length from the length byte, capped at 25 ms), or when
another channel is at least --leak-db stronger in the same row (spill-over or
ADC clipping from that stronger signal, which is then judged on its own
channel). For a burst with unexplained rows on a channel that demod.py
covered, the IQ is re-decoded from the start of the burst and from just after
each frame already found inside it: demod.py stops at the first sync word, so
a frame that starts during another one (a collision) is otherwise missed.
What remains is listed with its duration, peak, spectral centroid and width,
and the strongest symbol-transition rate (50 kHz for the tado 50 kbps GFSK).

Narrowband tones (carriers, LO leakage, spurs) are found separately: per
bin, the median over time against the median over all bins.

Captures, CSVs and the optional --save spectrogram stay in ~/ib01-captures/,
never in git.
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy import signal

sys.dont_write_bytecode = True

XTAL_HZ = 26e6
BASE_HZ = XTAL_HZ / 2 ** 16 * 0x213276
SPACING_HZ = XTAL_HZ / 2 ** 18 * (256 + 0xF8) * 2 ** 2
BIT_S = 1 / 50e3
DEMOD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "demod.py")


def chan_hz(n):
    return BASE_HZ + n * SPACING_HZ


def spectrogram(path, fs, nfft, avg, seconds):
    win = np.hanning(nfft).astype(np.float32)
    chunk = nfft * avg * 2048 * 2                         # bytes (I and Q)
    out = []
    total = 0
    limit = int(seconds * fs) * 2 if seconds else None
    with open(path, "rb") as f:
        while True:
            want = chunk if limit is None else min(chunk, limit - total)
            if want <= 0:
                break
            b = f.read(want)
            if len(b) < nfft * avg * 2:
                break
            total += len(b)
            x = np.frombuffer(b, dtype=np.uint8).astype(np.float32) - 127.4
            n = (len(x) // (2 * nfft * avg)) * nfft * avg
            z = (x[0:2 * n:2] + 1j * x[1:2 * n:2]).astype(np.complex64).reshape(-1, nfft)
            p = np.abs(np.fft.fftshift(np.fft.fft(z * win, axis=1), axes=1)) ** 2
            out.append(p.reshape(-1, avg, nfft).mean(axis=1).astype(np.float32))
    return np.concatenate(out)


def runs(mask):
    """(start, end) index pairs of True runs, end exclusive."""
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)))


def frame_span(r):
    """(start, end) of a demod.py CSV row in seconds: envelope if known,
    else preamble start to the end implied by the length byte."""
    on, off = float(r["env_on"]), float(r["env_off"])
    if np.isnan(on):
        on = float(r["t_pre"])
    if np.isnan(off):
        off = float(r["t_sync"]) + min((1 + int(r["len"]) + 2) * 8 * BIT_S, 0.025)
    return on, off


def load_frames(path):
    fr = []
    with open(path) as f:
        for r in csv.DictReader(f):
            on, off = frame_span(r)
            fr.append(dict(ch=int(r["ch"]), on=on, off=off, sync=float(r["t_sync"]),
                           crc=r["crc_ok"] == "True", type=r["type"], length=int(r["len"]),
                           off_hz=float(r["off_hz"]), redecoded=False))
    return fr


def redecode(path, fc, fs, ch, t_start, dur):
    """Run demod.py on an excerpt; return frames with absolute times."""
    s = max(0, int(t_start * fs))
    with open(path, "rb") as f:
        f.seek(2 * s)
        b = f.read(2 * int(dur * fs))
    fd, tmp = tempfile.mkstemp(suffix=".csv", prefix=".burstscan_", dir=os.path.dirname(os.path.abspath(path)))
    os.close(fd)
    subprocess.run([sys.executable, "-I", DEMOD, "--fc", str(fc), "--fs", str(fs), "--channels", str(ch),
                    "--show-bad", "--quiet", "--csv", tmp], input=b, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    got = []
    with open(tmp) as f:
        for r in csv.DictReader(f):
            on, off = frame_span(r)
            t0 = s / fs
            got.append(dict(ch=int(r["ch"]), on=on + t0, off=off + t0, sync=float(r["t_sync"]) + t0,
                            crc=r["crc_ok"] == "True", type=r["type"], length=int(r["len"]),
                            off_hz=float(r["off_hz"]), redecoded=True))
    os.remove(tmp)
    return got


def symbol_rate(path, fc, fs, f_centre, t0, t1):
    """Strongest line of the FM-demodulated transition rate, Hz."""
    s, e = int(t0 * fs), int(t1 * fs)
    with open(path, "rb") as f:
        f.seek(2 * s)
        b = f.read(2 * (e - s))
    x = np.frombuffer(b, dtype=np.uint8).astype(np.float32) - 127.4
    z = x[0::2] + 1j * x[1::2]
    if len(z) < 2048:
        return float("nan")
    z = z * np.exp(-2j * np.pi * (f_centre - fc) / fs * np.arange(len(z)))
    z = signal.lfilter(signal.firwin(129, 120e3, fs=fs), 1, z)[200:]
    fi = np.angle(z[1:] * np.conj(z[:-1]))
    tr = np.abs(np.diff((fi > np.median(fi)).astype(np.float32)))
    S = np.abs(np.fft.rfft(tr - tr.mean())) ** 2
    fr = np.fft.rfftfreq(len(tr), 1 / fs)
    m = (fr > 5e3) & (fr < 300e3)
    return float(fr[m][np.argmax(S[m])])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", required=True)
    ap.add_argument("--fc", type=float, required=True)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--nfft", type=int, default=256)
    ap.add_argument("--avg", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--frames", help="demod.py --csv output for the same capture")
    ap.add_argument("--thresh", type=float, default=10.0, help="burst threshold above noise, dB")
    ap.add_argument("--leak-db", type=float, default=15.0,
                    help="a row is spill-over when another channel is this much stronger")
    ap.add_argument("--explained", type=float, default=0.9,
                    help="fraction of a burst's rows that must be explained")
    ap.add_argument("--long-ms", type=float, default=30.0,
                    help="report any unexplained run longer than this (longest tado frame is about 22 ms)")
    ap.add_argument("--save", help="save the spectrogram (npz, keep outside git)")
    ap.add_argument("--load", help="load a spectrogram saved with --save instead of computing it")
    a = ap.parse_args()

    if a.load:
        S = np.load(a.load)["S"]
    else:
        S = spectrogram(a.file, a.fs, a.nfft, a.avg, a.seconds)
        if a.save:
            np.savez(a.save, S=S)
    dt = a.nfft * a.avg / a.fs
    binhz = a.fs / a.nfft
    fbin = a.fc + (np.arange(a.nfft) - a.nfft // 2) * binhz
    nrows = S.shape[0]
    print(f"# {nrows} rows x {a.nfft} bins: {dt * 1e6:.0f} us per row, {binhz / 1e3:.3f} kHz per bin, "
          f"{nrows * dt:.1f} s, span {fbin[0] / 1e6:.4f}-{fbin[-1] / 1e6:.4f} MHz")

    usable = (np.abs(fbin - a.fc) > 15e3) & (np.abs(fbin - a.fc) < 0.45 * a.fs)
    lo_ch = int(np.ceil((fbin[usable].min() + 70e3 - BASE_HZ) / SPACING_HZ))
    hi_ch = int(np.floor((fbin[usable].max() - 70e3 - BASE_HZ) / SPACING_HZ))
    frames = load_frames(a.frames) if a.frames else []
    demod_chans = sorted({f["ch"] for f in frames})

    # --- Narrowband tones ------------------------------------------------------
    med_t = np.median(S, axis=0)
    floor = np.median(med_t[usable])
    print("\n== narrowband tones: bins whose median over the whole capture is >6 dB above the floor")
    tones = np.flatnonzero(usable & (med_t > floor * 10 ** 0.6))
    for k in tones:
        col = 10 * np.log10(S[:, k] / floor)
        on = col > 6
        first = np.argmax(on) * dt
        last = (len(on) - np.argmax(on[::-1])) * dt
        print(f"  {fbin[k] / 1e6:.4f} MHz: median {10 * np.log10(med_t[k] / floor):5.1f} dB, "
              f"above +6 dB {on.mean() * 100:5.1f}% of rows, first {first:6.2f} s, last {last:6.2f} s")
    if len(tones) == 0:
        print("  none")

    # --- Per-channel power, coverage ------------------------------------------
    chans = list(range(lo_ch, hi_ch + 1))
    P = np.stack([S[:, usable & (np.abs(fbin - chan_hz(ch)) <= 70e3)].sum(axis=1) for ch in chans])
    Pdb = 10 * np.log10(P / np.median(P, axis=1, keepdims=True))
    Pabs = 10 * np.log10(P)

    def coverage(ch, frs):
        cov = np.zeros(nrows, bool)
        for f in frs:
            if f["ch"] == ch:
                cov[max(0, int(f["on"] / dt) - 1):int(f["off"] / dt) + 2] = True
        return cov

    print(f"\n== per-channel bursts (>{a.thresh:.0f} dB above the channel median, +-70 kHz band; "
          f"leak = another channel >= {a.leak_db:.0f} dB stronger)")
    leftovers = []
    n_redecoded = 0
    for i, ch in enumerate(chans):
        cov = coverage(ch, frames)
        stronger = (np.delete(Pabs, i, axis=0) - Pabs[i]).max(axis=0) >= a.leak_db
        above = Pdb[i] > a.thresh
        br = runs(above)
        n_frame = n_leak = n_other = 0
        longest = 0.0
        for s, e in br:
            longest = max(longest, (e - s) * dt)
            ok = cov[s:e] | stronger[s:e]
            if ok.mean() < a.explained and ch in demod_chans:
                # collisions: decode again from the burst start and after each frame inside it
                starts = [s * dt - 0.001] + [f["sync"] + 0.0005 for f in frames
                                             if f["ch"] == ch and s * dt - dt <= f["sync"] <= e * dt]
                for t0 in starts:
                    got = redecode(a.file, a.fc, a.fs, ch, t0, (e - s) * dt + 0.03)
                    new = [g for g in got if not any(abs(g["sync"] - f["sync"]) < 1e-4 for f in frames)]
                    frames.extend(new)
                    n_redecoded += len(new)
                cov = coverage(ch, frames)
                ok = cov[s:e] | stronger[s:e]
            if ok.mean() >= a.explained:
                if cov[s:e].mean() >= stronger[s:e].mean():
                    n_frame += 1
                else:
                    n_leak += 1
                continue
            n_other += 1
            leftovers.append((ch, s, e, float(Pdb[i, s:e].max()), float(1 - ok.mean())))
        print(f"  ch {ch:2d} {chan_hz(ch) / 1e6:9.4f} MHz: {len(br):4d} bursts: {n_frame:4d} decoded frames, "
              f"{n_leak:4d} leak, {n_other:4d} other; longest burst {longest * 1e3:6.1f} ms, "
              f"busy {np.mean(above) * 100:5.2f}%")
    print(f"  frames found by re-decoding collisions: {n_redecoded}")
    rd = [f for f in frames if f["redecoded"]]
    for f in rd:
        print(f"    ch {f['ch']} t {f['sync']:8.4f} len {f['length']:3d} crc {'OK' if f['crc'] else 'BAD'} "
              f"{f['type']:8s} off {f['off_hz']:+7.0f} Hz")

    print("\n== bursts not explained (time s, duration ms, peak dB over the channel floor, "
          "unexplained share, centroid and -10 dB width against the channel centre, transition rate)")
    for ch, s, e, peak, share in leftovers:
        sel = usable & (np.abs(fbin - chan_hz(ch)) <= 100e3)
        spec = np.clip(S[s:e][:, sel].mean(axis=0) - med_t[sel], 0, None)
        fo = fbin[sel] - chan_hz(ch)
        cen = float((spec * fo).sum() / spec.sum()) if spec.sum() > 0 else 0.0
        wide = fo[spec > spec.max() / 10]
        width = (wide.max() - wide.min() + binhz) / 1e3 if len(wide) else 0
        rate = symbol_rate(a.file, a.fc, a.fs, chan_hz(ch) + cen, s * dt, e * dt) if a.file and os.path.exists(a.file) else float("nan")
        flag = "  LONG" if (e - s) * dt * 1e3 > a.long_ms else ""
        print(f"  ch {ch:2d} t {s * dt:8.4f} dur {(e - s) * dt * 1e3:6.2f} ms peak {peak:5.1f} dB "
              f"unexpl {share * 100:3.0f}% centroid {cen / 1e3:+6.1f} kHz width {width:5.1f} kHz "
              f"rate {rate / 1e3:5.1f} kHz{flag}")
    if not leftovers:
        print("  none")


if __name__ == "__main__":
    main()
