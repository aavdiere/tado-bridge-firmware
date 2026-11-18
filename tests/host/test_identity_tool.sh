#!/usr/bin/env bash
# Host test of the identity provisioning tool (#21, tools/identity/identity.c)
# with made-up values (not a real bridge): extraction from a fake backup,
# slot choice on a stock image, read-back as the firmware loads it, refusal
# to replace a different identity, and "nothing to write" when it is there.
# Usage: test_identity_tool.sh <tool> <scratch dir>
set -euo pipefail
TOOL="$1"
T="$2"
rm -rf "$T"
mkdir -p "$T"
fails=0
checks=0
ok() { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL test_identity_tool: $*"; }
expect() { if eval "$1"; then ok; else bad "$1"; fi; }

blank() { head -c 4096 /dev/zero | LC_ALL=C tr '\0' '\377' >"$1"; }
noise() { head -c 4096 /dev/urandom >"$1"; }

# Fake internal-flash backup: 64 KB of 0xFF, the configuration page at
# 0x4000: marker at +6, serial u32 LE at +14 (1234567890 = 0x499602D2),
# EUI-64 at +50.
head -c 65536 /dev/zero | LC_ALL=C tr '\0' '\377' >"$T/backup.img"
printf '\x42\x49\x00\x00\x54\x46' | dd of="$T/backup.img" bs=1 seek=$((0x4006)) conv=notrunc status=none
printf '\xd2\x02\x96\x49' | dd of="$T/backup.img" bs=1 seek=$((0x400e)) conv=notrunc status=none
printf '\x00\x1b\xc5\x07\x00\x00\x00\x2a' | dd of="$T/backup.img" bs=1 seek=$((0x4032)) conv=notrunc status=none

out="$("$TOOL" from-backup "$T/backup.img")"
expect '[[ $out == "backup: serial IB.. (12 characters), EUI-64 ..:..:..:..:..:..:..:2a" ]]'
out="$("$TOOL" --show from-backup "$T/backup.img")"
expect '[[ $out == "backup: serial IB1234567890, EUI-64 00 1b c5 07 00 00 00 2a" ]]'
# Default output of every command carries no full serial or EUI-64.
out="$("$TOOL" from-backup "$T/backup.img" 2>&1)"
expect '[[ $out != *1234567890* && $out != *"00 1b c5"* ]]'

# Stock image: slot A holds stock data, slot B blank. The first record goes
# to slot A, the invalid one.
noise "$T/a.img"
blank "$T/b.img"
out="$("$TOOL" make --backup "$T/backup.img" "$T/a.img" "$T/b.img" "$T/new.img")"
expect '[[ $out == *"sector 0x100000"* && $out == *"record seq 1"* ]]'
expect '[[ $(stat -c %s "$T/new.img") == 4096 ]]'
expect '[[ $(head -c 4 "$T/new.img") == IDN1 ]]'
# The rest of the sector after the 40-byte record is blank.
expect '[[ $(tail -c +41 "$T/new.img" | LC_ALL=C tr -d "\377" | wc -c) == 0 ]]'
# As the firmware loads it, by both ways of naming the identity.
expect '"$TOOL" check --backup "$T/backup.img" "$T/new.img" "$T/b.img" >/dev/null'
expect '"$TOOL" check --serial IB1234567890 --eui64 "00:1b:c5:07:00:00:00:2a" "$T/new.img" "$T/b.img" >/dev/null'
expect '! "$TOOL" check --serial IB1234567891 --eui64 "00 1b c5 07 00 00 00 2a" "$T/new.img" "$T/b.img" >/dev/null 2>&1'
# The stock data itself is no identity.
expect '! "$TOOL" check --backup "$T/backup.img" "$T/a.img" "$T/b.img" >/dev/null 2>&1'

# Already there: nothing to write.
out="$("$TOOL" make --backup "$T/backup.img" "$T/new.img" "$T/b.img" "$T/again.img")"
expect '[[ $out == *"sector none"* && ! -e "$T/again.img" ]]'
# A different identity is refused without --replace, and goes to slot B
# with seq 2 with it.
expect '! "$TOOL" make --serial IB0000000001 --eui64 001bc5070000002b "$T/new.img" "$T/b.img" "$T/x.img" >/dev/null 2>&1'
out="$("$TOOL" make --replace --serial IB0000000001 --eui64 001bc5070000002b "$T/new.img" "$T/b.img" "$T/x.img")"
expect '[[ $out == *"sector 0x101000"* && $out == *"record seq 2"* ]]'
expect '"$TOOL" check --serial IB0000000001 --eui64 001bc5070000002b "$T/new.img" "$T/x.img" >/dev/null'

# Refused inputs.
expect '! "$TOOL" make --serial ib1 --eui64 001bc5070000002b "$T/a.img" "$T/b.img" "$T/y.img" >/dev/null 2>&1'
expect '! "$TOOL" make --serial IB1 --eui64 001bc6070000002b "$T/a.img" "$T/b.img" "$T/y.img" >/dev/null 2>&1'
expect '! "$TOOL" make --serial IB1 --eui64 001bc507000000 "$T/a.img" "$T/b.img" "$T/y.img" >/dev/null 2>&1'
head -c 4095 "$T/b.img" >"$T/short.img"
expect '! "$TOOL" make --backup "$T/backup.img" "$T/a.img" "$T/short.img" "$T/y.img" >/dev/null 2>&1'
expect '[[ ! -e "$T/y.img" ]]'
# A backup without the marker, and one with two markers that disagree.
blank "$T/nomark.img"
expect '! "$TOOL" from-backup "$T/nomark.img" >/dev/null 2>&1'
cp "$T/backup.img" "$T/two.img"
printf '\x42\x49\x00\x00\x54\x46\xff\xff\x01' | dd of="$T/two.img" bs=1 seek=$((0x8006)) conv=notrunc status=none
expect '! "$TOOL" from-backup "$T/two.img" >/dev/null 2>&1'

rm -rf "$T"
echo "test_identity_tool: $checks checks, $fails failed"
[[ $fails == 0 ]]
