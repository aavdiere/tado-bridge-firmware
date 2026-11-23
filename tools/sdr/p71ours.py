#!/usr/bin/env python3
"""Our frames on the stock channel: rule-3 check and timing (P7.1, #20).

Receive-only, offline. Reads a demod.py --csv file of one plain capture of
channel 26 (tools/sdr/timedcap.py, demod.py --channels 26 --redact --csv) and
checks every frame of ours against abort rule 3 of docs/test-bench.md
("P7.1 channel 26, plan"), and measures our timing:

  trains   our wake-up trains (median step within --step-us +-15 us, or
           followed within 20 ms by a Data or broadcast frame of the bridge):
           count, frames, step statistics, the short address they carry
           (spare, ffff, or OTHER = a rule-3 breach)
  down     each Data frame of the bridge: its destination (spare, or OTHER =
           breach) and the spare's Enhanced ACK (same seq, ACK source = the
           spare, preamble within 20 ms of the Data end), with the gap
  up       each Data frame of the spare to the bridge and the bridge's
           Enhanced ACK: Data end (last CRC bit) to ACK preamble start
  acks     every Enhanced ACK whose source is the bridge must follow a CRC-OK
           Data frame of the spare to the bridge with the same seq
  stock    every Enhanced ACK, Data or broadcast frame that follows a stock
           Data frame within 25 ms and carries the bridge's or the spare's
           address, and every Enhanced ACK after a stock Data frame that
           comes earlier than any stock ACK does (below --fast-us) or has the
           bridge's carrier offset and level: an answer of ours to a stock
           frame
  beacons  (--beacons) every Enhanced Beacon: the Beacon Request before it
           (spare or stock), request end to beacon preamble; and every stock
           Beacon Request: whether any beacon followed within 50 ms

Roles: the bridge from --ours-ref (the source of Data frames after a
rendezvous-0 wake-up on our test channel), the spare from --spare-cmake
(PAIR_TARGET_EUI64 of the gitignored config/local.cmake), the coordinator as in
stockscore.py. Other senders are named by an 8-hex SHA-256 prefix. No address
is printed.

Timing resolution: as demod.py (2.4 MS/s decimated to 600 kS/s, 1.667 us per
sample; sync start from a least-squares fit of every bit-boundary zero
crossing, well under 1 us; preamble start and last CRC bit derived from it).
Gaps are t_pre of the answer minus t_end of the frame answered.

    python3 -I tools/sdr/p71ours.py --csv ~/ib01-captures/p71/runA1.frames.csv \\
        --times ~/ib01-captures/p71/runA1.cu8.times.csv \\
        --ours-ref ~/ib01-captures/found41/run.A.frames.csv:16 --spare-cmake config/local.cmake
"""
import argparse
import collections
import csv
import datetime
import hashlib
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stockscore import bridge_addrs, follows_rdv0, load, short_of_wire, spare_wire  # noqa: E402


def stats(v, unit="us"):
    if not v:
        return "n=0"
    s = sorted(v)
    n = len(s)
    mean = sum(s) / n
    sd = (sum((x - mean) ** 2 for x in s) / n) ** 0.5
    med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
    return f"n={n} min {s[0]:.1f} median {med:.1f} max {s[-1]:.1f} mean {mean:.1f} std {sd:.1f} {unit}"


def sx(seq):
    return "--" if seq in (None, "") else f"{int(seq):02x}"


def med(v):
    s = sorted(v)
    return s[len(s) // 2] if s else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True)
    ap.add_argument("--channel", type=int, default=26)
    ap.add_argument("--ours-ref", action="append", default=[], help="CSV:CH of our test channel")
    ap.add_argument("--spare-cmake", required=True)
    ap.add_argument("--times", help="timedcap .times.csv, for UTC times")
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--step-us", type=float, default=4048.0)
    ap.add_argument("--fast-us", type=float, default=320.0,
                    help="an ACK after a stock Data frame earlier than this is suspect (stock: 349 us and more)")
    ap.add_argument("--beacons", action="store_true")
    ap.add_argument("--list", action="store_true", help="one line per frame of ours")
    a = ap.parse_args()

    rows = load(a.csv, a.channel)
    ok = [r for r in rows if r["ok"]]
    bridge = set()
    for spec in a.ours_ref:
        bridge |= bridge_addrs(spec)
    spare = spare_wire(a.spare_cmake)
    if len(bridge) != 1:
        sys.exit(f"expected one bridge address from --ours-ref, got {len(bridge)}")
    (br,) = bridge
    sh_spare, sh_bridge = short_of_wire(spare), short_of_wire(br)

    host0 = None
    if a.times:
        with open(os.path.expanduser(a.times)) as f:
            t0 = next(csv.DictReader(f))
        host0 = float(t0["host_time"]) - int(t0["byte_offset"]) / 2 / a.fs

    def utc(t):
        if host0 is None:
            return f"s {t:9.3f}"
        return f"s {t:9.3f} " + datetime.datetime.fromtimestamp(host0 + t, datetime.timezone.utc).strftime("%H:%M:%S.%f")[:-3]

    def role(addr):
        if not addr:
            return "-"
        if addr == br:
            return "bridge"
        if addr == spare:
            return "spare"
        if addr in ("ffff", "ffffffffffffffff"):
            return "bcast"
        return "h:" + hashlib.sha256(addr.encode()).hexdigest()[:8]

    def srole(short):
        if short == sh_spare:
            return "spare"
        if short == sh_bridge:
            return "bridge"
        if short == "ffff":
            return "ffff"
        return "OTHER"

    breaches = []

    # Coordinator, as in stockscore.py.
    c = collections.Counter(r["src"] for i, r in enumerate(ok)
                            if r["type"] in ("data", "data+pend") and r["src"] not in (br, spare)
                            and follows_rdv0(ok, i))
    coord = c.most_common(1)[0][0] if c else None

    def name(addr):
        return "coord" if coord and addr == coord else role(addr)

    # Wake-up trains.
    trains, cur = [], []
    for i, r in enumerate(ok):
        if r["type"] != "wake-up":
            continue
        if cur and (r["seq"] != ok[cur[-1]]["seq"] or r["t_sync"] - ok[cur[-1]]["t_sync"] > 6e-3):
            trains.append(cur)
            cur = []
        cur.append(i)
    if cur:
        trains.append(cur)
    ours_idx = set()
    tr_rows = []
    for tr in trains:
        steps = [(ok[b]["t_sync"] - ok[x]["t_sync"]) * 1e6 for x, b in zip(tr, tr[1:])]
        m = med(steps) if steps else None
        nxt = tr[-1] + 1
        follow = ok[nxt] if nxt < len(ok) and ok[nxt]["t_pre"] - ok[tr[-1]]["t_end"] < 0.02 else None
        data_ours = follow is not None and follow["type"] != "enh-ack" and follow["src"] == br
        if data_ours or (m is not None and abs(m - a.step_us) <= 15):
            ours_idx.update(tr)
            shorts = collections.Counter(ok[j]["dst"] for j in tr)
            tr_rows.append({"t": ok[tr[0]]["t_sync"], "n": len(tr), "steps": steps, "med": m,
                            "short": shorts, "follow": follow if data_ours else None,
                            "dur": ok[tr[-1]]["t_end"] - ok[tr[0]]["t_pre"]})

    print(f"# {os.path.basename(a.csv)} channel {a.channel}: {len(rows)} sync hits, {len(ok)} CRC OK; "
          f"bridge and spare known (never printed); coordinator {'found' if coord else 'not found'}")

    # Fingerprints (carrier offset, level) per role.
    fp = collections.defaultdict(list)
    for i, r in enumerate(ok):
        if r["type"] == "wake-up":
            k = "bridge wake-up" if i in ours_idx else "stock wake-up"
        else:
            k = name(r["src"]) + " " + r["type"]
        fp[k].append((float(r["off_hz"]), float(r["rssi_dbfs"])))
    print("# fingerprints: role type: n, median carrier offset Hz, median level dBFS (range)")
    for k in sorted(fp, key=lambda k: -len(fp[k])):
        v = fp[k]
        if not (k.startswith("bridge") or k.startswith("spare") or k.startswith("coord") or len(v) >= 20):
            continue
        offs, lv = [x[0] for x in v], [x[1] for x in v]
        print(f"  {k:28s} n={len(v):5d} off {med(offs):+7.0f} ({min(offs):+.0f}..{max(offs):+.0f})  "
              f"level {med(lv):6.1f} ({min(lv):.1f}..{max(lv):.1f})")
    br_ack = fp.get("bridge enh-ack", [])
    br_off = med([x[0] for x in br_ack]) if br_ack else med([x[0] for x in fp.get("bridge data", [])])
    br_lv = med([x[1] for x in br_ack]) if br_ack else med([x[1] for x in fp.get("bridge data", [])])

    # Trains.
    print(f"# our wake-up trains: {len(tr_rows)}")
    allsteps = [s for t in tr_rows for s in t["steps"]]
    print("  every step within a train: " + stats(allsteps))
    print("  per-train median step: " + stats([t["med"] for t in tr_rows if t["med"] is not None]))
    by_short = collections.Counter()
    for t in tr_rows:
        for s, n in t["short"].items():
            by_short[srole(s)] += n
        bad = [s for s in t["short"] if srole(s) not in ("spare", "ffff")]
        if bad:
            breaches.append(f"train at {utc(t['t'])} carries a short address other than the spare's or ffff")
    print("  wake-up frames by short address: " + ", ".join(f"{k} {v}" for k, v in sorted(by_short.items())))
    kinds = collections.Counter()
    for t in tr_rows:
        k = "/".join(sorted(srole(s) for s in t["short"]))
        kinds[(k, "full" if t["n"] > 100 else "short")] += 1
    print("  trains: " + ", ".join(f"{k[0]} {k[1]} {v}" for k, v in sorted(kinds.items())))
    for kind in ("short", "full"):
        sel = [t for t in tr_rows if (t["n"] > 100) == (kind == "full")]
        if sel:
            print(f"  {kind} trains: frames {stats([t['n'] for t in sel], '')}, duration "
                  f"{stats([t['dur'] * 1e3 for t in sel], 'ms')}")
    nofollow = [t for t in tr_rows if t["follow"] is None]
    print(f"  trains with no frame of the bridge heard within 20 ms after: {len(nofollow)}")

    # Every frame with the bridge as source, and frames to the bridge.
    ours_frames = collections.Counter()
    for i, r in enumerate(ok):
        if r["type"] == "wake-up":
            continue
        s, d = name(r["src"]), name(r["dst"])
        if "bridge" in (s, d) or "spare" in (s, d):
            ours_frames[(r["type"], s, d)] += 1
            if s == "bridge":
                allowed = {"data": ("spare",), "data+pend": ("spare",), "enh-ack": ("spare",),
                           "bcast": ("bcast",), "bcast+pend": ("bcast",), "enh-beacon": ("spare",)}
                if d not in allowed.get(r["type"], ()):
                    breaches.append(f"{r['type']} of the bridge to {d} at {utc(r['t_sync'])}")
    # Beacons: the src field of an Enhanced Beacon is not in the Data source form.
    print("# frames with the bridge's or the spare's address (type, source, destination: n)")
    for k, v in sorted(ours_frames.items()):
        print(f"  {k[0]:10s} {k[1]:>10s} -> {k[2]:<10s} {v}")

    def answer_after(i, typ, src, ms=20.0):
        d = ok[i]
        j = i + 1
        while j < len(ok) and ok[j]["t_pre"] - d["t_end"] < ms / 1e3:
            x = ok[j]
            if x["type"] == typ and x["seq"] == d["seq"] and x["src"] == src:
                return j
            j += 1
        return None

    # Downlinks of ours.
    down_g, down_un = [], []
    for i, r in enumerate(ok):
        if r["type"] in ("data", "data+pend") and r["src"] == br:
            j = answer_after(i, "enh-ack", spare)
            if j is None:
                down_un.append(i)
            else:
                down_g.append((ok[j]["t_pre"] - r["t_end"]) * 1e6)
            if a.list:
                print(f"  down {utc(r['t_sync'])} seq {sx(r['seq'])} len {r['len']} "
                      + (f"spare ACK gap {(ok[j]['t_pre'] - r['t_end']) * 1e6:.1f} us" if j is not None else "NO ACK heard"))
    print(f"# Data frames of the bridge: {len(down_g) + len(down_un)}, ACKed by the spare {len(down_g)}, "
          f"no ACK heard {len(down_un)}")
    print("  Data end -> spare's ACK preamble: " + stats(down_g))
    for i in down_un:
        r = ok[i]
        nx = [ok[j] for j in range(i + 1, min(i + 6, len(ok))) if ok[j]["t_pre"] - r["t_end"] < 0.02]
        bad = [x for x in rows if not x["ok"] and 0 <= x["t_pre"] - r["t_end"] < 0.02]
        print(f"  no ACK: {utc(r['t_sync'])} seq {sx(r['seq'])} len {r['len']}; next 20 ms: "
              + (", ".join(f"{x['type']} {name(x['src'])}" for x in nx) or "nothing CRC-OK")
              + (f"; CRC-failed hits {len(bad)} (len {','.join(str(x['len']) for x in bad)})" if bad else ""))

    # Uplinks of the spare and our ACKs.
    up_g, up_un = [], []
    for i, r in enumerate(ok):
        if r["type"] in ("data", "data+pend") and r["src"] == spare:
            if r["dst"] != br:
                print(f"  NOTE: spare Data to {name(r['dst'])} at {utc(r['t_sync'])}")
            j = answer_after(i, "enh-ack", br)
            if j is None:
                up_un.append(i)
            else:
                up_g.append((ok[j]["t_pre"] - r["t_end"]) * 1e6)
    print(f"# Data frames of the spare: {len(up_g) + len(up_un)}, ACKed by the bridge {len(up_g)}, "
          f"no ACK heard {len(up_un)}")
    print("  Data end -> bridge's ACK preamble: " + stats(up_g))
    print(f"  within the 0-1 ms budget: {sum(1 for g in up_g if 0 <= g <= 1000)} of {len(up_g)}")
    for i in up_un:
        print(f"  no ACK: {utc(ok[i]['t_sync'])} seq {sx(ok[i]['seq'])} len {ok[i]['len']}")

    # Every ACK of the bridge must answer the spare's Data to the bridge.
    n_ack = 0
    for j, x in enumerate(ok):
        if x["type"] != "enh-ack" or x["src"] != br:
            continue
        n_ack += 1
        k = j - 1
        good = False
        while k >= 0 and x["t_pre"] - ok[k]["t_end"] < 0.02:
            d = ok[k]
            if d["type"] in ("data", "data+pend") and d["seq"] == x["seq"]:
                good = d["src"] == spare and d["dst"] == br
                break
            k -= 1
        if not good:
            breaches.append(f"ACK of the bridge at {utc(x['t_sync'])} not after a Data frame of the spare to the bridge")
    print(f"# Enhanced ACKs with the bridge as source: {n_ack}, each after a Data frame of the spare "
          f"to the bridge: {'yes' if not any('ACK of the bridge' in b for b in breaches) else 'NO'}")

    # Answers to stock frames.
    print(f"# answers to stock Data frames (bridge ACK fingerprint: off {br_off:+.0f} Hz, level {br_lv:.1f} dBFS)")
    n_stock, sus, gaps_d, gaps_u = 0, [], [], []
    for i, r in enumerate(ok):
        if r["type"] not in ("data", "data+pend") or r["src"] in (br, spare) or r["dst"] in (br, spare):
            continue
        n_stock += 1
        down = follows_rdv0(ok, i)
        j = i + 1
        while j < len(ok) and ok[j]["t_pre"] - r["t_end"] < 0.025:
            x = ok[j]
            g = (x["t_pre"] - r["t_end"]) * 1e6
            if x["type"] != "wake-up" and (x["src"] in (br, spare) or x["dst"] in (br, spare)):
                sus.append(f"{x['type']} {name(x['src'])}->{name(x['dst'])} {g:.0f} us after a stock Data frame "
                           f"at {utc(r['t_sync'])}")
            if x["type"] == "enh-ack" and x["seq"] == r["seq"]:
                (gaps_d if down else gaps_u).append(g)
                fp_like = abs(float(x["off_hz"]) - br_off) < 600 and abs(float(x["rssi_dbfs"]) - br_lv) < 2.0
                if g < a.fast_us or fp_like:
                    sus.append(f"enh-ack {name(x['src'])}->{name(x['dst'])} {g:.0f} us after a stock "
                               f"{'downlink' if down else 'uplink'} at {utc(r['t_sync'])}, off "
                               f"{float(x['off_hz']):+.0f} Hz level {float(x['rssi_dbfs']):.1f} dBFS"
                               + (" (bridge-like fingerprint)" if fp_like else " (fast)"))
            j += 1
    print(f"  stock Data frames {n_stock}; their ACKs: downlinks {stats(gaps_d)}; uplinks {stats(gaps_u)}")
    print(f"  suspect answers: {len(sus)}")
    for s in sus:
        print("   " + s)
    # Any CRC-OK frame with the bridge's fingerprint that is not ours by address or train.
    fpl = [r for i, r in enumerate(ok)
           if abs(float(r["off_hz"]) - br_off) < 600 and abs(float(r["rssi_dbfs"]) - br_lv) < 2.0
           and not (i in ours_idx or r["src"] == br or (r["type"] == "enh-beacon" and r["dst"] == spare))]
    print(f"  frames not of the bridge by address or train, with the bridge's fingerprint "
          f"(+-600 Hz, +-2 dB): {len(fpl)}")
    for r in fpl:
        print(f"   {utc(r['t_sync'])} {r['type']} {name(r['src'])}->{name(r['dst'])} off {float(r['off_hz']):+.0f} "
              f"level {float(r['rssi_dbfs']):.1f}")
    lv_all = [float(r["rssi_dbfs"]) for i, r in enumerate(ok) if not (i in ours_idx or r["src"] == br)]
    lv_bad = [float(r["rssi_dbfs"]) for r in rows if not r["ok"]]
    print(f"  loudest frame not of the bridge: {max(lv_all):.1f} dBFS (CRC OK), "
          f"{max(lv_bad) if lv_bad else float('nan'):.1f} dBFS (CRC failed); bridge {br_lv:.1f} dBFS")

    if a.beacons:
        print("# beacons")
        reqs = [i for i, r in enumerate(ok) if r["type"] == "beacon-req"]
        bcn = [i for i, r in enumerate(ok) if r["type"] == "enh-beacon"]
        print(f"  Beacon Requests {len(reqs)}: " + ", ".join(
            f"{k} {v}" for k, v in collections.Counter(name(ok[i]["src"]) for i in reqs).items()))
        print(f"  Enhanced Beacons {len(bcn)}: to " + ", ".join(
            f"{k} {v}" for k, v in collections.Counter(name(ok[i]["dst"]) for i in bcn).items()))
        bg = []
        for j in bcn:
            x = ok[j]
            k = j - 1
            prev = None
            while k >= 0 and x["t_pre"] - ok[k]["t_end"] < 0.05:
                if ok[k]["type"] == "beacon-req":
                    prev = ok[k]
                    break
                k -= 1
            if prev is None:
                breaches.append(f"beacon at {utc(x['t_sync'])} with no Beacon Request within 50 ms before")
                print(f"  beacon {utc(x['t_sync'])} to {name(x['dst'])}: NO request within 50 ms before")
                continue
            g = (x["t_pre"] - prev["t_end"]) * 1e6
            bg.append(g)
            ok_rule = prev["src"] == spare and x["dst"] == spare and 2400 <= g <= 7400
            if not ok_rule:
                breaches.append(f"beacon at {utc(x['t_sync'])} after a request of {name(prev['src'])}, gap {g:.0f} us")
            print(f"  beacon {utc(x['t_sync'])} seq {sx(x['seq'])} to {name(x['dst'])}, after request of "
                  f"{name(prev['src'])} seq {sx(prev['seq'])}: gap {g:.1f} us, off {float(x['off_hz']):+.0f} Hz "
                  f"level {float(x['rssi_dbfs']):.1f} dBFS {'ok' if ok_rule else 'BREACH'}")
        print("  request end -> beacon preamble: " + stats(bg))
        for i in reqs:
            r = ok[i]
            nb = [ok[j] for j in bcn if 0 <= ok[j]["t_pre"] - r["t_end"] < 0.05]
            print(f"  request {utc(r['t_sync'])} seq {sx(r['seq'])} from {name(r['src'])}: "
                  + (f"beacon after, gap {(nb[0]['t_pre'] - r['t_end']) * 1e6:.1f} us" if nb else "no beacon")
                  + f" (level {float(r['rssi_dbfs']):.1f} dBFS)")
            if nb and r["src"] != spare:
                breaches.append(f"beacon after a request of {name(r['src'])} at {utc(r['t_sync'])}")
        # Damaged beacon-length hits (CRC failed) are not decoded; count them near requests.

    print(f"# rule-3 breaches: {len(breaches)}")
    for b in breaches:
        print("  BREACH " + b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
