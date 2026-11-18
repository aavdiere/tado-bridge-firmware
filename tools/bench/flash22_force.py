#!/usr/bin/env python3
"""Force counter-log sector erases on the running bridge (#22 bench test, part 1).

Talks to an OpenOCD that is already running (tools/rtt.sh, telnet port 4444)
and uses only `mdw` reads and two `mww` writes on the running core:
  1. ctr.next (store.c, offset 16) := 512, so the next counter-log append
     moves to the other sector and erases it first;
  2. limit (network.c) := limit - 2048, so network_service() finds less
     than STORE_CTR_LOW (2048) left and appends one entry at its next quiet
     moment.
It never halts, resets, or uses flash or option-byte commands.

Reviewer conditions (fw-reviewer, 2026-11-18), enforced here:
  - a force only when at least one frame counter was used since the last
    reservation (counter > limit - 4096), so the append is above every entry;
  - never a second lowering before the firmware has moved `limit` (no
    pending lowering is ever stacked);
  - no force when a natural extension is near (limit - counter < 2048 + 64);
  - no force while network.c's `failing` flag is set; it stops if it is.
  - at least --min-gap seconds between forced erases.

Timing: once the frame counter moves (a transaction of ours started), the
next force is aimed --aim-lo..--aim-hi s later; otherwise one every
--min-gap..--max-gap s while a counter is available. With --rtt-log (the
rttlog.py file), the aim comes from the RTT instead: --aim-lo..--aim-hi s after
a `tx: datagram ... delivered` line, where the device's reply to the downlink
is due (about 0.32 s after the line on the bench), and the timer waits
--hold s after a received uplink (`to server`), so that the 2 s gap does not
use up the downlink that usually follows it. --aim-phase LO HI aims instead at
LO..HI s into the host-time second of the `delivered` line: on the bench the
device's reply starts at a fixed phase of its 1 s sample period (measured on
air), and the RTT line comes 40-120 ms after the downlink, too variable to aim
from. The timer path waits; the timer also waits until the
counter has been still for --still s (a counter is taken when a downlink job is
scheduled, before its train). Polls every --poll s;
the "done" time is the first poll that sees `limit` move (after the erase,
the program and their read-back), so it is good to one poll plus one `mdw`.

Usage (symbols from `arm-none-eabi-nm -S` on the flashed ELF):
  python3 -I tools/bench/flash22_force.py --ctr 0x20006940 --limit 0x20007918 \
      --failing 0x20007920 --counter 0x20006984 --seconds 1800 \
      --log ~/ib01-captures/flash22/force.log
Touch <log>.STOP to stop early. --dry-run reads and logs only.
"""
import argparse, os, random, re, socket, sys, time

CTR_AHEAD, CTR_LOW, CTR_ENTRIES = 4096, 2048, 512


class Ocd:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self._read()

    def _read(self):
        buf = b""
        self.s.settimeout(5)
        while not buf.endswith(b"> "):
            d = self.s.recv(4096)
            if not d:
                raise ConnectionError("OpenOCD closed the telnet session")
            buf += d
        return buf.decode(errors="replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        return self._read()

    def mdw(self, addr, n=1):
        out = self.cmd("mdw 0x%08x %d" % (addr, n))
        words = []
        for line in out.splitlines():
            m = re.match(r"\s*\x00?0x([0-9a-fA-F]{8}):((?:\s+[0-9a-fA-F]{8})+)", line.strip("\x00"))
            if m:
                words += [int(w, 16) for w in m.group(2).split()]
        if len(words) != n:
            raise RuntimeError("mdw 0x%08x: %r" % (addr, out))
        return words

    def mww(self, addr, val):
        out = self.cmd("mww 0x%08x 0x%08x" % (addr, val))
        if "rror" in out or "fail" in out:
            raise RuntimeError("mww 0x%08x: %r" % (addr, out))


def main():
    ap = argparse.ArgumentParser()
    for a in ("ctr", "limit", "failing", "counter"):
        ap.add_argument("--" + a, required=True, type=lambda x: int(x, 0))
    ap.add_argument("--seconds", type=float, default=1800)
    ap.add_argument("--min-gap", type=float, default=2.0)
    ap.add_argument("--max-gap", type=float, default=5.0)
    ap.add_argument("--aim-lo", type=float, default=0.05)
    ap.add_argument("--aim-hi", type=float, default=0.6)
    ap.add_argument("--poll", type=float, default=0.01)
    ap.add_argument("--rtt-log", help="rttlog.py file: aim at the device's reply to a downlink")
    ap.add_argument("--hold", type=float, default=2.5)
    ap.add_argument("--aim-phase", type=float, nargs=2, metavar=("LO", "HI"),
                    help="with --rtt-log: aim at host-time phase LO..HI s within the second "
                         "of the `delivered` line (the device's sample phase), instead of "
                         "--aim-lo..--aim-hi after the line")
    ap.add_argument("--still", type=float, default=1.0,
                    help="with --rtt-log, the timer waits until the counter is still this long")
    ap.add_argument("--done-timeout", type=float, default=120.0)
    ap.add_argument("--dry-run", action="store_true", help="read and log only, never write")
    ap.add_argument("--port", type=int, default=4444)
    ap.add_argument("--log", required=True)
    a = ap.parse_args()

    log = open(a.log, "a", buffering=1)
    stop_file = a.log + ".STOP"

    def say(msg):
        line = "%.3f %s" % (time.time(), msg)
        log.write(line + "\n")
        print(line, flush=True)

    ocd = Ocd(a.port)

    def state():
        c = ocd.mdw(a.ctr, 5)
        return {
            "scanned": c[0] & 0xFF, "found": (c[0] >> 8) & 0xFF, "max": c[1], "max_sec": c[2],
            "cur": c[3], "next": c[4], "limit": ocd.mdw(a.limit)[0],
            "failing": ocd.mdw(a.failing)[0] & 0xFF, "counter": ocd.mdw(a.counter)[0],
        }

    st = state()
    say("start %s" % st)
    if not (st["scanned"] == 1 and st["found"] == 1 and st["cur"] in (0, 1)
            and st["next"] < CTR_ENTRIES and st["max"] == st["limit"] and st["failing"] == 0
            and st["counter"] <= st["limit"]):
        say("ABORT: unexpected state, nothing written")
        return 2

    t_end = time.time() + a.seconds
    last_force = 0.0
    next_due = time.time() + random.uniform(a.min_gap, a.max_gap)
    aim_at = None
    last_counter = st["counter"]
    n_force = n_done = 0
    rtt = None
    hold_until = 0.0
    if a.rtt_log:
        rtt = open(os.path.expanduser(a.rtt_log), errors="replace")
        rtt.seek(0, 2)
    rtt_buf = ""
    counter_moved = time.time()
    while time.time() < t_end and not os.path.exists(stop_file):
        now = time.time()
        counter = ocd.mdw(a.counter)[0]
        limit = ocd.mdw(a.limit)[0]
        if rtt is not None:
            rtt_buf += rtt.read()
            while "\n" in rtt_buf:
                line, rtt_buf = rtt_buf.split("\n", 1)
                m = re.match(r"([0-9.]+) (.*)", line)
                if not m:
                    continue
                lt, text = float(m.group(1)), m.group(2)
                if re.search(r"tx: datagram .*delivered", text) and aim_at is None:
                    if a.aim_phase:
                        aim_at = int(lt) + random.uniform(*a.aim_phase)
                    else:
                        aim_at = lt + random.uniform(a.aim_lo, a.aim_hi)
                elif "to server" in text:
                    hold_until = max(hold_until, lt + a.hold)
        if counter != last_counter:
            counter_moved = now
            if counter > last_counter and aim_at is None and rtt is None:
                aim_at = now + random.uniform(a.aim_lo, a.aim_hi)
            last_counter = counter
        base = limit - CTR_AHEAD
        eligible = counter > base and limit - counter >= CTR_LOW + 64
        due = (aim_at is not None and now >= aim_at) or (
            now >= next_due and now >= hold_until and aim_at is None
            and (rtt is None or now - counter_moved >= a.still))
        if not (eligible and due and now - last_force >= a.min_gap):
            if aim_at is not None and now >= aim_at and (not eligible or now - aim_at > 1.0):
                aim_at = None
            time.sleep(a.poll)
            continue

        why = "aimed" if aim_at is not None and now >= aim_at else "timer"
        aim_at = None
        st = state()
        if st["failing"]:
            say("STOP: network failing flag set before a force: %s" % st)
            break
        if not (st["limit"] == limit and st["max"] == limit and st["next"] < CTR_ENTRIES
                and st["counter"] > st["limit"] - CTR_AHEAD
                and st["limit"] - st["counter"] >= CTR_LOW + 64):
            say("skip: state moved: %s" % st)
            time.sleep(a.poll)
            continue
        if a.dry_run:
            say("dry-run: would force (%s): %s" % (why, st))
            last_force = time.time()
            next_due = last_force + random.uniform(a.min_gap, a.max_gap)
            continue
        ocd.mww(a.ctr + 16, CTR_ENTRIES)
        if ocd.mdw(a.ctr + 16)[0] != CTR_ENTRIES:
            say("STOP: ctr.next read back wrong")
            break
        new_lim = st["limit"] - CTR_LOW
        t_fire = time.time()
        ocd.mww(a.limit, new_lim)
        n_force += 1
        last_force = t_fire
        say("force %d %s: cur %d next %d->512, limit %d->%d, counter %d (used since reservation %d)"
            % (n_force, why, st["cur"], st["next"], st["limit"], new_lim, st["counter"],
               st["counter"] - (st["limit"] - CTR_AHEAD)))
        # Wait for the firmware to move `limit` (append done).
        ok = False
        while time.time() - t_fire < a.done_timeout:
            lim2 = ocd.mdw(a.limit)[0]
            if lim2 != new_lim:
                t_done = time.time()
                s2 = state()
                good = (s2["limit"] == lim2 and s2["max"] == lim2 and s2["cur"] == st["cur"] ^ 1
                        and s2["next"] == 1 and s2["max_sec"] == s2["cur"] and s2["failing"] == 0
                        and lim2 > st["limit"])
                n_done += 1
                say("done %d after %.1f ms: limit %d, cur %d next %d max %d max_sec %d counter %d %s"
                    % (n_done, (t_done - t_fire) * 1e3, lim2, s2["cur"], s2["next"], s2["max"],
                       s2["max_sec"], s2["counter"], "ok" if good else "UNEXPECTED"))
                if not good:
                    say("STOP: unexpected state after an append")
                    return 3
                ok = True
                break
            time.sleep(a.poll)
        if not ok:
            st = state()
            say("STOP: limit did not move within %.0f s: %s" % (a.done_timeout, st))
            return 4
        last_counter = ocd.mdw(a.counter)[0]
        next_due = time.time() + random.uniform(a.min_gap, a.max_gap)
    say("end: forced %d, done %d; %s" % (n_force, n_done, state()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
