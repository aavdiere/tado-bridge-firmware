#!/usr/bin/env python3
"""Place each bridge Enhanced ACK inside or outside forced flash work (#22).

Receive-only, offline. Inputs: a demod.py --csv file of a plain rtl_sdr
capture (no hopping), its timedcap.py `<file>.times.csv`, and the log of
tools/bench/flash22_force.py (`force N ...` written just before the `limit`
write, `done N after ...` at the first poll that saw the firmware move
`limit`, i.e. after the erase, the program and their read-back).

1. Every CRC-OK uplink Data frame on --channel (not behind a rendezvous-0
   wake-up, as in ackgap.py) is paired with the Enhanced ACK of the same
   sequence number that follows within 20 ms: gap = Data end (last CRC bit) to
   ACK preamble start, from demod.py's sync fits. Unanswered uplinks are listed.
2. Capture time -> host time: a least-squares line through the times.csv rows
   (host time at which the chunk ending at a byte offset arrived), moved down to
   the 1st percentile of its residuals, so it follows the earliest arrivals.
   Good to a few ms (USB latency); the slope absorbs the dongle's clock error.
3. A frame is IN a flash window when its end lies in [force, done] host time,
   EDGE when within --edge-ms of either end, else OUT. The window holds the
   wait for a quiet moment, the sector erase, the 8-byte program and their
   read-back; its end is late by up to one poll plus one OpenOCD read
   (--poll 10 ms, about 15 ms).

Timing resolution: gaps as demod.py (600 kS/s after decimation, 1.667 us per
sample; sync start fitted over every bit-boundary zero crossing, good to well
under 1 us). Placement: host time, a few ms plus the window's end tolerance.

    python3 -I tools/sdr/flash22place.py --csv run.frames.csv \\
        --times run.cu8.times.csv --force-log force.log [--list]
Output stays in ~/ib01-captures/; no address is printed.
"""
import argparse
import bisect
import csv
import os
import re
import sys

sys.dont_write_bytecode = True


def stats(v):
    if not v:
        return "n=0"
    s = sorted(v)
    n = len(s)
    mean = sum(s) / n
    sd = (sum((x - mean) ** 2 for x in s) / n) ** 0.5
    med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
    return f"n={n} min {s[0]:.1f} median {med:.1f} max {s[-1]:.1f} mean {mean:.1f} std {sd:.1f}"


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True)
    ap.add_argument("--times", required=True, help="timedcap.py <file>.times.csv")
    ap.add_argument("--force-log", required=True)
    ap.add_argument("--channel", type=int, default=16)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--edge-ms", type=float, default=10.0)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    # --- capture time -> host time (fitted line on the earliest arrivals)
    tb, th = [], []
    with open(os.path.expanduser(a.times)) as f:
        for r in csv.DictReader(f):
            tb.append(int(r["byte_offset"]) / 2 / a.fs)
            th.append(float(r["host_time"]))
    n = len(tb)
    mx, my = sum(tb) / n, sum(th) / n
    sxx = sum((x - mx) ** 2 for x in tb)
    slope = sum((x - mx) * (y - my) for x, y in zip(tb, th)) / sxx
    res = [y - (my + slope * (x - mx)) for x, y in zip(tb, th)]
    c0 = pct(res, 0.01)
    icpt = my - slope * mx + c0
    lag = [r - c0 for r in res]

    def host(tf):
        return icpt + slope * tf

    # --- flash windows
    fire, wins = {}, []
    with open(os.path.expanduser(a.force_log)) as f:
        for line in f:
            m = re.match(r"([0-9.]+) force (\d+) (\w+):", line)
            if m:
                fire[int(m.group(2))] = (float(m.group(1)), m.group(3))
                continue
            m = re.match(r"([0-9.]+) done (\d+) after ([0-9.]+) ms", line)
            if m and int(m.group(2)) in fire:
                t0, why = fire[int(m.group(2))]
                wins.append((t0, float(m.group(1)), int(m.group(2)), why))
    wins.sort()
    starts = [w[0] for w in wins]

    def place(t):
        i = bisect.bisect_right(starts, t + a.edge_ms / 1e3) - 1
        for j in (i, i + 1):
            if 0 <= j < len(wins):
                t0, t1, k, _ = wins[j]
                if t0 - a.edge_ms / 1e3 <= t <= t1 + a.edge_ms / 1e3:
                    inner = min(t - t0, t1 - t) * 1e3
                    return ("IN" if inner >= a.edge_ms else "EDGE"), k, inner
        return "OUT", None, None

    # --- SDR frames
    rows = []
    with open(os.path.expanduser(a.csv)) as f:
        for r in csv.DictReader(f):
            if int(r["ch"]) != a.channel or r["crc_ok"] != "True":
                continue
            for k in ("t_pre", "t_sync", "t_end", "env_on", "env_off"):
                r[k] = float(r[k]) if r[k] not in ("", "nan") else float("nan")
            rows.append(r)
    rows.sort(key=lambda r: r["t_sync"])
    ups = []
    for i, d in enumerate(rows):
        if d["type"] not in ("data", "data+pend"):
            continue
        prev = rows[i - 1] if i else None
        if (prev is not None and prev["type"] == "wake-up" and prev["extra"] == "rdv=0"
                and d["t_pre"] - prev["t_end"] < 0.02):
            continue  # downlink
        nxt = rows[i + 1] if i + 1 < len(rows) else None
        ack = (nxt is not None and nxt["type"] == "enh-ack" and nxt["seq"] == d["seq"]
               and nxt["t_pre"] - d["t_end"] < 0.02)
        ups.append(dict(d=d, ack=nxt if ack else None, host=host(d["t_end"])))
    others = [r for r in rows if r["type"] not in ("data", "data+pend", "enh-ack", "wake-up")]

    print(f"# capture->host: slope-1 {(slope - 1) * 1e6:+.1f} ppm, arrival lag after fit (ms): "
          f"p1 0.0 median {pct(lag, 0.5) * 1e3:.1f} p99 {pct(lag, 0.99) * 1e3:.1f}")
    print(f"# flash windows: {len(wins)}, force->done (ms): "
          f"{stats([(w[1] - w[0]) * 1e3 for w in wins])}")
    in_cap = [w for w in wins if host(0) <= w[0] <= host(tb[-1])]
    busy = sum(w[1] - w[0] for w in in_cap)
    span = host(tb[-1]) - host(0)
    print(f"# windows inside the capture: {len(in_cap)}, {busy:.1f} s of {span:.1f} s "
          f"({100 * busy / span:.2f} %)")
    groups = {"IN": [], "EDGE": [], "OUT": []}
    unanswered = []
    for u in ups:
        where, k, inner = place(u["host"])
        u["where"], u["win"], u["inner"] = where, k, inner
        if u["ack"] is None:
            unanswered.append(u)
            continue
        gap = (u["ack"]["t_pre"] - u["d"]["t_end"]) * 1e6
        u["gap"] = gap
        groups[where].append(gap)
    print(f"# uplinks on ch {a.channel}: {len(ups)}, ACKed {len(ups) - len(unanswered)}, "
          f"unanswered {len(unanswered)}")
    for g in ("IN", "EDGE", "OUT"):
        print(f"{g:4s} Data end -> ACK preamble (us): {stats(groups[g])}")
    allg = groups["IN"] + groups["EDGE"] + groups["OUT"]
    print(f"ALL  Data end -> ACK preamble (us): {stats(allg)}")
    for u in unanswered:
        print(f"UNANSWERED file t {u['d']['t_end']:.4f} host {u['host']:.3f} len {u['d']['len']} "
              f"seq {u['d']['seq']} {u['where']} win {u['win']}")
    oth_in = [(r, place(host(r["t_end"]))) for r in others]
    print("# other CRC-OK frames on ch %d inside a window: %s" % (
        a.channel, ", ".join(f"{r['type']} {r['len']} B" for r, p in oth_in if p[0] == "IN") or "none"))
    if a.list:
        for u in ups:
            print(f"file {u['d']['t_end']:10.4f} host {u['host']:.3f} len {u['d']['len']:>3} "
                  f"seq {u['d']['seq']:>3} gap {u.get('gap', float('nan')):7.1f} {u['where']:4s}"
                  + (f" win {u['win']} {u['inner']:.0f} ms from edge" if u["win"] else ""))


if __name__ == "__main__":
    main()
