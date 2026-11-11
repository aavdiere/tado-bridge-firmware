#!/usr/bin/env python3
"""Detect tado 868 MHz frames on selected channels from a live IQ stream.

Reads unsigned 8-bit IQ from stdin (rtl_sdr output) tuned to --fc. For each
channel in --channels it mixes the channel to baseband, low-pass filters,
FM-demodulates, searches every symbol phase for the sync word D391D391, reads
the length byte and frame, and checks the radio CRC (CRC-16, poly 0x8005,
init 0xFFFF, over length byte and payload, big-endian; docs/radio.md).

Prints per channel: frames with CRC OK, frames with CRC error, frame types
(first frame byte) and the mean carrier offset of CRC-OK frames. It never
prints addresses or payload. Receive-only; nothing is written to disk.

    rtl_sdr -f <centre Hz> -s 2400000 -g 30 -n <samples> - \
        | python3 -I tools/sdr/frames.py --fc <centre Hz> --channels 20,24,26
"""
import argparse
import collections
import sys

import numpy as np
from scipy import signal

CH0_HZ = 863.125e6
CH_STEP_HZ = 199951.0
BAUD = 49987.79
SYNC = 0xD391D391
FRAME_TYPES = {0x69: "data", 0x79: "data+pending", 0x49: "broadcast", 0x59: "broadcast+pending",
               0x42: "enh-ack", 0x25: "wake-up", 0x0B: "beacon-req", 0x18: "enh-beacon"}


def crc16_8005(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bits_to_bytes(bits):
    out = []
    for i in range(0, len(bits) - 7, 8):
        v = 0
        for b in bits[i:i + 8]:
            v = (v << 1) | int(b)
        out.append(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fc", type=float, required=True)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--channels", required=True, help="comma-separated channel numbers")
    ap.add_argument("--decim", type=int, default=4)
    ap.add_argument("--max-sync-errors", type=int, default=2)
    a = ap.parse_args()

    chans = [int(c) for c in a.channels.split(",")]
    fs_d = a.fs / a.decim
    sps = fs_d / BAUD
    taps = signal.firwin(64, 80e3, fs=a.fs)
    sync_bits = np.array([(SYNC >> (31 - i)) & 1 for i in range(32)], dtype=np.int8) * 2 - 1
    min_corr = 32 - 2 * a.max_sync_errors

    block = int(a.fs)                        # 1 s per block
    overlap = int(0.04 * a.fs)               # 40 ms, longer than a 131-byte frame
    stats = {c: {"ok": 0, "bad": 0, "types": collections.Counter(), "offs": []} for c in chans}
    seen = {c: [] for c in chans}            # absolute start times of decoded frames
    carry = np.zeros(0, dtype=np.complex64)
    t0 = 0                                   # absolute sample index of carry[0]
    stdin = sys.stdin.buffer
    while True:
        raw = stdin.read(block * 2)
        if len(raw) < 2:
            break
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 127.5) / 127.5
        iq = np.concatenate([carry, (x[0::2] + 1j * x[1::2]).astype(np.complex64)])
        n_abs = t0 + np.arange(len(iq))
        for c in chans:
            f_off = CH0_HZ + c * CH_STEP_HZ - a.fc
            z = iq * np.exp(-2j * np.pi * f_off * n_abs / a.fs).astype(np.complex64)
            z = signal.lfilter(taps, 1.0, z)[:: a.decim]
            d = np.angle(z[1:] * np.conj(z[:-1]))                 # rad per sample
            d_s = np.convolve(d, np.ones(int(round(sps))) / round(sps), mode="same")
            for ph in range(int(round(sps))):
                idx = np.arange(ph, len(d_s), sps).astype(int)
                if len(idx) < 64:
                    continue
                sym = np.where(d_s[idx] > 0, 1, -1).astype(np.int8)
                corr = np.correlate(sym, sync_bits, mode="valid")
                for pos in np.where(np.abs(corr) >= min_corr)[0]:
                    inv = corr[pos] < 0
                    start = pos + 32
                    b = (sym[start:start + 8 * 140] > 0) ^ inv
                    if len(b) < 24:
                        continue
                    length = bits_to_bytes(b[:8])[0]
                    nbits = 8 * (1 + length + 2)
                    if length == 0 or len(b) < nbits:
                        continue
                    t_abs = t0 + idx[pos] * a.decim
                    if any(abs(t_abs - s) < 0.002 * a.fs for s in seen[c][-50:]):
                        continue
                    by = bits_to_bytes(b[:nbits])
                    ok = crc16_8005(by[:1 + length]) == (by[1 + length] << 8 | by[2 + length])
                    if ok or corr[pos] * (1 if not inv else -1) >= 32:
                        seen[c].append(t_abs)
                        st = stats[c]
                        if ok:
                            st["ok"] += 1
                            st["types"][FRAME_TYPES.get(by[1], f"0x{by[1]:02x}")] += 1
                            seg = d[idx[pos]: idx[pos] + int(nbits * sps)]
                            st["offs"].append(float(np.mean(seg)) * fs_d / (2 * np.pi))
                        else:
                            st["bad"] += 1
        carry = iq[-overlap:]
        t0 += len(iq) - overlap

    print(f"centre {a.fc/1e6:.4f} MHz, decimated {fs_d/1e3:.0f} kS/s, {sps:.2f} samples/symbol")
    for c in chans:
        st = stats[c]
        off = f"{np.mean(st['offs'])/1e3:+.1f} kHz" if st["offs"] else "-"
        types = ", ".join(f"{k} {v}" for k, v in st["types"].most_common())
        print(f"ch {c:>2} {(CH0_HZ + c*CH_STEP_HZ)/1e6:.4f} MHz: CRC ok {st['ok']:>4}, CRC bad {st['bad']:>4}, offset {off}; {types}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
