#!/usr/bin/env bash
# Refuses firmware images, flash dumps, keys and other binaries in git.
# A dump of a real IB01 holds its network key, so it must never be committed.
#
#   tools/check-no-firmware.sh --staged    files staged for the next commit
#   tools/check-no-firmware.sh --tracked   all files at HEAD
#   tools/check-no-firmware.sh --history   every file version in the whole history
#
# Exits non-zero and lists the reason for each refused file.
set -euo pipefail

MAX_BYTES=262144
# Paths where binary files are allowed (documentation images only).
ALLOWED_BINARY='^docs/images/[^/]+\.(png|jpg|jpeg|svg)$'
BAD_EXT='\.(bin|hex|elf|srec|s19|dfu|uf2|img|dump|raw|sal|cu8|cs8|cs16|cf32|iq|wav|sigmf-data|sigmf-meta|pem|key|der|cer|crt|csr|srl|p12)$'
BAD_NAME='(^|/)(unmodded[^/]*|ib01_[^/]*)$|(^|/)(original|out|out_old|dumps|backup[^/]*)/'
# Text signs of a dump: hexdump or xxd style lines (offset then hex bytes), and
# blocks of raw hex such as `xxd -p` output. A single hash or key line does not trigger.
HEXDUMP_LINE='^[0-9a-fA-F]{7,8}:? +([0-9a-fA-F]{2,4} +){7,}'
RAW_HEX_LINE='^[0-9a-fA-F]{48,}$'
RAW_HEX_MAX=3

fail=0
refuse() { echo "check-no-firmware: refused $1: $2" >&2; fail=1; }

check_blob() { # $1 = path, $2 = blob id
  local path=$1 blob=$2 size
  [[ $path =~ $BAD_EXT ]] && refuse "$path" "firmware, dump or key file type"
  [[ $path =~ $BAD_NAME ]] && refuse "$path" "dump or backup location"
  [[ $path == tools/check-no-firmware.sh ]] && return 0
  size=$(git cat-file -s "$blob")
  (( size > MAX_BYTES )) && refuse "$path" "larger than $MAX_BYTES bytes"
  (( size == 0 )) && return 0
  # Binary: any NUL byte. Checked on the whole file, independent of locale.
  if (( $(git cat-file -p "$blob" | LC_ALL=C tr -d -c '\000' | wc -c) > 0 )); then
    [[ $path =~ $ALLOWED_BINARY ]] || refuse "$path" "binary content"
    return 0
  fi
  if git cat-file -p "$blob" | LC_ALL=C grep -Eq "$HEXDUMP_LINE"; then
    refuse "$path" "contains hexdump-style lines"
  fi
  if (( $(git cat-file -p "$blob" | LC_ALL=C grep -Ec "$RAW_HEX_LINE" || true) > RAW_HEX_MAX )); then
    refuse "$path" "contains a block of raw hex"
  fi
  return 0
}

case "${1:-}" in
  --staged)
    while IFS=$'\t' read -r meta path; do
      blob=$(awk '{print $4}' <<<"$meta")
      check_blob "$path" "$blob"
    done < <(git diff --cached --raw --no-renames --diff-filter=ACM --no-abbrev)
    ;;
  --tracked)
    while read -r _ _ blob path; do check_blob "$path" "$blob"; done < <(git ls-tree -r HEAD)
    ;;
  --history)
    while read -r blob path; do
      [[ -n ${path:-} ]] || continue
      [[ $(git cat-file -t "$blob") == blob ]] && check_blob "$path" "$blob"
    done < <(git rev-list --objects --all)
    ;;
  *) echo "usage: $0 --staged | --tracked | --history" >&2; exit 2 ;;
esac

if (( fail )); then
  echo "check-no-firmware: commit refused. Firmware and dumps must never enter git." >&2
  exit 1
fi
