#!/usr/bin/env python3
"""Enhanced ACK turnaround per frame, against the bridge's TLS activity (#18).

Receive-only, offline. Reads a demod.py --csv file of one hopcap.py view (or of
a plain rtl_sdr capture), pairs every CRC-OK unicast Data frame with the
Enhanced ACK of the same sequence number that follows within --max-gap-ms, and
prints the gap from the Data frame's last CRC bit to the ACK's preamble start
(demod.py's t_end and t_pre), plus the envelope-to-envelope gap.

Direction, as in demod.py --summary: a Data frame that starts within 20 ms of a
wake-up frame with rendezvous 0 is a downlink (the coordinator's train, a device
ACKs it); any other Data frame is an uplink, which the coordinator (on the test
channel: our bridge) ACKs. Uplinks without an ACK are listed, flagged EDGE when
they sit within --edge-ms of a retune (the ACK may have fallen off the stretch).

With --run, file times are mapped to stream and host time through
<run>.segments.csv and the START line of <run>.log (tools/sdr/hoplist.py). With
--rtt-log (each line prefixed with host time.time()), every 'tls: ...
handshake N ms' line gives a window [line - N ms - --rtt-lead, line + --rtt-lag]
in host time, and each pair is tagged TLS when its Data frame falls inside one.
Lines given by --mark (regex, repeatable) add windows of --mark-s seconds
starting at the line, tagged MARK (for example 'session: ready', the server's
first requests after a reconnect). Host time is good to a few ms (hopcap's
START line) plus the RTT line latency (tens of ms), far below the 1 s windows.

Timing resolution: as demod.py, sync start from a least-squares fit of every
bit-boundary zero crossing at 600 kS/s (1.667 us per sample), good to well
under 1 us; envelope edges to one 600 kS/s sample. Intervals scale with the
dongle's crystal error (tens of ppm), negligible here.

Addresses are never printed; senders are hashed as in hoplist.py. Output stays
in ~/ib01-captures/.

    python3 -I tools/sdr/ackgap.py --csv ~/ib01-captures/ack18/run.A.frames.csv \\
        --run ~/ib01-captures/ack18/run --rtt-log ~/ib01-captures/ack18/rtt.log --channel 16
"""
import argparse
import csv
import hashlib
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hoplist import Segments  # noqa: E402


def h(addr):
    return "h:" + hashlib.sha256(addr.encode()).hexdigest()[:8] if addr else "-"


def stats(v):
    if not v:
        return "n=0"
    s = sorted(v)
    n = len(s)
    mean = sum(s) / n
    sd = (sum((x - mean) ** 2 for x in s) / n) ** 0.5
    med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
    p = lambda q: s[min(n - 1, int(round(q * (n - 1))))]
    return (f"n={n} min {s[0]:.1f} p5 {p(0.05):.1f} median {med:.1f} p95 {p(0.95):.1f} "
            f"max {s[-1]:.1f} mean {mean:.1f} std {sd:.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True, help="demod.py --csv file")
    ap.add_argument("--channel", type=int, default=16)
    ap.add_argument("--run", help="hopcap.py --out prefix, for stream and host time")
    ap.add_argument("--view", default="A", help="hopcap view of the csv file")
    ap.add_argument("--rtt-log", help="RTT log with host-time prefix")
    ap.add_argument("--rtt-lead", type=float, default=0.3, help="s added before a handshake window")
    ap.add_argument("--rtt-lag", type=float, default=0.2, help="s added after a handshake window")
    ap.add_argument("--mark", action="append", default=[], help="regex of RTT lines that open a MARK window")
    ap.add_argument("--mark-s", type=float, default=3.0)
    ap.add_argument("--max-gap-ms", type=float, default=20.0)
    ap.add_argument("--edge-ms", type=float, default=30.0)
    ap.add_argument("--list", action="store_true", help="one line per pair")
    a = ap.parse_args()

    rows = []
    with open(os.path.expanduser(a.csv)) as f:
        for r in csv.DictReader(f):
            if int(r["ch"]) != a.channel or r["crc_ok"] != "True":
                continue
            for k in ("t_pre", "t_sync", "t_end", "env_on", "env_off"):
                r[k] = float(r[k]) if r[k] not in ("", "nan") else float("nan")
            rows.append(r)
    rows.sort(key=lambda r: r["t_sync"])

    sg = Segments(os.path.expanduser(a.run)) if a.run else None

    def when(t_file):
        if not sg:
            return t_file, None, 1e9
        st, left = sg.to_stream(a.view, t_file)
        return st, sg.host(st), left

    wins = []  # (host0, host1, tag, text)
    if a.rtt_log:
        with open(os.path.expanduser(a.rtt_log), errors="replace") as f:
            for line in f:
                m = re.match(r"([0-9]+\.[0-9]+) (.*)", line)
                if not m:
                    continue
                t, text = float(m.group(1)), m.group(2)
                hm = re.search(r"tls:.*handshake (\d+) ms", text)
                if hm:
                    wins.append((t - int(hm.group(1)) / 1e3 - a.rtt_lead, t + a.rtt_lag, "TLS", text[:90]))
                for rx in a.mark:
                    if re.search(rx, text):
                        wins.append((t, t + a.mark_s, "MARK", text[:90]))
    for w in wins:
        print(f"# window {w[2]} host {w[0]:.3f}-{w[1]:.3f}: {w[3]}")

    def tags(host):
        if host is None:
            return ""
        return ",".join(sorted({w[2] for w in wins if w[0] <= host <= w[1]}))

    groups = {}
    unacked = []
    for i, d in enumerate(rows):
        if d["type"] not in ("data", "data+pend"):
            continue
        prev = rows[i - 1] if i else None
        down = (prev is not None and prev["type"] == "wake-up" and prev["extra"] == "rdv=0"
                and d["t_pre"] - prev["t_end"] < 0.02)
        nxt = rows[i + 1] if i + 1 < len(rows) else None
        st, host, left = when(d["t_sync"])
        if (nxt is not None and nxt["type"] == "enh-ack" and nxt["seq"] == d["seq"]
                and nxt["t_pre"] - d["t_end"] < a.max_gap_ms / 1e3):
            g = (nxt["t_pre"] - d["t_end"]) * 1e6
            ge = (nxt["env_on"] - d["env_off"]) * 1e6
            tg = tags(host)
            key = ("downlink, device ACKs" if down else "uplink, bridge ACKs") + (f" [{tg}]" if tg else "")
            groups.setdefault(key, ([], []))[0].append(g)
            groups[key][1].append(ge)
            if a.list:
                print(f"{'down' if down else 'up  '} stream {st:10.4f} host {host or 0:.3f} seq {d['seq']:>3} "
                      f"len {int(d['len']):3d} gap {g:8.1f} us env {ge:8.1f} us "
                      f"{h(d['src'])}->{h(nxt['src'])} {tg}")
        elif not down:
            unacked.append((st, host, d, left))

    print("# Data end -> Enhanced ACK preamble start (us), and envelope off -> on (us)")
    for k in sorted(groups):
        g, ge = groups[k]
        print(f"{k}: {stats(g)}")
        print(f"{k} envelope: {stats([x for x in ge if x == x])}")
    print(f"# uplinks without an ACK: {len(unacked)}")
    for st, host, d, left in unacked:
        print(f"  stream {st:10.4f} host {host or 0:.3f} seq {d['seq']:>3} len {int(d['len']):3d} "
              f"src {h(d['src'])} {tags(host)}{' EDGE' if left * 1e3 < a.edge_ms else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
