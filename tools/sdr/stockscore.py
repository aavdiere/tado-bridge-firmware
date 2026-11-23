#!/usr/bin/env python3
"""Score the stock network's delivery on one channel per time block (P7.1, #20).

Receive-only, offline. Reads a demod.py --csv file of one plain capture of the
stock channel (tools/sdr/timedcap.py, demod.py --channels 26 --redact --csv)
and prints, per block of --block-s seconds (default 600):

  down  stock coordinator Data frames (unicast) and the share ACKed by the
        addressed device (Enhanced ACK, same seq, ACK source = Data
        destination, preamble within --ack-ms of the Data frame's end)
  up    stock device Data frames and the share ACKed by the coordinator
  dmg   of the frames without a CRC-OK ACK, those with a CRC-failed sync hit
        of ACK length (29 bytes) in the ACK window: an ACK sent but damaged
        at the SDR (or also at the receiver; the SDR cannot tell)
  retx  stock Data or broadcast frames whose (source, destination, seq) was
        already seen within --retx-s: retransmissions
  crc   sync hits with a failed radio CRC; "ovl" those that overlap any CRC-OK
        frame (a collision), "ours" those that overlap a frame of ours
  busy  share of the block's time with a frame on the air (union of preamble
        start to last CRC bit of every sync hit, CRC-failed hits capped at
        --bad-cap-ms), stock frames and ours separately
  lbt   share of listen windows that hold a frame on the air: a train start T
        uniform over the block, a window [T - W, T] with W = 3.968 ms + k x
        640 us, k uniform over 0..6 (random backoff plus the 3.968 ms listen,
        src/csl.h), counted busy when any frame overlaps it; exact, not
        sampled. "listen" is the 3.968 ms listen alone (k = 0), the only part
        src/csl.c listens to
  sim   Monte Carlo of the LBT loop (busy: retry 20 ms later with a new
        backoff; 10 busy listens in a row end the attempt, src/csl.h): the
        share of attempts that end BUSY

Ours: --ours-ref CSV:CH names a demod.py CSV of our own test channel; the
source of every Data frame there that follows a rendezvous-0 wake-up is the
bridge (only the bridge sends wake-up trains on the test channel).
--spare-cmake reads PAIR_TARGET_EUI64 from the gitignored config/local.cmake;
the spare device's frames count as ours, never as stock. A wake-up train with
a median step within --ours-step-us +-15 us, or followed within 20 ms by a Data
frame of ours, is ours (the bridge's step is 4048 us; stock trains step
3.94-3.99 or 4.40-4.45 ms, docs/test-bench.md "Stock network reference").
The coordinator is the most frequent source of Data frames that follow a
rendezvous-0 wake-up, ours excluded.

Short-address check (D7, docs/test-bench.md "P7.1 channel 26, plan" step 1):
with --spare-cmake, the 2-byte short address of every stock wake-up frame and
the first two wire bytes (swapped) of every stock address seen (Data
destination and source, ACK source and destination) are compared by SHA-256
with the spare's short address (src/csl.c:577-579: short = wire bytes 1, 0 of
the EUI-64 in wire order, the stored EUI-64 reversed). Only PASS or FAIL and
counts are printed.

Baseline and scoring against it (abort rule 2): --save-baseline FILE.json
writes the per-block figures and their means; --baseline FILE.json then marks
each block OVER when: a down or up ACK share is more than 15 percentage points
below the baseline's share (pooled over the whole baseline capture);
retransmissions are more than twice the baseline's rate per block (at least
1 per block, so a zero baseline does not make every single retransmission an
abort); or more than 3 CRC-failed frames overlap a frame of
ours. The baseline file holds counts and shares only, no address.

Blocks: from stream time --start-s (default 0, the capture start). With
--times (timedcap's .times.csv) and --start-host (Unix time, for example the
run's start in the RTT log), block 0 starts at that host time instead; the
mapping uses the sample count, not the host clock, past the first row.

Timing resolution: as demod.py (2.4 MS/s decimated to 600 kS/s, 1.667 us per
sample; sync start from a least-squares fit of every bit-boundary zero
crossing, well under 1 us; preamble start and frame end derived from it).
Limit: demod.py does not look for a sync word inside a frame it has decoded,
so a frame that starts during another is missed, not counted as a collision.

Addresses are never printed. Captures and CSVs stay in ~/ib01-captures/.

    python3 -I tools/sdr/stockscore.py --csv ~/ib01-captures/p71/baseline.frames.csv \\
        --ours-ref ~/ib01-captures/found41/run.A.frames.csv:16 \\
        --spare-cmake config/local.cmake --save-baseline ~/ib01-captures/p71/baseline.json
    python3 -I tools/sdr/stockscore.py --csv ~/ib01-captures/p71/runA1.frames.csv \\
        --ours-ref ~/ib01-captures/found41/run.A.frames.csv:16 \\
        --spare-cmake config/local.cmake --baseline ~/ib01-captures/p71/baseline.json
"""
import argparse
import bisect
import collections
import csv
import hashlib
import json
import os
import re
import sys

import numpy as np

sys.dont_write_bytecode = True

COUNTS = ("down", "down_acked", "down_dmg", "up", "up_acked", "up_dmg", "retx", "retx_base", "crc_bad", "crc_bad_ovl",
          "crc_bad_ours", "sync_hits", "stock_frames", "ours_frames", "ours_trains", "ours_ffff_trains",
          "bridge_frames", "spare_frames")
RETRY_S = 20e-3         # src/csl.h CSL_LBT_RETRY_US
BUSY_MAX = 10           # src/csl.h CSL_LBT_BUSY_MAX
LISTEN_S = 3.968e-3      # src/csl.h listen
BACKOFF_STEP_S = 640e-6  # src/csl.h backoff unit, 0..6 of them
BACKOFF_MAX = 6


def load(path, ch):
    rows = []
    with open(os.path.expanduser(path)) as f:
        for r in csv.DictReader(f):
            if int(r["ch"]) != ch:
                continue
            for k in ("t_pre", "t_sync", "t_end"):
                r[k] = float(r[k])
            r["ok"] = r["crc_ok"] == "True"
            r["seq"] = r["seq"] if r["seq"] != "" else None
            rows.append(r)
    rows.sort(key=lambda r: r["t_sync"])
    return rows


def hsh(s):
    return hashlib.sha256(s.encode()).hexdigest()


def short_of_wire(addr):
    """Short address (wake-up wire bytes) of a wire-order address: bytes 1, 0."""
    return addr[2:4] + addr[0:2] if len(addr) >= 4 else ""


def spare_wire(path):
    txt = open(os.path.expanduser(path)).read()
    m = re.search(r'PAIR_TARGET_EUI64\s+"?([0-9A-Fa-f:\- ]+)"?\s*\)', txt)
    if not m:
        sys.exit("no PAIR_TARGET_EUI64 in " + path)
    e = re.sub(r"[: -]", "", m.group(1)).lower()
    if len(e) != 16:
        sys.exit("PAIR_TARGET_EUI64 is not 8 bytes")
    return "".join(e[i:i + 2] for i in range(14, -2, -2))   # stored order reversed


def follows_rdv0(rows, i):
    p = rows[i - 1] if i else None
    return (p is not None and p["ok"] and p["type"] == "wake-up" and p["extra"] == "rdv=0"
            and rows[i]["t_pre"] - p["t_end"] < 0.02)


def bridge_addrs(spec):
    path, _, ch = spec.rpartition(":")
    rows = [r for r in load(path, int(ch)) if r["ok"]]
    c = collections.Counter(r["src"] for i, r in enumerate(rows)
                            if r["type"] in ("data", "data+pend") and follows_rdv0(rows, i))
    return {a for a, n in c.items() if n >= 2}


def union_len(iv, lo, hi):
    tot, cur0, cur1 = 0.0, None, None
    for a, b in sorted(iv):
        a, b = max(a, lo), min(b, hi)
        if b <= a:
            continue
        if cur1 is None or a > cur1:
            if cur1 is not None:
                tot += cur1 - cur0
            cur0, cur1 = a, b
        else:
            cur1 = max(cur1, b)
    if cur1 is not None:
        tot += cur1 - cur0
    return tot


def merged(iv):
    out = []
    for p, e in sorted(iv):
        if out and p <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([p, e])
    return np.array(out, dtype=float).reshape(-1, 2)


def lbt_sim(iv, b0, b1, n, rng):
    """Monte Carlo of the LBT loop of src/csl.c:428-434,504-516,627-631: a
    listen of 3.968 ms ends at each train start; busy -> next start 20 ms
    plus a new random backoff plus the listen later; 10 busy listens in a row
    end the attempt. Returns (share of first listens busy, mean listens per
    attempt, share of attempts ended BUSY)."""
    m = merged(iv)
    if not len(m):
        return 0.0, 1.0, 0.0
    s, e = m[:, 0], m[:, 1]

    def busy(t):
        i = np.searchsorted(s, t) - 1     # last frame starting before the listen's end
        return (i >= 0) & (e[np.clip(i, 0, None)] > t - LISTEN_S)

    t = rng.uniform(b0 + LISTEN_S, b1 - 0.4, n)
    alive = np.ones(n, bool)
    listens = np.zeros(n)
    first = None
    for k in range(BUSY_MAX):
        listens += alive
        b = busy(t) & alive
        if first is None:
            first = b.mean()
        alive = b
        t = t + RETRY_S + rng.integers(0, BACKOFF_MAX + 1, n) * BACKOFF_STEP_S + LISTEN_S
    return float(first), float(listens.mean()), float(alive.mean())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", required=True, help="demod.py --csv of the stock channel")
    ap.add_argument("--channel", type=int, default=26)
    ap.add_argument("--block-s", type=float, default=600.0)
    ap.add_argument("--start-s", type=float, default=0.0, help="stream time of block 0")
    ap.add_argument("--times", help="timedcap .times.csv, with --start-host")
    ap.add_argument("--start-host", type=float, help="Unix time of block 0 (needs --times)")
    ap.add_argument("--fs", type=float, default=2.4e6)
    ap.add_argument("--min-block-s", type=float, default=300.0, help="drop a shorter last block")
    ap.add_argument("--ours-ref", action="append", default=[], help="CSV:CH of our test channel")
    ap.add_argument("--spare-cmake", help="config/local.cmake with PAIR_TARGET_EUI64 (never printed)")
    ap.add_argument("--ours-step-us", type=float, default=4048.0)
    ap.add_argument("--ack-ms", type=float, default=20.0)
    ap.add_argument("--retx-s", type=float, default=30.0)
    ap.add_argument("--bad-cap-ms", type=float, default=25.0)
    ap.add_argument("--retx-block", type=float, default=6.0,
                    help="a block is over at this many retransmissions or more (owner 2026-11-23: 6)")
    ap.add_argument("--lbt-sim", type=int, default=200000, help="Monte Carlo attempts per block")
    ap.add_argument("--save-baseline")
    ap.add_argument("--baseline")
    a = ap.parse_args()

    rows = load(a.csv, a.channel)
    if not rows:
        sys.exit("no frames on channel %d" % a.channel)
    for r in rows:
        if not r["ok"]:
            r["t_end"] = min(r["t_end"], r["t_pre"] + a.bad_cap_ms / 1e3)
    ok = [r for r in rows if r["ok"]]

    bridge = set()
    for spec in a.ours_ref:
        bridge |= bridge_addrs(spec)
    ours = set(bridge)
    spare = spare_wire(a.spare_cmake) if a.spare_cmake else None
    if spare:
        ours.add(spare)
    print(f"# ours: {len(ours)} address(es) known (bridge from --ours-ref, spare from --spare-cmake)")

    # Wake-up trains: consecutive wake-ups with the same seq, steps < 6 ms.
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
    is_ours = [False] * len(ok)
    for i, r in enumerate(ok):
        if r["type"] == "wake-up":
            continue
        # An Enhanced Beacon's source field is not the bridge's Data source
        # form, so a beacon to the spare counts as ours by its destination.
        if r["src"] in ours or (r["type"] == "enh-beacon" and r["dst"] in ours):
            is_ours[i] = True
    n_our_trains = 0
    our_train_steps = []
    our_trains = []   # (t_sync of the first wake-up, short address)
    for tr in trains:
        steps = sorted(ok[b]["t_sync"] - ok[a_]["t_sync"] for a_, b in zip(tr, tr[1:]))
        med = steps[len(steps) // 2] * 1e6 if steps else None
        last = ok[tr[-1]]
        nxt = tr[-1] + 1
        data_ours = (nxt < len(ok) and ok[nxt]["type"] != "enh-ack" and ok[nxt]["src"] in bridge
                     and ok[nxt]["t_pre"] - last["t_end"] < 0.02)
        if data_ours or (med is not None and abs(med - a.ours_step_us) <= 15):
            n_our_trains += 1
            our_trains.append((ok[tr[0]]["t_sync"], ok[tr[0]]["dst"]))
            if med is not None:
                our_train_steps.append(med)
            for j in tr:
                is_ours[j] = True

    c = collections.Counter(r["src"] for i, r in enumerate(ok)
                            if r["type"] in ("data", "data+pend") and not is_ours[i]
                            and follows_rdv0(ok, i))
    coord = c.most_common(1)[0][0] if c else None
    print(f"# coordinator: {'found' if coord else 'NOT found'} ({c.most_common(1)[0][1] if c else 0} "
          f"Data frames after a rendezvous-0 wake-up)")

    # Block edges in stream time.
    t_first, t_last = rows[0]["t_pre"], rows[-1]["t_end"]
    start = a.start_s
    if a.start_host is not None:
        if not a.times:
            sys.exit("--start-host needs --times")
        with open(os.path.expanduser(a.times)) as f:
            tr0 = next(csv.DictReader(f))
        host0 = float(tr0["host_time"]) - int(tr0["byte_offset"]) / 2 / a.fs
        start = a.start_host - host0
    if a.times and os.path.exists(os.path.expanduser(a.times)):
        with open(os.path.expanduser(a.times)) as f:
            tl = list(csv.DictReader(f))
        dur = int(tl[-1]["byte_offset"]) / 2 / a.fs
    else:
        dur = t_last
    edges = []
    t = start
    while t < dur:
        e = min(t + a.block_s, dur)
        if e - t >= a.min_block_s or not edges:
            edges.append((t, e))
        t += a.block_s

    starts = [r["t_pre"] for r in ok]
    our_iv = [(r["t_pre"], r["t_end"]) for i, r in enumerate(ok) if is_ours[i]]
    our_iv.sort()
    our_starts = [x[0] for x in our_iv]
    maxlen = max([b - a_ for a_, b in our_iv] + [0.05])

    def overlaps_ours(r):
        j = bisect.bisect_left(our_starts, r["t_pre"] - maxlen)
        while j < len(our_iv) and our_iv[j][0] < r["t_end"]:
            if our_iv[j][1] > r["t_pre"]:
                return True
            j += 1
        return False

    def ack_after(i, want_src):
        d = ok[i]
        j = i + 1
        while j < len(ok) and ok[j]["t_pre"] - d["t_end"] < a.ack_ms / 1e3:
            x = ok[j]
            if x["type"] == "enh-ack" and x["seq"] == d["seq"] and x["src"] == want_src:
                return True
            j += 1
        return False

    bad_rows = sorted((r for r in rows if not r["ok"]), key=lambda r: r["t_pre"])
    bad_starts = [r["t_pre"] for r in bad_rows]

    def damaged_ack_after(i):
        """A CRC-failed sync hit of Enhanced ACK length (29) in the ACK window."""
        d = ok[i]
        j = bisect.bisect_left(bad_starts, d["t_end"])
        while j < len(bad_rows) and bad_rows[j]["t_pre"] - d["t_end"] < a.ack_ms / 1e3:
            if int(bad_rows[j]["len"]) == 29:
                return True
            j += 1
        return False

    blocks = []
    rng = np.random.default_rng(1)
    last_seen = {}
    retx_flag = [False] * len(ok)
    for i, r in enumerate(ok):
        if is_ours[i] or r["type"] not in ("data", "data+pend", "bcast", "bcast+pend"):
            continue
        key = (r["src"], r["dst"], r["seq"])
        if key in last_seen and r["t_sync"] - last_seen[key] <= a.retx_s:
            retx_flag[i] = True
        last_seen[key] = r["t_sync"]

    for b0, b1 in edges:
        B = collections.Counter()
        for i, r in enumerate(ok):
            if not (b0 <= r["t_sync"] < b1):
                continue
            if is_ours[i]:
                B["ours_frames"] += 1
                B["bridge_frames"] += r["src"] in bridge or r["dst"] in bridge
                B["spare_frames"] += bool(spare) and (r["src"] == spare or r["dst"] == spare)
                continue
            B["stock_frames"] += 1
            if r["type"] in ("data", "data+pend"):
                if coord and r["src"] == coord:
                    B["down"] += 1
                    hit = ack_after(i, r["dst"])
                    B["down_acked"] += hit
                    B["down_dmg"] += (not hit) and damaged_ack_after(i)
                elif coord:
                    B["up"] += 1
                    hit = ack_after(i, coord)
                    B["up_acked"] += hit
                    B["up_dmg"] += (not hit) and damaged_ack_after(i)
            if retx_flag[i]:
                B["retx"] += 1
            if r["type"] in ("data", "data+pend", "bcast", "bcast+pend"):
                B["retx_base"] += 1
        B["ours_trains"] = sum(1 for t, _ in our_trains if b0 <= t < b1)
        B["ours_ffff_trains"] = sum(1 for t, d in our_trains if b0 <= t < b1 and d == "ffff")
        bad = [r for r in rows if not r["ok"] and b0 <= r["t_sync"] < b1]
        B["crc_bad"] = len(bad)
        B["sync_hits"] = sum(1 for r in rows if b0 <= r["t_sync"] < b1)
        for r in bad:
            j = bisect.bisect_left(starts, r["t_pre"] - 0.05)
            hit = False
            while j < len(ok) and ok[j]["t_pre"] < r["t_end"]:
                if ok[j]["t_end"] > r["t_pre"]:
                    hit = True
                    break
                j += 1
            B["crc_bad_ovl"] += hit
            B["crc_bad_ours"] += overlaps_ours(r)
        T = b1 - b0
        all_iv = [(r["t_pre"], r["t_end"]) for r in rows if r["t_end"] > b0 and r["t_pre"] < b1]
        stock_iv = [(r["t_pre"], r["t_end"]) for i, r in enumerate(ok) if not is_ours[i]] + \
                   [(r["t_pre"], r["t_end"]) for r in rows if not r["ok"]]
        stock_iv = [x for x in stock_iv if x[1] > b0 and x[0] < b1]
        busy = union_len(all_iv, b0, b1) / T
        busy_stock = union_len(stock_iv, b0, b1) / T
        busy_ours = union_len([x for x in our_iv if x[1] > b0 and x[0] < b1], b0, b1) / T
        # LBT: T_start busy iff some frame [p, e] has p < T_start and e > T_start - W.
        lbt_k = []
        for k in range(BACKOFF_MAX + 1):
            W = LISTEN_S + k * BACKOFF_STEP_S
            lbt_k.append(union_len([(p, e + W) for p, e in stock_iv], b0, b1) / T)
        blocks.append({
            "t0": b0, "t1": b1, **{k: int(B[k]) for k in COUNTS},
            "down_share": B["down_acked"] / B["down"] if B["down"] else None,
            "up_share": B["up_acked"] / B["up"] if B["up"] else None,
            "busy": busy, "busy_stock": busy_stock, "busy_ours": busy_ours,
            "lbt": sum(lbt_k) / len(lbt_k), "lbt_listen": lbt_k[0],
            **dict(zip(("sim_first", "sim_listens", "sim_busy_end"),
                       lbt_sim(stock_iv, b0, b1, a.lbt_sim, rng))),
        })

    base = json.load(open(os.path.expanduser(a.baseline))) if a.baseline else None

    def pct(x):
        return "  n/a" if x is None else f"{100 * x:5.1f}"

    print("# block  stream s       down acked dmg  %     up acked dmg  %    retx  crc bad/ovl/ours  "
          "busy% (stock/ours)   lbt% listen% sim:busy-end%  ours: frames trains ffff-trains")
    over_any = False
    for n, b in enumerate(blocks):
        flags = []
        if base:
            m = base["ref"]
            if b["down_share"] is not None and m["down_share"] is not None and \
                    b["down_share"] < m["down_share"] - 0.15:
                flags.append("DOWN-ACK")
            if b["up_share"] is not None and m["up_share"] is not None and \
                    b["up_share"] < m["up_share"] - 0.15:
                flags.append("UP-ACK")
            if b["retx"] >= a.retx_block * (b["t1"] - b["t0"]) / a.block_s:
                flags.append("RETX")
            if b["crc_bad_ours"] > 3:
                flags.append("CRC-OURS")
        over_any |= bool(flags)
        print(f"  {n:3d}  {b['t0']:7.1f}-{b['t1']:7.1f}  {b['down']:4d} {b['down_acked']:4d} {b['down_dmg']:3d} "
              f"{pct(b['down_share'])}  {b['up']:4d} {b['up_acked']:4d} {b['up_dmg']:3d} {pct(b['up_share'])}  "
              f"{b.get('retx', 0):4d}  {b['crc_bad']:4d} {b.get('crc_bad_ovl', 0):3d} {b.get('crc_bad_ours', 0):3d}   "
              f"{100 * b['busy']:5.2f} ({100 * b['busy_stock']:5.2f}/{100 * b['busy_ours']:4.2f})  "
              f"{100 * b['lbt']:6.2f} {100 * b['lbt_listen']:6.2f} {100 * b['sim_busy_end']:7.3f}   "
              f"{b.get('ours_frames', 0):5d} "
              f"{b.get('ours_trains', 0):4d} {b.get('ours_ffff_trains', 0):4d}  {' '.join(flags) or ('ok' if base else '')}")
    full = [b for b in blocks if b["t1"] - b["t0"] >= a.block_s - 1] or blocks

    def mean(key):
        v = [b[key] for b in full if b.get(key) is not None]
        return sum(v) / len(v) if v else None

    keys = ["down_share", "up_share", "retx", "crc_bad", "crc_bad_ovl", "busy", "busy_stock", "lbt",
            "sim_first", "sim_listens", "sim_busy_end",
            "lbt_listen", "down", "up", "stock_frames"]
    mn = {k: mean(k) for k in keys}
    # Pooled over every block (the last, short one too): the reference for abort rule 2.
    tot = {k: sum(b[k] for b in blocks) for k in ("down", "down_acked", "up", "up_acked", "retx")}
    span = sum(b["t1"] - b["t0"] for b in blocks)
    ref = {"down_share": tot["down_acked"] / tot["down"] if tot["down"] else None,
           "up_share": tot["up_acked"] / tot["up"] if tot["up"] else None,
           "retx": tot["retx"] * a.block_s / span, "span_s": span,
           **{"n_" + k: v for k, v in tot.items()}}
    for k in ("retx", "crc_bad", "crc_bad_ovl", "down", "up", "stock_frames"):
        mn[k] = mn[k] or 0.0
    print(f"# mean of {len(full)} full block(s): down {pct(mn['down_share'])} %, up {pct(mn['up_share'])} %, "
          f"retx {mn['retx']:.1f}, crc bad {mn['crc_bad']:.1f} (ovl {mn['crc_bad_ovl']:.1f}), "
          f"busy {100 * mn['busy']:.2f} %, lbt {100 * mn['lbt']:.2f} % (listen {100 * mn['lbt_listen']:.2f} %)")
    print(f"# LBT loop (Monte Carlo, {a.lbt_sim} attempts per block): first listen busy "
          f"{100 * mn['sim_first']:.2f} %, listens per attempt {mn['sim_listens']:.4f}, "
          f"attempts ended BUSY (10 busy in a row) {100 * mn['sim_busy_end']:.3f} %")
    print(f"# our wake-up trains: {n_our_trains}" + (
        f", median step {sorted(our_train_steps)[len(our_train_steps) // 2]:.1f} us" if our_train_steps else ""))
    if base:
        m = base["ref"]
        print(f"# baseline (pooled over {m['span_s']:.0f} s): down {pct(m['down_share'])} %, up "
              f"{pct(m['up_share'])} %, retransmissions {m['retx']:.2f} per {a.block_s:.0f} s")
        print(f"# this run (pooled over {span:.0f} s): down {pct(ref['down_share'])} %, up {pct(ref['up_share'])} %, "
              f"retransmissions {tot['retx']} (twice the baseline rate over this span: "
              f"{2 * m['retx'] * span / a.block_s:.1f})")
        print("# thresholds (abort rule 2): down ACK share below "
              + (f"{100 * (m['down_share'] - 0.15):.1f} %" if m["down_share"] is not None else "n/a")
              + ", up ACK share below "
              + (f"{100 * (m['up_share'] - 0.15):.1f} %" if m["up_share"] is not None else "n/a")
              + f", retransmissions {a.retx_block:.0f} or more per block or above "
              f"{2 * m['retx'] * span / a.block_s:.1f} over the run, "
              "CRC-failed frames over ours above 3 per block")
        run_retx = tot["retx"] > 2 * m["retx"] * span / a.block_s
        if run_retx:
            print("# RETX-RUN: retransmissions over the run above twice the baseline rate")
        print("# verdict: " + ("OVER a threshold" if over_any or run_retx else "all blocks and the run within the thresholds"))

    if spare:
        sh = hsh(short_of_wire(spare))
        stock_shorts = collections.Counter()
        full_seen = 0
        for i, r in enumerate(ok):
            if is_ours[i]:
                continue
            if r["type"] == "wake-up":
                if r["dst"] and r["dst"] != "ffff":
                    stock_shorts[hsh(r["dst"])] += 1
                continue
            for addr in (r["dst"], r["src"]):
                if len(addr) == 16:
                    stock_shorts[hsh(short_of_wire(addr))] += 1
                    full_seen += addr == spare
        n_match = stock_shorts.get(sh, 0)
        print(f"# short-address check: {len(stock_shorts)} distinct stock short addresses, "
              f"{sum(stock_shorts.values())} occurrences; spare's short address "
              + ("FAIL: shared by %d stock occurrences" % n_match if n_match else "PASS: not among them"))
        print(f"# frames with the spare's address: {sum(b['spare_frames'] for b in blocks)}; "
              f"with the bridge's: {sum(b['bridge_frames'] for b in blocks)}")
        if full_seen:
            print(f"# NOTE: the spare's full address appears in {full_seen} stock-classified frames")

    if a.save_baseline:
        with open(os.path.expanduser(a.save_baseline), "w") as f:
            json.dump({"csv": os.path.basename(a.csv), "block_s": a.block_s, "blocks": blocks, "mean": mn,
                       "ref": ref},
                      f, indent=1)
        print("# baseline written to " + a.save_baseline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
