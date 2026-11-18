#!/usr/bin/env bash
# Write the bridge's identity record (#21) over SWD: serial and EUI-64 from
# the P0 backup of the internal flash into the identity sectors of the
# external flash, by the firmware's own record rules (src/store.c).
#
#   tools/identity/provision.sh <workdir> --backup <internal.bin>
#   tools/identity/provision.sh <workdir> --serial IB0123456789 --eui64 "00 1b c5 .."
#   --replace right after <workdir>: replace a different valid identity
#   (refused without it; the identity is meant never to change)
#
# <workdir> must lie outside any git repository (for example
# ~/ib01-backups/identity-YYYY-MM-DD): it receives the sectors read from the
# bridge (slot A may hold stock data) and the new sector. Nothing of it may
# enter git (CLAUDE.md).
#
# Steps: build the host tool; check tanoclo's SPI stub against its address
# file; read both identity sectors (each until two reads agree); build the
# new sector with the tool (its messages go to <workdir>/make.log, masked;
# the full values need the tool's --show); ask before writing; write that one sector (the
# stub erases, programs and verifies every page); read both sectors again
# and check that the record loads as the firmware loads it; reset the
# bridge so it boots with the new record.
#
# Bench rules (docs/test-bench.md): USB power, ST-Link 3V3 wire off; OpenOCD
# only (no NRST, no connect-under-reset); no option bytes. Writes only
# 0x100000 or 0x101000 (tools/identity/identity.tcl refuses anything else).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STUB_DIR="${TANOCLO_PATCH:-$REPO/../tanoclo/patch_ib_firmware}"
TOOL="$REPO/build/host-tools/identity"
OCD=(openocd -f interface/stlink.cfg -f target/stm32f4x.cfg)

die() { echo "provision: $*" >&2; exit 1; }

[[ $# -ge 3 ]] || die "usage: $0 <workdir> [--replace] (--backup <internal.bin> | --serial S --eui64 HEX)"
WORK="$1"
shift
REPLACE=()
if [[ ${1:-} == --replace ]]; then
    REPLACE=(--replace)
    shift
fi
ID=("$@")

mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
if git -C "$WORK" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    die "$WORK is inside a git repository: use a folder outside git, e.g. ~/ib01-backups/"
fi

make -C "$REPO/tools/identity" --no-print-directory >/dev/null

# The stub's symbols must be where its address file says (the TCL script
# pokes those addresses).
[[ -f $STUB_DIR/spi_stub.elf && -f $STUB_DIR/spi_stub_addrs.tcl ]] || die "no SPI stub in $STUB_DIR (set TANOCLO_PATCH)"
sym() { arm-none-eabi-nm "$STUB_DIR/spi_stub.elf" | awk -v s="$1" '$3 == s { print "0x" $1 }'; }
tclv() { awk -v s="$1" '$1 == "set" && $2 == s { print $3 }' "$STUB_DIR/spi_stub_addrs.tcl"; }
for pair in STUB_ENTRY:stub_entry BUF_ADDR:g_buf G_LEN:g_flash_len G_ADDR:g_flash_addr G_OP:g_op G_OUT:g_out; do
    v="${pair%%:*}" s="${pair##*:}"
    [[ -n $(sym "$s") && $(( $(sym "$s") )) -eq $(( $(tclv "$v") )) ]] || die "spi_stub.elf: $s is not at $v in spi_stub_addrs.tcl"
done

ocd() { "${OCD[@]}" -c "set ID_STUB_DIR {$STUB_DIR}" "$@" -f "$REPO/tools/identity/identity.tcl"; }

echo "provision: reading the identity sectors"
mkdir -p "$WORK/before" "$WORK/after"
ocd -c "set ID_MODE read; set ID_DIR {$WORK/before}"

OUT="$WORK/sector.bin"
rm -f "$OUT"
# The tool's messages (masked identity) go to the log; the terminal gets the
# sector line and the masked new-record line only.
LOG="$WORK/make.log"
"$TOOL" make "${REPLACE[@]}" "${ID[@]}" "$WORK/before/slot_a.bin" "$WORK/before/slot_b.bin" "$OUT" >"$LOG" || { cat "$LOG" >&2; die "identity tool refused (see above)"; }
MSG="$(cat "$LOG")"
grep '^sector ' "$LOG"
ADDR="$(awk '$1 == "sector" { print $2 }' <<<"$MSG")"
if [[ $ADDR == none ]]; then
    echo "provision: the bridge already holds this identity: nothing written"
    exit 0
fi
[[ $ADDR == 0x100000 || $ADDR == 0x101000 ]] || die "unexpected sector '$ADDR'"
[[ $ADDR == 0x100000 ]] && echo "provision: note: slot A (0x100000) holds the stock data in a stock image; it is replaced (the backup keeps it)"

echo "provision: $(grep '^new record:' "$LOG")"
read -r -p "provision: write this identity to sector $ADDR now? [y/N] " yes
[[ $yes == y || $yes == Y ]] || die "not written"

ocd -c "set ID_MODE write; set ID_ADDR $ADDR; set ID_FILE {$OUT}"

echo "provision: reading back"
ocd -c "set ID_MODE read; set ID_DIR {$WORK/after}"
"$TOOL" check "${ID[@]}" "$WORK/after/slot_a.bin" "$WORK/after/slot_b.bin" >"$WORK/check.log" || { cat "$WORK/check.log" >&2; die "read-back check failed"; }
grep '^identity record OK' "$WORK/check.log"
# The sector not written must be as it was.
if [[ $ADDR == 0x100000 ]]; then other=slot_b.bin; mine=slot_a.bin; else other=slot_a.bin; mine=slot_b.bin; fi
cmp -s "$WORK/before/$other" "$WORK/after/$other" || die "the other slot changed"
cmp -s "$OUT" "$WORK/after/$mine" || die "the written sector does not read back as built"

echo "provision: done; resetting the bridge"
"${OCD[@]}" -c "init; reset run; shutdown"
