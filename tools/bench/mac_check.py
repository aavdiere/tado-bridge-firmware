#!/usr/bin/env python3
"""Check the bench bridge's Ethernet MAC (#39) without printing it.

The original bridge's MAC lives outside git, in the owner's file (default
~/ib01-backups/original-bridge-mac.txt, one line, 6 bytes in hex). This tool
compares it with what the laptop sees and prints only IP addresses, times and
yes/no answers. The MAC is never printed, so the output may go into docs.
The placeholder MAC of #12 (02:7a:d0:b1:00:01, public in src/net.c) is named
when it is seen.

  mac_check.py neigh [dev]      ip neigh entries on dev (default enp196s0f0):
                                IP, state, and whether the lladdr is the file's
                                MAC, the placeholder or other
  mac_check.py lease [since]    dnsmasq-dhcp journal lines since `since`
                                (journalctl syntax, default "-2h") for the
                                file's MAC and the placeholder: time, message
                                type and IP
  mac_check.py log FILE...      whether any FILE holds the MAC in any common
                                spelling (plain hex, ':', '-', ' ', reversed)

Options: --mac-file FILE. Read-only: it changes nothing on the laptop or the
bridge. Run with python3 -I.
"""
import os
import re
import subprocess
import sys

PLACEHOLDER = "027ad0b10001"


def load_mac(path):
    lines = [l for l in open(path).read().splitlines() if l.strip()]
    if len(lines) != 1:
        sys.exit("mac_check: %s: want one line" % path)
    h = re.sub(r"[^0-9a-fA-F]", "", lines[0]).lower()
    if len(h) != 12:
        sys.exit("mac_check: %s: want 6 bytes in hex" % path)
    return h


def norm(s):
    h = re.sub(r"[^0-9a-fA-F]", "", s).lower()
    return h if len(h) == 12 else None


def kind(h, mac):
    if h == mac:
        return "the MAC file's MAC"
    if h == PLACEHOLDER:
        return "the placeholder 02:7a:d0:b1:00:01"
    return "another MAC"


def spellings(mac):
    b = [mac[i:i + 2] for i in range(0, 12, 2)]
    out = {mac}
    for seq in (b, b[::-1]):
        for sep in (":", "-", " ", ""):
            out.add(sep.join(seq))
    return out


def main(argv):
    mac_file = os.path.expanduser("~/ib01-backups/original-bridge-mac.txt")
    if "--mac-file" in argv:
        i = argv.index("--mac-file")
        mac_file = argv[i + 1]
        del argv[i:i + 2]
    if not argv:
        sys.exit(__doc__)
    mac = load_mac(mac_file)
    cmd, args = argv[0], argv[1:]
    if cmd == "neigh":
        dev = args[0] if args else "enp196s0f0"
        out = subprocess.run(["ip", "neigh", "show", "dev", dev], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            f = line.split()
            if "lladdr" not in f:
                print("%s: no lladdr (%s)" % (f[0], f[-1]))
                continue
            h = norm(f[f.index("lladdr") + 1])
            print("%s: %s, %s" % (f[0], f[-1], kind(h, mac)))
    elif cmd == "lease":
        since = args[0] if args else "-2h"
        out = subprocess.run(["journalctl", "--no-pager", "-o", "short-iso", "--since", since, "-t", "dnsmasq-dhcp"],
                             capture_output=True, text=True).stdout
        n = 0
        for line in out.splitlines():
            m = re.search(r"(DHCP[A-Z]+)\([^)]*\)\s+(?:(\d+\.\d+\.\d+\.\d+)\s+)?([0-9a-fA-F:]{17})", line)
            if not m:
                continue
            h = norm(m.group(3))
            if h not in (mac, PLACEHOLDER):
                continue
            n += 1
            print("%s %s %s: %s" % (line.split()[0], m.group(1), m.group(2) or "-", kind(h, mac)))
        if not n:
            print("no dnsmasq-dhcp line for the MAC file's MAC or the placeholder since %s" % since)
    elif cmd == "log":
        forms = spellings(mac)
        for path in args:
            t = open(path, errors="replace").read().lower()
            print("%s: MAC %s" % (path, "PRESENT" if any(s in t for s in forms) else "absent"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
