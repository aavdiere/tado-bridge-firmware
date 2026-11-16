#!/usr/bin/env bash
# Regenerates tests/host/vectors_freedo.h by running freedo's Go code (#17).
# Needs Go and the freedo checkout next to this repository (../freedo); the
# generator runs inside freedo's module so that its packages resolve.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
freedo=${FREEDO_DIR:-$here/../../../freedo}
rev=$(git -C "$freedo" rev-parse --short HEAD)
tmp=$(mktemp)
(cd "$freedo" && go run "$here/gen_vectors.go") >"$tmp"
{
  echo "/* freedo revision $rev. */"
  cat "$tmp"
} >"$here/vectors_freedo.h"
rm -f "$tmp"
echo "wrote $here/vectors_freedo.h (freedo $rev)"
