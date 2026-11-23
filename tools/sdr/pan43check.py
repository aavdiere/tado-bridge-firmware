#!/usr/bin/env python3
"""Check the #43 wake-up PAN test (test image T1) on air.

Receive-only, offline. Inputs: a demod.py --csv file of the test channel from
a plain rtl_sdr capture, its timedcap.py `<file>.times.csv`, and the RTT log
of the test image (`pan43: trial ...` and `tx: datagram ...` lines, `rx: t`
lines in ms since boot).

For every `tx:` attempt of a unicast downlink to the spare device it finds
the bridge Data frame with the same MAC seq (the k-th one on air is attempt
k), the wake-up train in front of it (frames less than --chain-ms apart), the
PAN bytes (frame bytes 2-3) and destination class of each wake-up frame, and
any frame in the 30 ms after the Data frame, in particular the spare's
Enhanced ACK of that seq (CRC good or bad). Then it checks the whole capture
for wake-up frames, beacons or any other frame carrying the test PAN.

Timing resolution: frame times are demod.py sync fits at 600 kS/s (1.667 us
per sample, well under 1 us). RTT `rx: t` values are whole ms; they are
mapped to capture time by a line fitted through the spare's Enhanced ACKs
matched by seq. Host time (UTC) from the times.csv rows, good to a few ms.

    python3 -I tools/sdr/pan43check.py --csv t1.ch16.frames.csv \\
        --times t1.cu8.times.csv --rtt t1.rtt.log [--list]

Addresses are never printed: wake-up destinations show as spare / ffff /
other, senders as bridge / spare / other. Keep the output in ~/ib01-captures/.
"""
import argparse
import collections
import csv
import os
import re
import sys
import time

sys.dont_write_bytecode = True

TEST_PAN = "434e"      # 0x4e43, little-endian on air
NET_PAN = "cdab"       # 0xabcd


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def utc(t):
    return time.strftime("%H:%M:%S", time.gmtime(t)) + f".{int((t % 1) * 1000):03d}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True)
    ap.add_argument("--times", required=True)
    ap.add_argument("--rtt", required=True)
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--chain-ms", type=float, default=10.0)
    ap.add_argument("--list", action="store_true", help="also list every unicast train on air")
    a = ap.parse_args()

    # capture time -> host time (line through the times.csv rows, moved to the earliest arrivals)
    tb, th = [], []
    with open(os.path.expanduser(a.times)) as f:
        for r in csv.DictReader(f):
            tb.append(int(r["byte_offset"]) / 2 / a.fs)
            th.append(float(r["host_time"]))
    n = len(tb)
    mx, my = sum(tb) / n, sum(th) / n
    slope = sum((x - mx) * (y - my) for x, y in zip(tb, th)) / sum((x - mx) ** 2 for x in tb)
    res = [y - (my + slope * (x - mx)) for x, y in zip(tb, th)]
    icpt = my - slope * mx + pct(res, 0.01)
    host = lambda tf: icpt + slope * tf  # noqa: E731
    print(f"capture: {utc(host(0))} to {utc(host(tb[-1]))} UTC, {tb[-1]:.1f} s; "
          f"host clock vs dongle {(slope - 1) * 1e6:+.1f} ppm")

    rows = []
    with open(os.path.expanduser(a.csv)) as f:
        for r in csv.DictReader(f):
            for k in ("t_pre", "t_sync", "t_end", "rssi_dbfs", "snr_db", "off_hz"):
                r[k] = float(r[k])
            r["crc_ok"] = r["crc_ok"] == "True"
            r["seq"] = int(r["seq"]) if r["seq"] not in ("", "None") else None
            r["len"] = int(r["len"])
            rows.append(r)
    rows.sort(key=lambda r: r["t_pre"])
    ok = [r for r in rows if r["crc_ok"]]
    print(f"frames: {len(rows)} ({len(ok)} CRC good)")

    # roles: unicast trains = wake-up rdv=0 then a data frame within 20 ms
    trains = []
    for i, d in enumerate(ok):
        if d["type"] not in ("data", "data+pend", "bcast") or i == 0:
            continue
        p = ok[i - 1]
        if not (p["type"] == "wake-up" and p["extra"] == "rdv=0" and d["t_pre"] - p["t_end"] < 0.02):
            continue
        k, w = i - 1, []
        while k >= 0 and ok[k]["type"] == "wake-up":
            if w and ok[k + 1]["t_pre"] - ok[k]["t_end"] > a.chain_ms / 1e3:
                break
            w.append(ok[k])
            k -= 1
        trains.append(dict(data=d, wakes=w[::-1]))
    uni = [t for t in trains if t["data"]["type"] != "bcast"]
    spare_short = collections.Counter(w["dst"] for t in uni for w in t["wakes"]).most_common(1)[0][0]
    bridge = collections.Counter(t["data"]["src"] for t in uni).most_common(1)[0][0]
    spare = collections.Counter(t["data"]["dst"] for t in uni).most_common(1)[0][0]

    def dcls(d):
        return "spare" if d == spare_short else ("ffff" if d == "ffff" else "other")

    def sender(r):
        if r["type"] == "wake-up":
            return "bridge?"        # wake-up frames carry no source
        if r["src"] == bridge or r["src"][:10] == bridge[:10]:
            return "bridge"
        if r["src"] == spare:
            return "spare"
        return "other" if r["src"] else "?"

    # RTT
    lines = open(os.path.expanduser(a.rtt), errors="replace").read().splitlines()
    tx, rxack, pend = [], [], None
    for ln in lines:
        m = re.search(r"pan43: trial (\d+), (TEST PAN 4e43|network PAN, control after a test-PAN miss abcd|"
                      r"network PAN abcd), seq (\w+) attempt (\d+), short train of (\d+) wake frames: (\w+ ?\w*)", ln)
        if m:
            kind = {"T": "test", "n": "control" if "control" in m.group(2) else "network"}[m.group(2)[0]]
            pend = dict(trial=int(m.group(1)), kind=kind, seq=int(m.group(3), 16), att=int(m.group(4)),
                        res=m.group(6))
            continue
        m = re.search(r"tx: datagram (\d+) seq (\w+) attempt (\d+): (.*?), (\d+) wake frames, (\w+) train", ln)
        if m:
            e = dict(dg=int(m.group(1)), seq=int(m.group(2), 16), att=int(m.group(3)), res=m.group(4),
                     wakes=int(m.group(5)), train=m.group(6), trial=None)
            if pend and pend["seq"] == e["seq"] and pend["att"] == e["att"]:
                e["trial"] = pend
            pend = None
            tx.append(e)
            continue
        m = re.search(r"rx: t (\d+) len (\d+) crc ok rssi (-?[\d.]+) dBm .* hdr 42 ee (\w\w): (.*)$", ln)
        if m:
            rxack.append(dict(t=int(m.group(1)) / 1e3, seq=int(m.group(4), 16), rssi=float(m.group(3)),
                              what=m.group(5)))

    # RTT ms since boot -> capture time, through the spare's Enhanced ACKs matched by seq
    sacks = [r for r in ok if r["type"] == "enh-ack" and r["src"] == spare]
    diffs = [x["t"] - s["t_end"] for x in rxack for s in sacks if s["seq"] == x["seq"]]
    peak = collections.Counter(round(dv, 1) for dv in diffs).most_common(1)[0][0]
    pairs = [(s["t_end"], x["t"]) for x in rxack for s in sacks
             if s["seq"] == x["seq"] and abs(x["t"] - s["t_end"] - peak) < 0.05]
    pmx = sum(p[0] for p in pairs) / len(pairs)
    pmy = sum(p[1] for p in pairs) / len(pairs)
    sl = (sum((p[0] - pmx) * (p[1] - pmy) for p in pairs) / sum((p[0] - pmx) ** 2 for p in pairs)
          if len(pairs) > 1 else 1.0)
    rtt_of = lambda tc: pmy + sl * (tc - pmx)  # noqa: E731
    cap_of = lambda tr: pmx + (tr - pmy) / sl  # noqa: E731
    resid = [(p[1] - rtt_of(p[0])) * 1e3 for p in pairs]
    print(f"RTT clock: {len(pairs)} spare ACKs matched by seq; boot at {utc(host(cap_of(0)))} UTC "
          f"(capture t {cap_of(0):.3f} s); bridge vs dongle {(sl - 1) * 1e6:+.0f} ppm; "
          f"residual {min(resid):+.1f}..{max(resid):+.1f} ms (rx t is whole ms, logged after the frame)")
    print(f"capture covers RTT t {rtt_of(0):.1f} .. {rtt_of(tb[-1]):.1f} s")

    # bridge Data frames per seq, in order -> attempt k
    bdata = collections.defaultdict(list)
    for t in trains:
        bdata[t["data"]["seq"]].append(t)

    def after(d, ms=30):
        return [r for r in rows if d["t_end"] < r["t_pre"] < d["t_end"] + ms / 1e3]

    print("\nper downlink attempt (RTT tx lines; air = this capture, ch16):")
    hdr = ("trial", "kind", "seq", "att", "utc", "PAN air", "wakes air/rtt", "dst", "data", "ACK on air",
           "RTT", "agree")
    print("  " + " | ".join(hdr))
    used = collections.Counter()
    totals = collections.Counter()
    for e in tx:
        if e["dg"] == 65535:
            continue
        used[e["seq"]] += 1
        k = used[e["seq"]] - 1
        cands = bdata.get(e["seq"], [])
        tr = e["trial"]
        lab = (str(tr["trial"]), tr["kind"]) if tr else ("-", "normal")
        rtt_ack = e["res"].startswith("delivered")
        if k >= len(cands):
            print("  " + " | ".join(lab + (f"{e['seq']:02x}", str(e["att"]), "-", "not on air / not captured",
                                           "", "", "", "", e["res"], "n/a")))
            continue
        t = cands[k]
        d = t["data"]
        pans = collections.Counter(w["payload"][4:8] for w in t["wakes"])
        pan_s = ",".join(("4e43" if p == TEST_PAN else "abcd" if p == NET_PAN else p) + f"x{c}"
                         for p, c in pans.items())
        dsts = collections.Counter(dcls(w["dst"]) for w in t["wakes"])
        dst_s = ",".join(f"{k_}x{c}" for k_, c in dsts.items())
        rdv = [int(w["extra"][4:]) for w in t["wakes"]]
        nx = after(d)
        ack = [r for r in nx if r["type"] == "enh-ack" and r["seq"] == d["seq"] and r["src"] == spare]
        bad = [r for r in nx if not r["crc_ok"]]
        other = [r for r in nx if r["crc_ok"] and r not in ack]
        if ack:
            ack_s = f"yes, {(ack[0]['t_pre'] - d['t_end']) * 1e6:.1f} us"
        else:
            ack_s = "no"
            if bad:
                ack_s += f" (CRC-bad frame {', '.join(f'+{(r['t_pre'] - d['t_end']) * 1e3:.2f} ms len {r['len']}' for r in bad)})"
            if other:
                ack_s += " (other: " + ", ".join(f"{r['type']} {sender(r)} +{(r['t_pre'] - d['t_end']) * 1e3:.2f} ms"
                                                 for r in other) + ")"
        exp_pan = None
        if tr:
            exp_pan = TEST_PAN if tr["kind"] == "test" else NET_PAN
        pan_ok = exp_pan is None or set(pans) == {exp_pan}
        agree = (bool(ack) == rtt_ack) and pan_ok and len(t["wakes"]) == e["wakes"]
        totals[(lab[1], "air_ack" if ack else "air_noack")] += 1
        row = lab + (f"{e['seq']:02x}", str(e["att"]), utc(host(d["t_end"])), pan_s,
                     f"{len(t['wakes'])}/{e['wakes']} rdv {max(rdv)}..{min(rdv)}", dst_s,
                     f"{d['type']} len {d['len']} {d['rssi_dbfs']:.1f} dBFS", ack_s, e["res"],
                     "yes" if agree else "NO")
        print("  " + " | ".join(row))
    print("  totals on air: " + ", ".join(f"{k[0]} {k[1]} {v}" for k, v in sorted(totals.items())))

    # whole-capture checks for the test PAN
    print("\nwhole capture, CRC-good frames:")
    wk = [r for r in ok if r["type"] == "wake-up"]
    byp = collections.Counter((("4e43" if r["payload"][4:8] == TEST_PAN else
                                "abcd" if r["payload"][4:8] == NET_PAN else r["payload"][4:8]), dcls(r["dst"]))
                              for r in wk)
    print("  wake-up frames by (PAN, destination): " + ", ".join(f"{k[0]}->{k[1]} {v}" for k, v in sorted(byp.items())))
    test_in_trains = sum(1 for t in uni for w in t["wakes"] if w["payload"][4:8] == TEST_PAN)
    print(f"  test-PAN wake-up frames inside a unicast train to the spare: {test_in_trains}")
    beac = [r for r in ok if r["type"] == "enh-beacon"]
    print(f"  enhanced beacons: {len(beac)}" +
          (", PAN bytes 3-4: " + ", ".join(sorted({r['payload'][6:10] for r in beac})) if beac else ""))
    hit = [r for r in ok if r["type"] != "wake-up" and TEST_PAN in r["payload"][4:12]]
    print(f"  non-wake-up frames with 43 4e in bytes 2-5 (PAN fields): {len(hit)}"
          + ("".join(f"\n    {r['type']} {sender(r)} {utc(host(r['t_pre']))}" for r in hit)))
    anyhit = [r for r in ok if r["type"] != "wake-up"
              and any(r["payload"][i:i + 4] == TEST_PAN for i in range(0, len(r["payload"]) - 3, 2))]
    print(f"  non-wake-up frames with 43 4e anywhere (byte-aligned; ciphertext can match by chance): "
          f"{len(anyhit)}" + "".join(f"\n    {r['type']} {sender(r)} {utc(host(r['t_pre']))} "
                                     f"at byte {[i // 2 for i in range(0, len(r['payload']) - 3, 2) if r['payload'][i:i + 4] == TEST_PAN]}"
                                     f" of {r['len']}" for r in anyhit))
    types = collections.Counter((r["type"], sender(r)) for r in ok)
    print("  by (type, sender): " + ", ".join(f"{k[0]}/{k[1]} {v}" for k, v in sorted(types.items())))

    # RSSI of the spare (dongle) and as the bridge logged it
    print("\nlevels per minute (UTC minute: spare at dongle dBFS mean (n) | bridge at dongle dBFS | spare at bridge dBm, RTT):")
    per = collections.defaultdict(lambda: ([], [], []))
    for r in ok:
        s = sender(r)
        m = int(host(r["t_pre"]) // 60)
        if s == "spare":
            per[m][0].append(r["rssi_dbfs"])
        elif s in ("bridge", "bridge?"):
            per[m][1].append(r["rssi_dbfs"])
    for x in rxack:
        per[int(host(cap_of(x["t"])) // 60)][2].append(x["rssi"])

    def mean(v):
        return f"{sum(v) / len(v):6.1f} ({len(v)})" if v else "     -    "
    for m in sorted(per):
        s, b, r = per[m]
        print(f"  {time.strftime('%H:%M', time.gmtime(m * 60))}: {mean(s)} | {mean(b)} | {mean(r)}")
    if a.list:
        print("\nall unicast trains on air:")
        for t in uni:
            d = t["data"]
            pans = collections.Counter(w["payload"][4:8] for w in t["wakes"])
            print(f"  {utc(host(d['t_end']))} seq {d['seq']:02x} wakes {len(t['wakes'])} PAN {dict(pans)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
