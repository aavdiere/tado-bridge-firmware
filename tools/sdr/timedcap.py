#!/usr/bin/env python3
"""Write rtl_sdr's stdout to a file and log host time against byte offset.

Receive-only. Usage (capture outside git):
  rtl_sdr -f 865924158 -s 2400000 -g 12.5 - | \
      python3 -I tools/sdr/timedcap.py ~/ib01-captures/<run>/run.cu8 [seconds]
Writes <file> and <file>.times.csv with "host_time,byte_offset" once per
chunk boundary, about every 0.1 s. A row's time is when the chunk ending at
that offset arrived
on the host, so a sample's host time is good to the USB buffering (a few
ms to tens of ms). Stops after `seconds` (default: until EOF).
"""
import sys, time
out_path = sys.argv[1]
limit = float(sys.argv[2]) if len(sys.argv) > 2 else None
src = sys.stdin.buffer
out = open(out_path, "wb")
tlog = open(out_path + ".times.csv", "w", buffering=1)
tlog.write("host_time,byte_offset\n")
off = 0
t_start = None
last_log = 0.0
while True:
    d = src.read(262144)
    if not d:
        break
    now = time.time()
    if t_start is None:
        t_start = now
    out.write(d)
    off += len(d)
    if now - last_log >= 0.1:
        # host time at which the chunk ending at this offset arrived
        tlog.write("%.6f,%d\n" % (now, off))
        last_log = now
    if limit is not None and now - t_start >= limit:
        break
tlog.write("%.6f,%d\n" % (time.time(), off))
out.close()
