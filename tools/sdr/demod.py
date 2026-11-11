#!/usr/bin/env python3
"""Demodulate tado 868 MHz frames with sample-accurate timestamps.

Receive-only. Reads unsigned 8-bit IQ (rtl_sdr format) from a file or from
stdin, mixes one or more tado channels to baseband, FM-demodulates the 50 kbps
GFSK, finds the sync word D391D391, decodes length + frame + radio CRC
(CC110L CRC-16, poly 0x8005, init 0xFFFF, over length and frame, big-endian;
docs/radio.md) and prints one line per frame.

Live, channel 26 (protected channel, receive only), tuned 400 kHz below the
channel so the dongle's DC spike stays out of the band:

    rtl_sdr -f 867923700 -s 2400000 -g 20 - \
        | python3 -I tools/sdr/demod.py --fc 867923700 --channels 26

From a capture (keep captures in ~/ib01-captures/, never in git):

    rtl_sdr -f 867923700 -s 2400000 -g 20 -n 288000000 ~/ib01-captures/ch26.cu8
    python3 -I tools/sdr/demod.py --fc 867923700 --channels 26 \
        --file ~/ib01-captures/ch26.cu8 --summary

For timing measurements prefer the capture-then-decode route: a pipe that
falls behind makes librtlsdr drop samples silently, which breaks the time base.

Per frame it prints:
  t        sync start (first bit after the preamble), seconds from stream start
  pre      preamble start = sync start - detected preamble bits
  end      end of the last CRC bit
  env      envelope rise and fall (half of the frame amplitude), ms relative
           to the sync start and to the end respectively
  len, crc, rssi (dBFS, uncalibrated), snr (dB), off (carrier offset, Hz, the
  midpoint of the two FSK tones), dev (half the tone spacing, Hz), rate (bit
  rate error of the sender, ppm), type, seq, addresses, payload hex.

Timing: the sync start comes from a least-squares fit of every bit-boundary
zero crossing of the demodulated frequency (linear interpolation between
samples at fs/decim), so its precision is well below one sample; see
docs/test-bench.md for the measured spread. Filter delays are removed so
times refer to the raw IQ sample index. --ppm corrects the dongle's crystal
error in both frequency and time (do not also pass -p to rtl_sdr).

--redact hides addresses and payload. --csv writes one row per frame for
later timing analysis (keep it outside git). --summary prints counts, CRC
rate, offset spread and the Data -> Enhanced ACK and wake-up train timing.
"""
import argparse
import collections
import csv
import os
import sys
import time

import numpy as np
from scipy import signal

sys.dont_write_bytecode = True        # no __pycache__ in the repo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frames import BAUD, SYNC, bits_to_bytes, crc16_8005  # noqa: E402

# Channel centres as the stock CC110L programs them (docs/radio.md register
# table): FREQ = 0x213276, CHANSPC_E = 2, CHANSPC_M = 0xF8, 26 MHz crystal.
# Channel 0 is 863.124939 MHz, 61 Hz below the rounded 863.125 MHz.
XTAL_HZ = 26e6
BASE_HZ = XTAL_HZ / 2 ** 16 * 0x213276
SPACING_HZ = XTAL_HZ / 2 ** 18 * (256 + 0xF8) * 2 ** 2


def chan_hz(n):
    return BASE_HZ + n * SPACING_HZ

SYNC_BITS = np.array([(SYNC >> (31 - i)) & 1 for i in range(32)], dtype=np.int8)
SYNC_PM = SYNC_BITS.astype(np.int16) * 2 - 1
MAX_FRAME_BITS = 8 * (1 + 255 + 2)
EUI_SUFFIX = "c51b00"


def hx(b):
    return bytes(b).hex()


def parse_mac(f):
    """Header fields of the tado frame classes (freedo/tado/mac.go:61-86,
    :97-148, :222-229, freedo/tado/beacon.go:80-85). Returns (type, seq, dst, src, extra)."""
    if not f:
        return "empty", None, "", "", ""
    if f[0] == 0x25 and len(f) >= 12:
        return "wake-up", f[1], hx(f[4:6]), "", f"rdv={f[8] | f[9] << 8}"
    if len(f) < 3:
        return f"fcf-{hx(f)}", None, "", "", ""
    fcf = (f[0], f[1])
    if fcf in ((0x69, 0xEC), (0x79, 0xEC)) and len(f) >= 16:
        name = "data" if f[0] == 0x69 else "data+pend"
        return name, f[2], hx(f[3:11]), hx(f[11:16]) + EUI_SUFFIX, ""
    if fcf in ((0x49, 0xE8), (0x59, 0xE8)) and len(f) >= 16:
        name = "bcast" if f[0] == 0x49 else "bcast+pend"
        return name, f[2], hx(f[3:5]), hx(f[5:13]), ""
    if fcf == (0x42, 0xEE) and len(f) >= 29:
        return "enh-ack", f[2], hx(f[3:11]), hx(f[11:19]), f"phase={f[21] | f[22] << 8}"
    if fcf == (0x0B, 0xE8) and len(f) >= 15:
        return "beacon-req", f[2], hx(f[5:7]), hx(f[7:15]), ""
    if fcf == (0x18, 0xEC) and len(f) >= 21:
        return "enh-beacon", f[2], hx(f[5:13]), hx(f[13:21]), ""
    return f"fcf-{hx(f[:2])}", f[2], "", "", ""


class Demod:
    def __init__(self, a):
        self.a = a
        self.fs = a.fs
        self.decim = a.decim or max(1, int(round(a.fs / 600e3)))
        self.fs_d = self.fs / self.decim
        self.sps = self.fs_d / BAUD                     # decimated samples per bit
        ntaps = int(round(64 * self.fs / 2.4e6)) | 1    # odd: integer group delay
        self.taps = signal.firwin(ntaps, a.cutoff, fs=self.fs).astype(np.float32)
        self.D = (ntaps - 1) / 2                        # FIR delay, raw samples
        self.L = int(round(self.sps))                   # bit matched filter length
        self.sync_off = np.round((np.arange(32) + 0.5) * self.sps).astype(int)
        self.min_corr = 32 - 2 * a.max_sync_errors
        self.time_scale = 1.0 / (self.fs * (1 + a.ppm * 1e-6))

    # Index maps from decimated arrays to raw sample index within the buffer.
    def raw_of_ds(self, x):           # d_s index (frequency, bit-filtered)
        return self.decim * (x + self.L / 2) - self.D

    def raw_of_z(self, m):            # z / power index
        return self.decim * m - self.D

    def process(self, iq, t0, ch, lo, hi):
        """Decode frames on channel ch whose sync start lies in raw [lo, hi) of iq."""
        a = self.a
        f_off = chan_hz(ch) - a.fc
        n = (t0 + np.arange(len(iq), dtype=np.float64)) * (f_off / self.fs)
        z = iq * np.exp(-2j * np.pi * (n - np.floor(n))).astype(np.complex64)
        z = signal.upfirdn(self.taps, z, down=self.decim)[: len(iq) // self.decim + 1]
        pw = (z.real ** 2 + z.imag ** 2).astype(np.float32)
        noise = float(np.percentile(pw, 20))      # channel is idle most of the time
        d = np.angle(z[1:] * np.conj(z[:-1])).astype(np.float32)
        ds = np.convolve(d, np.ones(self.L, np.float32) / self.L, mode="valid")
        # Slicing threshold: running mean over 32 bits (tracks carrier offset).
        w = int(32 * self.sps)
        thr = np.convolve(ds, np.ones(w, np.float32) / w, mode="same")
        hard = np.where(ds > thr, 1, -1).astype(np.int8)
        m = len(ds) - self.sync_off[-1] - 1
        if m <= 0:
            return []
        corr = np.zeros(m, np.int16)
        for k, o in enumerate(self.sync_off):
            corr += hard[o:o + m] * SYNC_PM[k]
        cand = np.where(corr >= self.min_corr)[0]
        out = []
        last_end = -1e18
        i = 0
        while i < len(cand):
            j = i
            while j + 1 < len(cand) and cand[j + 1] - cand[j] <= self.L:
                j += 1
            grp = cand[i:j + 1]
            i = j + 1
            best = grp[corr[grp] == corr[grp].max()]
            p0 = int(best[len(best) // 2])          # centre of the best plateau
            r0 = self.raw_of_ds(p0)
            if not (lo <= r0 < hi) or r0 < last_end:
                continue
            fr = self.decode(ds, d, pw, noise, p0, t0, ch)
            if fr is not None:
                out.append(fr)
                last_end = fr["_end_raw_buf"]
        return out

    def decode(self, ds, d, pw, noise, p0, t0, ch):
        a, sps = self.a, self.sps
        nds = len(ds)
        # Carrier offset from the balanced sync word (16 ones, 16 zeros).
        s_lo, s_hi = p0 + self.L // 2, p0 + self.L // 2 + int(32 * sps)
        off = float(np.mean(d[s_lo:s_hi]))

        def sample(pos):
            pos = np.asarray(pos, dtype=np.float64)
            ok = (pos >= 0) & (pos < nds - 1)
            pc = np.clip(pos, 0, nds - 2)
            i = np.floor(pc).astype(int)
            fr = pc - i
            v = ds[i] * (1 - fr) + ds[i + 1] * fr
            return np.where(ok, v, 0.0), ok

        # Length byte with the coarse timing.
        v, ok = sample(p0 + (np.arange(32, 40) + 0.5) * sps)
        if not ok.all():
            return None
        length = bits_to_bytes((v > off).astype(int))[0]
        if length == 0:
            return None
        nbits = 32 + 8 * (1 + length + 2)
        if p0 + nbits * sps >= nds - 1:
            return None

        # Fine timing: fit all bit-boundary zero crossings inside sync + frame.
        seg_hi = int(p0 + nbits * sps) + 1
        y = ds[p0 - self.L: seg_hi] - off if p0 >= self.L else None
        t_start, bit = float(p0), sps
        if y is not None:
            sgn = np.signbit(y)
            zc = np.where(sgn[1:] != sgn[:-1])[0]
            x = zc + y[zc] / (y[zc] - y[zc + 1]) + (p0 - self.L)  # ds index
            # Coarse correction: median crossing residual inside the sync word.
            k = np.round((x - p0) / sps)
            r = x - p0 - k * sps
            in_sync = (k >= 1) & (k <= 32)
            if in_sync.sum() >= 4:
                p0 = p0 + float(np.median(r[in_sync]))
                k = np.round((x - p0) / sps)
                r = x - p0 - k * sps
            keep = (np.abs(r) < sps / 3) & (k >= 0) & (k <= nbits)
            if keep.sum() >= 8:
                kk, xx = k[keep], x[keep]
                A = np.vstack([np.ones_like(kk), kk]).T
                (t_start, bit), *_ = np.linalg.lstsq(A, xx, rcond=None)
                if keep.sum() < 40 or abs(bit / sps - 1) > 2e-3:
                    bit = sps
                    t_start = float(np.mean(xx - kk * sps))
                fit_res = xx - (t_start + kk * bit)
                jitter = float(np.std(fit_res)) if len(fit_res) > 2 else float("nan")
            else:
                jitter = float("nan")
        else:
            jitter = float("nan")

        # Bits with the refined timing.
        v, ok = sample(t_start + (np.arange(nbits) + 0.5) * bit)
        if not ok.all():
            return None
        bits = (v > off).astype(int)
        sync_err = int(np.count_nonzero(bits[:32] != SYNC_BITS))

        # Second timing pass with the decoded bits: keep only crossings at real
        # transitions and model the GFSK pattern shift of each crossing by its
        # outer neighbours, so the fit (start and bit period) is not biased by
        # the data pattern.
        if y is not None and keep.sum() >= 8:
            ext = np.concatenate([np.tile([1, 0], 4), bits])     # 8 preamble bits first
            bpm = ext * 2 - 1
            kk = k[keep].astype(int)
            xx = x[keep]
            kk_e = kk + 8
            valid = (kk_e >= 2) & (kk_e + 1 < len(ext))
            kk, xx, kk_e = kk[valid], xx[valid], kk_e[valid]
            trans = ext[kk_e] != ext[kk_e - 1]
            kk, xx, kk_e = kk[trans], xx[trans], kk_e[trans]
            if len(kk) >= 8:
                s_ = bpm[kk_e]
                f1, f2 = s_ * bpm[kk_e - 2], s_ * bpm[kk_e + 1]
                if len(kk) >= 40:
                    A = np.vstack([np.ones(len(kk)), kk, f1, f2]).T
                    c, *_ = np.linalg.lstsq(A, xx, rcond=None)
                    if abs(c[1] / sps - 1) < 2e-3:
                        t_start, bit = c[0], c[1]
                        jitter = float(np.std(xx - A @ c))
                else:
                    A = np.vstack([np.ones(len(kk)), f1, f2]).T
                    c, *_ = np.linalg.lstsq(A, xx - kk * sps, rcond=None)
                    t_start, bit = c[0], sps
                    jitter = float(np.std(xx - kk * sps - A @ c))
        by = bits_to_bytes(bits[32:])
        length = by[0]
        if len(by) < 3 + length:
            return None
        frame = by[1:1 + length]
        crc_ok = crc16_8005(by[:1 + length]) == (by[1 + length] << 8 | by[2 + length])

        # Tone levels -> offset and deviation. A least-squares fit of the
        # bit-centre samples with neighbour terms removes the data-dependent
        # ISI bias of GFSK: v_k = off + a b_k + c (b_k-1 + b_k+1) + e (b_k-2 + b_k+2).
        # off is the midpoint of the two tones, a + 2c + 2e their half spacing.
        b = bits * 2.0 - 1
        n1 = np.roll(b, 1) + np.roll(b, -1)
        n2 = np.roll(b, 2) + np.roll(b, -2)
        X = np.vstack([np.ones_like(b), b, n1, n2]).T[2:-2]
        coef, *_ = np.linalg.lstsq(X, v[2:-2], rcond=None)
        off = coef[0]
        dev = coef[1] + 2 * coef[2] + 2 * coef[3]
        hz = self.fs_d / (2 * np.pi)
        off_hz = off * hz + chan_hz(ch) * a.ppm * 1e-6
        dev_hz = dev * hz

        # Preamble: count alternating bits before the sync (sync starts with 1).
        pv, pok = sample(t_start - (np.arange(1, 1 + 8 * a.max_preamble) - 0.5) * bit)
        pb = (pv > off).astype(int)
        npre = 0
        for idx in range(len(pb)):
            want = idx % 2   # bit -1 is 0, bit -2 is 1, ...
            if not pok[idx] or pb[idx] != want:
                break
            npre += 1

        # Envelope and power.
        r_sync = self.raw_of_ds(t_start)
        r_end = self.raw_of_ds(t_start + nbits * bit)
        z_sync = (r_sync + self.D) / self.decim
        z_end = (r_end + self.D) / self.decim
        zi0, zi1 = int(z_sync), int(z_end)
        p_frame = float(np.median(pw[zi0:zi1])) if zi1 > zi0 else float("nan")
        thr_p = p_frame / 4           # half amplitude
        env_on = env_off = float("nan")
        k = zi0
        while k > 0 and pw[k] > thr_p:
            k -= 1
        if k > 0 and zi0 - k < (npre + 64) * self.sps:
            env_on = self.raw_of_z(k + (thr_p - pw[k]) / (pw[k + 1] - pw[k]))
        k = max(zi1 - int(4 * self.sps), 0)
        while k < len(pw) - 1 and pw[k] > thr_p:
            k += 1
        if k < len(pw) - 1 and k - zi1 < 64 * self.sps:
            env_off = self.raw_of_z(k - 1 + (pw[k - 1] - thr_p) / (pw[k - 1] - pw[k]))

        ts = self.time_scale
        mtype, seq, dst, src, extra = parse_mac(frame) if crc_ok else ("?", None, "", "", "")
        return {
            "ch": ch,
            "t_sync": (t0 + r_sync) * ts,
            "t_pre": (t0 + self.raw_of_ds(t_start - npre * bit)) * ts,
            "t_end": (t0 + r_end) * ts,
            "env_on": (t0 + env_on) * ts,
            "env_off": (t0 + env_off) * ts,
            "sample_sync": t0 + r_sync,
            "len": length,
            "crc_ok": crc_ok,
            "sync_err": sync_err,
            "npre": npre,
            "rssi_dbfs": 10 * np.log10(p_frame + 1e-20),
            "snr_db": 10 * np.log10(p_frame / noise) if noise > 0 else float("nan"),
            "off_hz": off_hz,
            "dev_hz": dev_hz,
            "rate_ppm": (sps / bit - 1) * 1e6,
            "zc_jitter_us": jitter * self.decim * ts * 1e6,
            "type": mtype,
            "seq": seq,
            "dst": dst,
            "src": src,
            "extra": extra,
            "payload": hx(frame),
            "_end_raw_buf": r_end,
        }


def fmt_frame(f, redact):
    seq = f"{f['seq']:02x}" if f["seq"] is not None else "--"
    s = (f"t={f['t_sync']:13.7f} pre={f['t_pre']:13.7f} end={f['t_end']:13.7f} "
         f"env={(f['env_on'] - f['t_sync']) * 1e3:+7.3f}/{(f['env_off'] - f['t_end']) * 1e3:+6.3f}ms "
         f"ch{f['ch']} len={f['len']:3d} crc={'OK ' if f['crc_ok'] else 'BAD'} "
         f"rssi={f['rssi_dbfs']:6.1f}dBFS snr={f['snr_db']:5.1f} off={f['off_hz']:+7.0f}Hz "
         f"dev={f['dev_hz'] / 1e3:5.1f}k pre={f['npre']:3d}b rate={f['rate_ppm']:+6.0f}ppm "
         f"{f['type']:<10} seq={seq}")
    if not redact:
        if f["dst"]:
            s += f" dst={f['dst']}"
        if f["src"]:
            s += f" src={f['src']}"
        if f["extra"]:
            s += f" {f['extra']}"
        s += f" | {f['payload']}"
    elif f["extra"].startswith("rdv"):
        s += f" {f['extra']}"
    return s


def stats(xs):
    xs = np.asarray(xs, dtype=float)
    if len(xs) == 0:
        return "n=0"
    return (f"n={len(xs)} min={xs.min():.1f} median={np.median(xs):.1f} "
            f"mean={xs.mean():.1f} max={xs.max():.1f} std={xs.std():.1f}")


def summary(frames, dm, dur_s):
    print(f"\n== summary: {dur_s:.1f} s, fs={dm.fs/1e6:.3f} MS/s, decimated {dm.fs_d/1e3:.0f} kS/s "
          f"({1e6/dm.fs_d:.2f} us/sample, {dm.sps:.2f} samples/bit)")
    for ch in sorted({f["ch"] for f in frames}):
        fr = [f for f in frames if f["ch"] == ch]
        ok = [f for f in fr if f["crc_ok"]]
        print(f"ch {ch}: {len(fr)} sync hits, CRC OK {len(ok)} ({100*len(ok)/max(1,len(fr)):.0f}%)")
        print("  types: " + ", ".join(f"{k} {v}" for k, v in collections.Counter(f["type"] for f in ok).most_common()))
        print("  offset Hz, all CRC-OK frames: " + stats([f["off_hz"] for f in ok]))
        print("  deviation kHz: " + stats([f["dev_hz"] / 1e3 for f in ok]))
        print("  preamble bits: " + stats([f["npre"] for f in ok]))
        print("  zero-crossing jitter per frame, us: " + stats([f["zc_jitter_us"] for f in ok]))
        # Per sender (source address, or wake-up/broadcast class), without printing addresses.
        groups = collections.defaultdict(list)
        names = {}
        for f in ok:
            key = f["src"] or ("coordinator(wake-up)" if f["type"] == "wake-up" else "")
            if not key:
                continue
            if key not in names:
                names[key] = key if key.startswith("coord") else f"sender{len(names) + 1}"
            groups[key].append(f)
        for key, g in groups.items():
            offs = [f["off_hz"] for f in g]
            rates = [f["rate_ppm"] for f in g if f["len"] >= 40]
            print(f"  {names[key]:<22} frames {len(g):4d}  offset median {np.median(offs):+7.0f} Hz "
                  f"(spread {np.ptp(offs):.0f})  rssi {np.median([f['rssi_dbfs'] for f in g]):6.1f} dBFS"
                  + (f"  bitrate {np.median(rates):+.0f} ppm" if rates else ""))
        # Wake-up train step.
        wk = [f for f in ok if f["type"] == "wake-up"]
        steps = [(b["t_sync"] - a_["t_sync"]) * 1e6 for a_, b in zip(wk, wk[1:])
                 if (b["t_sync"] - a_["t_sync"]) < 6e-3]
        print("  wake-up step (sync to sync, consecutive), us: " + stats(steps))
        if steps:
            st = np.asarray(steps)
            med = np.median(st)
            print(f"    after removing the median: std {np.std(st - med):.2f} us, "
                  f"95% within +-{np.percentile(np.abs(st - med), 95):.2f} us")
        lastwk = []
        for a_, b in zip(ok, ok[1:]):
            if a_["type"] == "wake-up" and b["type"] != "wake-up" and b["t_pre"] - a_["t_end"] < 0.05:
                lastwk.append((b["t_pre"] - a_["t_end"]) * 1e3)
        print("  last wake-up end -> next frame preamble start, ms: " + stats(lastwk))
        # Data -> Enhanced ACK with the same seq. A Data frame right after a
        # wake-up with rendezvous 0 is a downlink (the coordinator sends it and
        # a device ACKs); any other Data frame is an uplink that the
        # coordinator ACKs.
        gaps = {"uplink, coordinator ACKs": ([], []), "downlink, device ACKs": ([], [])}
        for i in range(1, len(ok) - 1):
            a_, b, prev = ok[i], ok[i + 1], ok[i - 1]
            if not (b["type"] == "enh-ack" and a_["type"] in ("data", "data+pend")
                    and a_["seq"] == b["seq"] and b["t_pre"] - a_["t_end"] < 0.02):
                continue
            down = (prev["type"] == "wake-up" and prev["extra"] == "rdv=0"
                    and a_["t_pre"] - prev["t_end"] < 0.02)
            g, ge = gaps["downlink, device ACKs" if down else "uplink, coordinator ACKs"]
            g.append((b["t_pre"] - a_["t_end"]) * 1e6)
            if np.isfinite(b["env_on"]) and np.isfinite(a_["env_off"]):
                ge.append((b["env_on"] - a_["env_off"]) * 1e6)
        for k, (g, ge) in gaps.items():
            print(f"  {k}: data end -> enh-ack preamble start, us: " + stats(g))
            print(f"  {k}: envelope off -> envelope on, us: " + stats(ge))
        # Beacon request -> Enhanced Beacon.
        bg = [(b["t_pre"] - a_["t_end"]) * 1e6 for a_, b in zip(ok, ok[1:])
              if a_["type"] == "beacon-req" and b["type"] == "enh-beacon" and b["t_pre"] - a_["t_end"] < 0.05]
        print("  beacon-req end -> enh-beacon preamble start, us: " + stats(bg))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fc", type=float, required=True, help="tuned centre frequency, Hz")
    ap.add_argument("--fs", type=float, default=2.4e6, help="sample rate, S/s")
    ap.add_argument("--channels", default="26", help="comma-separated channel numbers")
    ap.add_argument("--file", help="u8 IQ file; default stdin")
    ap.add_argument("--decim", type=int, default=0, help="decimation (default: to about 600 kS/s)")
    ap.add_argument("--cutoff", type=float, default=80e3, help="channel filter cutoff, Hz")
    ap.add_argument("--ppm", type=float, default=0.0, help="dongle crystal error to correct, ppm")
    ap.add_argument("--max-sync-errors", type=int, default=2)
    ap.add_argument("--max-preamble", type=int, default=8, help="preamble bytes to look back for")
    ap.add_argument("--block", type=float, default=0.5, help="seconds per processing block")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this much input")
    ap.add_argument("--show-bad", action="store_true", help="also print frames with a CRC error")
    ap.add_argument("--redact", action="store_true", help="hide addresses and payload")
    ap.add_argument("--quiet", action="store_true", help="no per-frame lines")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--csv", help="write one row per frame (keep outside git)")
    a = ap.parse_args()

    dm = Demod(a)
    chans = [int(c) for c in a.channels.split(",")]
    for c in chans:
        if abs(chan_hz(c) - a.fc) > 0.4 * a.fs:
            ap.error(f"channel {c} is outside the usable span around --fc")

    max_frame_raw = int((64 + MAX_FRAME_BITS) / BAUD * a.fs)
    margin = int(4e-3 * a.fs) + 8 * a.max_preamble * int(a.fs / BAUD)   # before an accepted sync
    tail = max_frame_raw + int(1e-3 * a.fs)                              # after it
    block = int(a.block * a.fs)
    src = open(a.file, "rb") if a.file else sys.stdin.buffer
    wall0 = time.time()
    print(f"# start {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(wall0))}, fc {a.fc/1e6:.4f} MHz, "
          f"fs {a.fs/1e6:.3f} MS/s, decim {dm.decim} ({1e6/dm.fs_d:.3f} us/sample), "
          f"channels {chans}, times in s from stream start", flush=True)

    writer = None
    if a.csv:
        cf = open(a.csv, "w", newline="")
        cols = ["ch", "t_pre", "t_sync", "t_end", "env_on", "env_off", "sample_sync", "len", "crc_ok",
                "sync_err", "npre", "rssi_dbfs", "snr_db", "off_hz", "dev_hz", "rate_ppm",
                "zc_jitter_us", "type", "seq", "dst", "src", "extra", "payload"]
        writer = csv.DictWriter(cf, fieldnames=cols, extrasaction="ignore")
        writer.writeheader()

    try:
        allf, total = run(a, dm, src, chans, writer, margin, tail, block)
    except KeyboardInterrupt:            # Ctrl-C in live mode still prints the summary
        allf, total = PARTIAL["frames"], PARTIAL["total"]
    if a.summary:
        summary(allf, dm, total / a.fs)
    return 0


PARTIAL = {"frames": [], "total": 0}


def run(a, dm, src, chans, writer, margin, tail, block):
    allf = PARTIAL["frames"]
    carry = np.zeros(0, np.complex64)
    t0 = 0              # absolute raw index of buf[0]
    total = 0
    eof = False
    while not eof:
        raw = src.read(block * 2)
        if len(raw) < block * 2:
            eof = True
        raw = raw[: len(raw) // 2 * 2]
        if a.seconds and total + len(raw) // 2 > a.seconds * a.fs:
            raw = raw[: int(a.seconds * a.fs - total) * 2]
            eof = True
        total += len(raw) // 2
        PARTIAL["total"] = total
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 127.5) / 127.5
        buf = np.concatenate([carry, (x[0::2] + 1j * x[1::2]).astype(np.complex64)])
        if len(buf) < margin + 1000:
            break
        lo = margin if t0 > 0 else 0
        hi = len(buf) if eof else len(buf) - tail
        found = []
        for c in chans:
            found += dm.process(buf, t0, c, lo, hi)
        found.sort(key=lambda f: f["t_sync"])
        for f in found:
            allf.append(f)
            if writer:
                writer.writerow(f)
            if not a.quiet and (f["crc_ok"] or a.show_bad):
                print(fmt_frame(f, a.redact), flush=True)
        keep = len(buf) - hi + margin
        carry = buf[-keep:] if not eof else carry
        t0 += len(buf) - keep
    return allf, total


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:              # output piped into head or similar
        sys.stderr.close()
        sys.exit(0)
