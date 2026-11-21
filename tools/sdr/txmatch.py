#!/usr/bin/env python3
"""Match the bridge's own transmissions in the RTT log to frames on the air.

Receive-only, offline. For a hopcap.py run: every transmission the bridge's
RTT log names is looked up among demod.py's CRC-OK frames of the test channel,
and every bridge frame on the air is matched back to an RTT line. Coverage by
view A (the test channel's view) is computed for each transmission, so a frame
that fell into a B visit is told apart from one that is missing.

RTT events, each line prefixed with host time.time() (as hopcap's --rtt-log):
  tx: datagram D seq SS attempt N: <result>, K wake frames, ...  -> K wake-ups
      with seq SS and one Data/broadcast frame with seq SS, ending before the
      line (searched in --tx-lead s before it)
  pair: beacon seq SS ... sent                  -> one Enhanced Beacon, seq SS
  rx: ... hdr xx xx SS: ..., acked              -> one Enhanced ACK, seq SS
  link: 60 s: acks prepared A sent B ..., tx frames F ...  -> per-minute totals

The bridge is the source of the Data frames that follow a rendezvous-0 wake-up
(only the bridge sends wake-up trains on the test channel); --bridge-hash
overrides it. Wake-ups carry no source address, so on the test channel every
wake-up counts as the bridge's. Addresses are printed only as h:xxxxxxxx
(SHA-256, as hoplist.py); the output stays in ~/ib01-captures/.

    python3 -I tools/sdr/txmatch.py --run ~/ib01-captures/pair40/run \\
        --csv A:~/ib01-captures/pair40/run.A.frames.csv --channel 16 \\
        --rtt-log ~/ib01-captures/pair40/rtt.log [--list]

Timing resolution: frame times as demod.py (sync fit at 600 kS/s, well under
1 us); file to stream mapping exact in samples; host time from hopcap's START
line (a few ms); RTT lines come tens of ms (up to about 0.5 s for a frame that
waited for its ACK) after the frame, so events are matched by sequence number
inside a host-time window, not by exact time.
"""
import argparse
import bisect
import csv
import hashlib
import os
import re
import sys
from collections import Counter

sys.dont_write_bytecode = True
FS = 2400000
STEP = 0.00405  # wake-up step, s (about 4.05 ms on the bridge)
SLACK = 0.05    # s a frame may seem to follow its RTT line (host time is good to a few ms)


def h(addr):
    return "h:" + hashlib.sha256(addr.encode()).hexdigest()[:8] if addr else ""


class Segments:
    """As in hoplist.py (copied: python3 -I does not import from the script's directory)."""

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
                row.append(nxt - row[0])
        self.start_host = None
        with open(run + ".log") as f:
            for line in f:
                m = re.match(r"START host=([0-9.]+)", line)
                if m:
                    self.start_host = float(m.group(1))
                    break

    def to_stream(self, tag, t_file):
        s = self.seg[tag]
        x = t_file * FS
        i = bisect.bisect_right([r[0] for r in s], x) - 1
        f0, s0, n = s[max(i, 0)]
        return (s0 + x - f0) / FS

    def windows(self, tag):
        return [(r[1] / FS, (r[1] + r[2]) / FS) for r in self.seg[tag]]

    def covered(self, tag, t0, t1):
        """Fraction of [t0, t1] (stream s) inside the view's stretches."""
        if t1 <= t0:
            return 1.0
        got = sum(max(0.0, min(t1, b) - max(t0, a)) for a, b in self.windows(tag))
        return got / (t1 - t0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--run", required=True, help="hopcap.py --out prefix")
    ap.add_argument("--csv", action="append", default=[], help="TAG:path of a demod.py --csv file")
    ap.add_argument("--channel", type=int, default=16, help="the test channel")
    ap.add_argument("--rtt-log", required=True)
    ap.add_argument("--bridge-hash", default="", help="h:xxxxxxxx of the bridge (default: found)")
    ap.add_argument("--tx-lead", type=float, default=1.6, help="s before a tx line to search")
    ap.add_argument("--list", action="store_true", help="one line per RTT event")
    a = ap.parse_args()

    run = os.path.expanduser(a.run)
    sg = Segments(run)
    t_start = sg.start_host
    frames = []
    for spec in a.csv:
        tag, path = spec.split(":", 1)
        with open(os.path.expanduser(path)) as f:
            for r in csv.DictReader(f):
                if int(r["ch"]) != a.channel or r["crc_ok"] != "True":
                    continue
                st = sg.to_stream(tag, float(r["t_sync"]))
                frames.append({"tag": tag, "st": st, "host": t_start + st, "type": r["type"],
                               "seq": int(r["seq"]) if r["seq"] else None, "len": int(r["len"]),
                               "src": h(r["src"]), "dst": h(r["dst"]), "extra": r["extra"]})
    frames.sort(key=lambda x: x["st"])
    stream_end = max(b for t in ("A", "B") for _, b in sg.windows(t))

    bridge = a.bridge_hash
    if not bridge:
        c = Counter()
        last_rdv0 = -1.0
        for fr in frames:
            if fr["type"] == "wake-up" and fr["extra"] == "rdv=0":
                last_rdv0 = fr["st"]
            elif fr["type"].startswith(("data", "bcast")) and fr["st"] - last_rdv0 < 0.02:
                c[fr["src"]] += 1
        bridge = c.most_common(1)[0][0] if c else ""
    print(f"# bridge {bridge} (source of the frames behind rendezvous-0 wake-ups)")

    def ours(fr):
        # Only the bridge sends wake-up trains and Enhanced Beacons on the test
        # channel. demod.py reads a beacon's source from other bytes than a
        # Data frame's, so its hash differs; the beacon is told by its type.
        return fr["type"] in ("wake-up", "enh-beacon") or fr["src"] == bridge

    used = set()
    events = []
    minutes = []
    with open(os.path.expanduser(a.rtt_log)) as f:
        for line in f:
            m = re.match(r"([0-9.]+) (.*)", line.rstrip())
            if not m:
                continue
            t, text = float(m.group(1)), m.group(2)
            st_line = t - t_start
            if st_line < 0 or st_line > stream_end + 1.0:
                continue
            m1 = re.search(r"tx: datagram (\d+) seq ([0-9a-f]{2}) attempt (\d+): (.*?), (\d+) wake frames", text)
            m2 = re.search(r"pair: beacon seq ([0-9a-f]{2}) to the target sent", text)
            m3 = re.search(r"rx: .* hdr [0-9a-f]{2} [0-9a-f]{2} ([0-9a-f]{2}): .*acked$", text)
            m4 = re.search(r"link: 60 s: acks prepared (\d+) sent (\d+) .*tx frames (\d+)", text)
            if m4:
                minutes.append((st_line, int(m4.group(2)), int(m4.group(3))))
            if m1:
                seq, k = int(m1.group(2), 16), int(m1.group(5))
                cand = [i for i, fr in enumerate(frames) if i not in used and ours(fr)
                        and fr["type"] != "wake-up" and fr["type"] != "enh-ack" and fr["seq"] == seq
                        and st_line - a.tx_lead <= fr["st"] <= st_line + SLACK]
                if cand:
                    di = cand[-1]
                    t1 = frames[di]["st"] + 0.02
                else:
                    di, t1 = None, st_line - 0.02
                t0 = t1 - 0.02 - k * STEP
                wk = [i for i, fr in enumerate(frames) if i not in used and fr["type"] == "wake-up"
                      and fr["seq"] == seq and t0 - 0.01 <= fr["st"] <= t1]
                got = wk + ([di] if di is not None else [])
                used.update(got)
                events.append(("tx", st_line, f"datagram {m1.group(1)} seq {m1.group(2)} att {m1.group(3)} "
                               f"({m1.group(4)})", k + 1, len(got), sg.covered("A", t0, t1), t0, t1))
            elif m2:
                seq = int(m2.group(1), 16)
                cand = [i for i, fr in enumerate(frames) if i not in used and ours(fr)
                        and fr["type"] == "enh-beacon" and fr["seq"] == seq
                        and st_line - 0.5 <= fr["st"] <= st_line + SLACK]
                used.update(cand[-1:])
                t1 = frames[cand[-1]]["st"] if cand else st_line - 0.05
                events.append(("beacon", st_line, f"beacon seq {m2.group(1)}", 1, len(cand[-1:]),
                               sg.covered("A", t1 - 0.02, t1 + 0.002), t1 - 0.02, t1))
            elif m3:
                seq = int(m3.group(1), 16)
                cand = [i for i, fr in enumerate(frames) if i not in used and ours(fr)
                        and fr["type"] == "enh-ack" and fr["seq"] == seq
                        and st_line - 0.6 <= fr["st"] <= st_line + SLACK]
                used.update(cand[-1:])
                t1 = frames[cand[-1]]["st"] if cand else st_line - 0.05
                events.append(("ack", st_line, f"ack seq {m3.group(1)}", 1, len(cand[-1:]),
                               sg.covered("A", t1 - 0.02, t1 + 0.002), t1 - 0.02, t1))

    if a.list:
        print("# kind  rtt_stream_s  event  expected decoded A_coverage  air_stream_s")
        for kind, st, name, exp, got, cov, t0, t1 in events:
            flag = "" if got == exp else ("  <- partly in B" if cov < 0.999 else "  <- MISSING")
            print(f"{kind:<6} {st:9.3f}  {name:<45} {exp:4d} {got:4d}  {100 * cov:5.1f} %  "
                  f"{t0:9.3f}-{t1:9.3f}{flag}")

    exp = Counter()
    got = Counter()
    short = Counter()
    for kind, st, name, e, g, cov, t0, t1 in events:
        exp[kind] += e
        got[kind] += g
        if g < e and cov >= 0.999:
            short[kind] += e - g
    print("# RTT events inside the capture: " + ", ".join(
        f"{k} {exp[k]} expected, {got[k]} decoded" for k in ("tx", "beacon", "ack")))
    print(f"# expected but missing while A covered the whole interval: {dict(short) or 0}")
    tx_min = sum(m[2] for m in minutes)
    ack_min = sum(m[1] for m in minutes)
    print(f"# per-minute RTT totals (link: 60 s lines inside the capture, incl. a first minute "
          f"that may start before it): tx frames {tx_min}, acks sent {ack_min}")
    stray = [fr for i, fr in enumerate(frames) if ours(fr) and i not in used]
    print(f"# bridge frames on the air not matched to an RTT line: {len(stray)}")
    for fr in stray[:40]:
        print(f"  {fr['tag']} {fr['st']:11.6f} {fr['type']:<10} seq {fr['seq']} len {fr['len']} {fr['extra']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
