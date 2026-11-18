#!/usr/bin/env python3
"""Log the bridge RTT stream (OpenOCD RTT server, TCP 19021) with a host-time prefix.

Read-only: it only connects to the running OpenOCD and never writes to the
bridge. Each line becomes "<unix time, ms> <line>". Usage:
  python3 -I tools/sdr/rttlog.py ~/ib01-captures/<run>/rtt.log [port]
"""
import socket, sys, time
out = open(sys.argv[1], "a", buffering=1)
s = socket.create_connection(("127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 19021))
buf = b""
while True:
    d = s.recv(4096)
    if not d:
        break
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        out.write("%.3f %s\n" % (time.time(), line.decode("utf-8", "replace").rstrip("\r")))
