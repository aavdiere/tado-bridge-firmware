#!/usr/bin/env python3
"""List demod.py frames of a hopcap.py capture on the stream and host clocks.

Receive-only, offline. hopcap.py writes the samples of each view back to back
(<out>.A.cu8, <out>.B.cu8) and one row per kept stretch to <out>.segments.csv.
demod.py's times are seconds into one view's file; this maps them to the
stream (seconds since hopcap's first buffer) and to host time (hopcap's START
line + stream time), so that frames line up with the RTT log, which carries
host time. Frames whose sync start falls within --edge-ms of a stretch's end
straddle a retune and are flagged.

Addresses are never printed: senders are told apart by the first 8 hex digits
of SHA-256 over the address string (`h:xxxxxxxx`). The output still stays in
~/ib01-captures/: an EUI-64 under the fixed `00 1b c5` prefix has 40 unknown
bits, so a 32-bit hash narrows it to a few hundred candidates. In docs/, name
senders by role (bridge, spare device, coordinator, stock device 1, ...).

    python3 -I tools/sdr/hoplist.py --run ~/ib01-captures/pair16/run \\
        --csv A:~/ib01-captures/pair16/run.A.frames.csv [--csv B:...] \\
        [--channels 16] [--crc-ok] [--coverage]

Timing resolution: as demod.py (sync start from a least-squares fit of the
bit-boundary zero crossings at 600 kS/s, good to well under 1 us); the file to
stream mapping is exact in samples (2.4 MS/s), host time follows the dongle's
sample clock from the START line (the first buffer's arrival, good to a few
ms).
"""
import argparse
import bisect
import csv
import hashlib
import os
import re
import sys

sys.dont_write_bytecode = True
FS = 2400000


def h(addr):
    return "h:" + hashlib.sha256(addr.encode()).hexdigest()[:8] if addr else ""


class Segments:
    def __init__(self, run):
        self.seg = {"A": [], "B": []}
        with open(run + ".segments.csv") as f:
            for r in csv.DictReader(f):
                self.seg[r["tag"]].append([int(r["file_sample0"]), int(r["stream_sample0"])])
        for tag in self.seg:
            path = f"{run}.{tag}.cu8"
            n = os.path.getsize(path) // 2 if os.path.exists(path) else 0
            s = self.seg[tag]
            for i, row in enumerate(s):
                nxt = s[i + 1][0] if i + 1 < len(s) else n
                row.append(nxt - row[0])          # samples kept in this stretch
        self.start_host = None
        try:
            with open(run + ".log") as f:
                for line in f:
                    m = re.match(r"START host=([0-9.]+)", line)
                    if m:
                        self.start_host = float(m.group(1))
                        break
        except OSError:
            pass

    def to_stream(self, tag, t_file):
        """(stream seconds, seconds to the stretch's end) for a file time."""
        s = self.seg[tag]
        x = t_file * FS
        i = bisect.bisect_right([r[0] for r in s], x) - 1
        f0, s0, n = s[max(i, 0)]
        return (s0 + x - f0) / FS, (f0 + n - x) / FS

    def host(self, stream_s):
        return None if self.start_host is None else self.start_host + stream_s

    def coverage(self):
        out = {}
        for tag, s in self.seg.items():
            out[tag] = sum(r[2] for r in s) / FS
        last = max((r[1] + r[2] for s in self.seg.values() for r in s), default=0)
        return out, last / FS

    def windows(self, tag):
        return [(r[1] / FS, (r[1] + r[2]) / FS) for r in self.seg[tag]]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--run", required=True, help="hopcap.py --out prefix")
    ap.add_argument("--csv", action="append", default=[], help="TAG:path of a demod.py --csv file")
    ap.add_argument("--channels", default="", help="only these channels")
    ap.add_argument("--crc-ok", action="store_true", help="only CRC-OK frames")
    ap.add_argument("--edge-ms", type=float, default=30.0)
    ap.add_argument("--coverage", action="store_true", help="print time per view and the B windows")
    a = ap.parse_args()

    sg = Segments(os.path.expanduser(a.run))
    if a.coverage:
        cov, total = sg.coverage()
        print(f"# stream {total:.3f} s: " + ", ".join(f"{k} {v:.3f} s ({100 * v / total:.1f} %)"
                                                    for k, v in cov.items()))
        for t0, t1 in sg.windows("B"):
            print(f"# B {t0:9.3f}-{t1:9.3f} s" + (f" host {sg.host(t0):.3f}-{sg.host(t1):.3f}"
                                                   if sg.start_host else ""))
    chans = {int(c) for c in a.channels.split(",") if c}
    rows = []
    for spec in a.csv:
        tag, path = spec.split(":", 1)
        with open(os.path.expanduser(path)) as f:
            for r in csv.DictReader(f):
                if chans and int(r["ch"]) not in chans:
                    continue
                ok = r["crc_ok"] == "True"
                if a.crc_ok and not ok:
                    continue
                st, left = sg.to_stream(tag, float(r["t_sync"]))
                rows.append((st, tag, r, ok, left))
    rows.sort(key=lambda x: x[0])
    print("# view ch stream_s host_s len crc rssi_dBFS off_Hz rate_ppm type seq src dst extra")
    for st, tag, r, ok, left in rows:
        hs = sg.host(st)
        flag = " EDGE" if left * 1e3 < a.edge_ms else ""
        print(f"{tag} ch{int(r['ch']):<2} {st:11.6f} {hs if hs else 0:.4f} {int(r['len']):3d} "
              f"{'OK ' if ok else 'BAD'} {float(r['rssi_dbfs']):6.1f} {float(r['off_hz']):+7.0f} "
              f"{float(r['rate_ppm']):+6.0f} {r['type']:<10} {r['seq'] or '--':>3} "
              f"{h(r['src']) or '-':<10} {h(r['dst']) or '-':<10} {r['extra'] if r['extra'].startswith(('rdv', 'phase')) else ''}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
