#!/usr/bin/env python3
"""Check every bridge downlink against forced flash work (#22 rerun, worst case).

Receive-only, offline. Inputs: a demod.py --csv file of a plain rtl_sdr
capture, its timedcap.py `<file>.times.csv`, the log of
tools/bench/flash22_force.py (with --csl-state, so each `force` line names the
coordinator state at the `limit` write), and the rttlog.py file.

1. On air: each bridge Data frame behind a rendezvous-0 wake-up (a downlink or
   a broadcast) gets the length of its train, counted back over wake-up frames
   less than --chain-ms apart, and the device's Enhanced ACK of the same seq
   within 20 ms (none for a broadcast).
2. RTT: every `tx: datagram ... attempt N: <result>, W wake frames, <train>`
   line, and every `no ack` line.
3. Per force: the RTT tx lines from the `limit` write to --after s after its
   `done` line, and the on-air trains whose Data frame ends in the same span
   (host time, as in flash22place.py: a line fitted through the times.csv rows,
   moved to the earliest arrivals).

Timing resolution: frame times from demod.py sync fits at 600 kS/s (1.667 us
per sample, well under 1 us); host-time placement good to a few ms, plus the
force log's poll (10 ms) and one OpenOCD read.

    python3 -I tools/sdr/flash22down.py --csv run.ch16.frames.csv \\
        --times run.cu8.times.csv --force-log force.log --rtt rtt.log [--list]
Output stays in ~/ib01-captures/; no address is printed.
"""
import argparse
import collections
import csv
import os
import re
import sys

sys.dont_write_bytecode = True


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True)
    ap.add_argument("--times", required=True)
    ap.add_argument("--force-log", required=True)
    ap.add_argument("--rtt", required=True)
    ap.add_argument("--channel", type=int, default=16)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--chain-ms", type=float, default=10.0)
    ap.add_argument("--after", type=float, default=0.5)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    # capture time -> host time, as flash22place.py
    tb, th = [], []
    with open(os.path.expanduser(a.times)) as f:
        for r in csv.DictReader(f):
            tb.append(int(r["byte_offset"]) / 2 / a.fs)
            th.append(float(r["host_time"]))
    n = len(tb)
    mx, my = sum(tb) / n, sum(th) / n
    slope = (sum((x - mx) * (y - my) for x, y in zip(tb, th))
             / sum((x - mx) ** 2 for x in tb))
    res = [y - (my + slope * (x - mx)) for x, y in zip(tb, th)]
    icpt = my - slope * mx + pct(res, 0.01)
    host = lambda tf: icpt + slope * tf  # noqa: E731
    cap0, cap1 = host(0), host(tb[-1])

    # on-air trains
    rows = []
    with open(os.path.expanduser(a.csv)) as f:
        for r in csv.DictReader(f):
            if int(r["ch"]) != a.channel or r["crc_ok"] != "True":
                continue
            for k in ("t_pre", "t_end"):
                r[k] = float(r[k])
            rows.append(r)
    rows.sort(key=lambda r: r["t_pre"])
    trains = []
    for i, d in enumerate(rows):
        if d["type"] not in ("data", "data+pend", "bcast") or i == 0:
            continue
        p = rows[i - 1]
        if not (p["type"] == "wake-up" and p["extra"] == "rdv=0" and d["t_pre"] - p["t_end"] < 0.02):
            continue
        k, wakes, t_first = i - 1, 0, p["t_pre"]
        while k >= 0 and rows[k]["type"] == "wake-up":
            if wakes and rows[k + 1]["t_pre"] - rows[k]["t_end"] > a.chain_ms / 1e3:
                break
            wakes += 1
            t_first = rows[k]["t_pre"]
            k -= 1
        nxt = rows[i + 1] if i + 1 < len(rows) else None
        acked = (nxt is not None and nxt["type"] == "enh-ack" and nxt["seq"] == d["seq"]
                 and nxt["t_pre"] - d["t_end"] < 0.02)
        bcast = d["type"] == "bcast"
        trains.append(dict(host=host(d["t_end"]), start=host(t_first), wakes=wakes, seq=d["seq"],
                           len=int(d["len"]), acked=acked, bcast=bcast,
                           ack_us=(nxt["t_pre"] - d["t_end"]) * 1e6 if acked else None))

    # RTT
    txl, noack = [], []
    with open(os.path.expanduser(a.rtt), errors="replace") as f:
        for line in f:
            m = re.match(r"([0-9.]+) tx: datagram (\d+) seq (\w+) attempt (\d+): (.*?), (\d+) wake frames, (\w+) train", line)
            if m:
                txl.append(dict(t=float(m.group(1)), dg=int(m.group(2)), att=int(m.group(4)),
                                res=m.group(5), wakes=int(m.group(6)), train=m.group(7)))
            if "no ack" in line:
                noack.append(line.strip())

    # forces
    forces = {}
    with open(os.path.expanduser(a.force_log)) as f:
        for line in f:
            m = re.match(r"([0-9.]+) force (\d+) (\S+): .* csl (\w+) hold (\d+)", line)
            if m:
                forces[int(m.group(2))] = dict(t0=float(m.group(1)), why=m.group(3),
                                              csl=m.group(4), hold=int(m.group(5)))
                continue
            m = re.match(r"([0-9.]+) done (\d+) after ([0-9.]+) ms.*csl seen (\S+), hold first seen (.*?), after: csl (\w+) hold (\d+)", line)
            if m and int(m.group(2)) in forces:
                forces[int(m.group(2))].update(t1=float(m.group(1)), ms=float(m.group(3)),
                                               seen=m.group(4), hold_after=int(m.group(7)))

    print(f"# capture host span {cap0:.3f}-{cap1:.3f}")
    full = [x["wakes"] for x in trains if x["wakes"] > 100]
    short = [x["wakes"] for x in trains if x["wakes"] <= 100]
    print(f"# on-air trains: {len(trains)} ({sum(x['bcast'] for x in trains)} broadcast); "
          f"short: {len(short)} wake counts {dict(sorted(collections.Counter(short).items()))}; "
          f"full: {len(full)} wake counts {dict(sorted(collections.Counter(full).items()))}")
    uni = [x for x in trains if not x["bcast"]]
    print(f"# unicast downlinks on air: {len(uni)}, device ACKed {sum(x['acked'] for x in uni)}; "
          f"ACK gap (us) min {min((x['ack_us'] for x in uni if x['acked']), default=0):.1f} "
          f"median {pct([x['ack_us'] for x in uni if x['acked']] or [0], 0.5):.1f} "
          f"max {max((x['ack_us'] for x in uni if x['acked']), default=0):.1f}")
    inc = [x for x in txl if cap0 <= x["t"] <= cap1]
    print(f"# RTT tx lines: {len(txl)} ({len(inc)} inside the capture): results "
          f"{dict(collections.Counter((x['att'], x['res'], x['train']) for x in txl))}")
    print(f"# RTT short-train wake counts: "
          f"{dict(sorted(collections.Counter(x['wakes'] for x in txl if x['train'] == 'short').items()))}"
          f"; full: {dict(sorted(collections.Counter(x['wakes'] for x in txl if x['train'] == 'full').items()))}")
    print(f"# RTT `no ack` lines: {len(noack)}")
    for line in noack:
        print("  " + line)

    bad = 0
    if not forces:
        print("# no force lines with a coordinator state (flash22_force.py --csl-state)")
        return
    states = collections.Counter(f["csl"] for f in forces.values())
    print(f"# forces: {len(forces)}, coordinator state at the write: {dict(states)}; "
          f"write->limit moved (ms): min {min(f.get('ms', 0) for f in forces.values()):.0f} "
          f"median {pct([f.get('ms', 0) for f in forces.values()], 0.5):.0f} "
          f"max {max(f.get('ms', 0) for f in forces.values()):.0f}; "
          f"hold on after done: {sum(f.get('hold_after', 0) for f in forces.values())}")
    for k in sorted(forces):
        f = forces[k]
        t0, t1 = f["t0"], f.get("t1", f["t0"]) + a.after
        rt = [x for x in txl if t0 <= x["t"] <= t1]
        air = [x for x in trains if t0 <= x["host"] <= t1]
        incap = cap0 <= t0 <= cap1
        ok = (rt and all(x["att"] == 1 and x["res"] == "delivered"
                         and (19 <= x["wakes"] <= 23 or x["train"] == "full") for x in rt)
              and (not incap or (air and all((x["acked"] or x["bcast"])
                                             and (19 <= x["wakes"] <= 23 or x["wakes"] > 240)
                                             for x in air))))
        bad += not ok
        if a.list or not ok:
            print(f"{'ok ' if ok else 'BAD'} force {k:3d} {f['why']:10s} csl {f['csl']:6s} "
                  f"{f.get('ms', float('nan')):6.0f} ms seen {f.get('seen', '?'):24s} | RTT "
                  + "; ".join(f"att {x['att']} {x['res']} {x['wakes']} {x['train']}" for x in rt)
                  + " | air " + ("; ".join(f"{x['wakes']} wakes {'B' if x['bcast'] else ('ACK' if x['acked'] else 'NOACK')}"
                                           for x in air) if incap else "(outside capture)"))
    print(f"# forces failing the check: {bad}")


if __name__ == "__main__":
    main()
