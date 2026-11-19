#!/usr/bin/env bash
# Host test of the update uploader (#24, tools/update/upload.py) against the
# firmware's own parsers: test_fwupd writes a synthetic application image,
# the tool turns it into the WebSocket frames it would send through
# tanoclo's send-raw, and test_fwupd takes those frames apart as
# src/session.c does and hands them to src/fwupd.c, which must end with the
# image verified in the candidate slot and PENDING written (then installed
# by the real bootloader decisions). Also: the tool refuses images the
# bridge would refuse, and its header agrees with src/fwimg.c byte for byte.
# Usage: test_upload_tool.sh <test_fwupd program> <scratch dir>
set -euo pipefail
PROG="$1"
T="$2"
TOOL="$(dirname "$0")/../../tools/update/upload.py"
rm -rf "$T"
mkdir -p "$T"
fails=0
checks=0
ok() { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL test_upload_tool: $*"; }
expect() { if eval "$1"; then ok; else bad "$1"; fi; }
up() { python3 -I "$TOOL" "$@"; }

IPV6=fe80::21b:c5ff:fe07:2a
IPV6HEX=fe80000000000000021bc5fffe07002a

for size in 30724 4096 65536 1028; do
    "$PROG" --write-image "$T/img.bin" 77 "$size"
    expect "up info '$T/img.bin' >'$T/info.txt'"
    expect "grep -q '^image .*: $size bytes' '$T/info.txt'"
    expect "up frames '$T/img.bin' '$T/frames' --ipv6 $IPV6"
    if "$PROG" --feed-frames "$T/img.bin" "$T/frames" "$IPV6HEX" >"$T/feed.txt"; then ok; else
        bad "feed-frames size $size: $(cat "$T/feed.txt")"
    fi
    # The body: header block then the image, nothing else.
    expect "up body '$T/img.bin' '$T/body'"
    expect "[[ \$(stat -c %s '$T/body') == $((1024 + size)) ]]"
    expect "cmp -s <(tail -c +1025 '$T/body') '$T/img.bin'"
    expect "[[ \$(head -c 4 '$T/body') == TFW1 ]]"
    expect "[[ \$(head -c 1024 '$T/body' | tail -c +65 | LC_ALL=C tr -d '\\000' | wc -c) == 0 ]]"
done

# Refused by the tool as by the bridge: not a multiple of 4, too large,
# no descriptor.
"$PROG" --write-image "$T/img.bin" 5 8192
head -c 8190 "$T/img.bin" >"$T/odd.bin"
expect "! up info '$T/odd.bin' >/dev/null 2>&1"
head -c $((448 * 1024 + 4)) /dev/zero >"$T/big.bin"
expect "! up info '$T/big.bin' >/dev/null 2>&1"
head -c 8192 /dev/urandom >"$T/noise.bin"
expect "! up info '$T/noise.bin' >/dev/null 2>&1"

echo "test_upload_tool: $checks checks, $fails failed"
[[ $fails == 0 ]]
