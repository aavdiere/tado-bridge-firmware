#!/usr/bin/env python3
"""Place each bridge Enhanced ACK inside or outside a TLS handshake (#18).

Receive-only, offline. For the temporary #18 test image, which logs every TLS
handshake step on the bridge's 1 ms tick clock (`hs18: hs N done: TCP t X,
steps t A-B, K (offset+ms):` followed by `hs18: steps <off>+<ms> ...`), and
every received frame as `rx: t <tick> ... hdr 69 ec <seq>: ...`.

Inputs: a demod.py --csv file of a plain rtl_sdr capture (no hopping), the
capture's timedcap.py `<file>.times.csv` (host time per byte offset), and the
RTT log with a host-time prefix (rttlog.py).

1. Every CRC-OK uplink Data frame on --channel (not behind a rendezvous-0
   wake-up, as in ackgap.py) is paired with the Enhanced ACK of the same
   sequence number that follows within 20 ms: gap = Data end (last CRC bit) to
   ACK preamble start, from demod.py's sync fits.
2. Each uplink is matched to the RTT `rx:` line with the same sequence number
   whose host time is 0-1.5 s after the frame's host time (timedcap mapping).
3. The tick of the frame's end on air is estimated from all matches:
   tick_air = t_end_file * 1000 + c, where c is the 5th percentile of
   (rx tick - t_end_file * 1000) (the radio task logs a frame 0-few ms after
   its end). This places frames whose RTT line was lost, and serves as a
   cross-check of the RTT tick.
4. Placement, on the RTT tick when there is one, else on tick_air:
   STEP k  inside handshake step k (net task inside mbedtls_ssl_handshake_step)
   GAP     between two steps of one handshake (waiting for the server)
   TCP     after the TCP connect, before the first step
   OUT     outside every handshake window.
   Step boundaries are good to 1 tick (1 ms): the step's start tick and its
   length rounded to 1 ms.

Timing resolution: gaps as demod.py (600 kS/s after decimation, 1.667 us per
sample; sync start fitted over every bit-boundary zero crossing, good to well
under 1 us). Placement: 1 ms bridge tick; host times to tens of ms.

    python3 -I tools/sdr/hs18place.py --csv run2.frames.csv \\
        --times run2.cu8.times.csv --rtt-log rtt.log [--list]
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
    ap.add_argument("--rtt-log", required=True)
    ap.add_argument("--channel", type=int, default=16)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    # --- capture time -> host time
    tb, th = [], []
    with open(os.path.expanduser(a.times)) as f:
        for r in csv.DictReader(f):
            tb.append(int(r["byte_offset"]) / 2 / a.fs)
            th.append(float(r["host_time"]))

    def host(tf):
        i = bisect.bisect_left(tb, tf)
        if i <= 0:
            return th[0] - (tb[0] - tf)
        if i >= len(tb):
            return th[-1] + (tf - tb[-1])
        x0, x1, y0, y1 = tb[i - 1], tb[i], th[i - 1], th[i]
        return y0 + (y1 - y0) * (tf - x0) / (x1 - x0)

    # --- RTT
    hs, rx = [], []
    cur = None
    with open(os.path.expanduser(a.rtt_log), errors="replace") as f:
        for line in f:
            m = re.match(r"([0-9]+\.[0-9]+) (.*)", line)
            if not m:
                continue
            t, text = float(m.group(1)), m.group(2)
            hm = re.search(r"hs18: hs (\d+) (\w+): TCP t (\d+), steps t (\d+)-(\d+), (\d+)", text)
            if hm:
                cur = dict(n=int(hm.group(1)), how=hm.group(2), tcp=int(hm.group(3)),
                           a=int(hm.group(4)), b=int(hm.group(5)), k=int(hm.group(6)), steps=[], host=t)
                hs.append(cur)
                continue
            sm = re.search(r"hs18: steps((?: \d+\+\d+)+)", text)
            if sm and cur is not None:
                for p in sm.group(1).split():
                    o, ms = p.split("+")
                    cur["steps"].append((cur["a"] + int(o), int(ms)))
                continue
            rm = re.search(r"rx: t (\d+) len (\d+) crc (ok|BAD).* hdr ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}): (.*)", text)
            if rm:
                rx.append(dict(tick=int(rm.group(1)), len=int(rm.group(2)), crc=rm.group(3),
                               fc=rm.group(4) + rm.group(5), seq=int(rm.group(6), 16), what=rm.group(7), host=t))
    hs.sort(key=lambda x: x["a"])

    def place(tick):
        for h in hs:
            lo = min(h["tcp"], h["a"])
            hi = h["b"]
            if h["steps"]:
                hi = max(hi, max(s + ms for s, ms in h["steps"]))
            if lo <= tick <= hi:
                for k, (s, ms) in enumerate(h["steps"]):
                    if s <= tick <= s + ms and ms > 0:
                        return f"hs{h['n']} STEP {k + 1}/{h['k']} ({ms} ms, +{tick - s})", "STEP", ms
                if tick < h["a"]:
                    return f"hs{h['n']} TCP", "TCP", 0
                if not h["steps"]:
                    return f"hs{h['n']} (steps not logged)", "IN?", 0
                return f"hs{h['n']} GAP", "GAP", 0
        return "outside", "OUT", 0

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
        ups.append(dict(d=d, ack=nxt if ack else None, host=host(d["t_end"]),
                        seq=int(d["seq"])))  # demod.py writes seq in decimal

    # --- match uplinks to RTT rx lines (data frames received from the device)
    rxd = [r for r in rx if r["fc"] in ("69ec", "69ee", "61ec", "61ee", "79ec", "79ee") or "server" in r["what"]]
    used = set()
    for u in ups:
        best = None
        for j, r in enumerate(rxd):
            if j in used or r["seq"] != u["seq"]:
                continue
            dt = r["host"] - u["host"]
            if -0.2 <= dt <= 1.5 and (best is None or abs(dt) < abs(best[1])):
                best = (j, dt)
        if best:
            used.add(best[0])
            u["rx"] = rxd[best[0]]
        else:
            u["rx"] = None
    # tick = c + (1 + b) * t_end_ms: least squares, then c moved down to the
    # 5th percentile of the residuals (the log tick never precedes the frame's
    # end). b is the bridge's tick clock against the dongle's sample clock.
    pts = [(u["d"]["t_end"] * 1000, u["rx"]["tick"]) for u in ups if u["rx"]]
    c = b = None
    if len(pts) >= 2:
        mx = sum(p[0] for p in pts) / len(pts)
        my = sum(p[1] for p in pts) / len(pts)
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx if sxx else 1.0
        b = slope - 1.0
        res = [p[1] - (my + slope * (p[0] - mx)) for p in pts]
        c0 = pct(res, 0.05)
        c = my - slope * mx + c0
        rel = [r - c0 for r in res]
    print(f"# hs18 handshakes parsed: {len(hs)} (steps listed for {sum(1 for h in hs if h['steps'])});"
          f" rx lines: {len(rx)}; SDR uplinks: {len(ups)}, with ACK {sum(1 for u in ups if u['ack'])},"
          f" matched to RTT {sum(1 for u in ups if u['rx'])}")
    if c is not None:
        print(f"# tick clock vs capture clock: {b * 1e6:+.1f} ppm; RTT rx tick - fitted air end (ms): {stats(rel)}")

    groups = {}
    for u in ups:
        d, k = u["d"], u["ack"]
        tick_air = c + (1 + b) * d["t_end"] * 1000 if c is not None else None
        tick = u["rx"]["tick"] if u["rx"] else (round(tick_air) if tick_air is not None else None)
        where, cls, sms = place(tick) if tick is not None else ("?", "?", 0)
        where_air, cls_air, _ = place(int(tick_air)) if tick_air is not None else ("?", "?", 0)
        g = (k["t_pre"] - d["t_end"]) * 1e6 if k else None
        ge = (k["env_on"] - d["env_off"]) * 1e6 if k else None
        key = cls if cls != "STEP" else ("STEP>=100ms" if sms >= 100 else "STEP<100ms")
        if g is not None:
            groups.setdefault(key, []).append(g)
            groups.setdefault("ALL", []).append(g)
            if cls != "OUT":
                groups.setdefault("IN (any)", []).append(g)
        else:
            groups.setdefault("NOACK " + key, []).append(0)
        if a.list:
            gs = f"{g:7.1f} us env {ge:6.1f}" if g is not None else "   NO ACK      "
            print(f"t {d['t_end']:9.4f} host {u['host']:.3f} seq {u['seq']:3d} len {int(d['len']):3d} gap {gs} "
                  f"rx-tick {u['rx']['tick'] if u['rx'] else '-':>7} air-tick {tick_air if tick_air is None else round(tick_air):>7} "
                  f"{where}{'' if cls_air == cls else '  [air: ' + where_air + ']'}")
    print("# Data end -> bridge ACK preamble start (us), by placement")
    for k in sorted(groups):
        if k.startswith("NOACK"):
            print(f"{k}: {len(groups[k])} uplinks without an ACK on air")
        else:
            print(f"{k}: {stats(groups[k])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
