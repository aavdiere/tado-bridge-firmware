#!/usr/bin/env bash
# Host test of the network-channel build option (stage P7.1, src/txgate.h,
# cmake/CMakeLists.txt, Makefile): channel 26, the protected channel of the
# owner's stock network, builds only with the second opt-in
# RADIO_ALLOW_CH26=1, at all three layers (the C headers, the Makefile and
# CMake), so that no one-word slip builds a channel-26 transmitter. Also:
# the opt-in without channel 26, a neighbour of channel 26, a value off the
# plan and a source compiled without the definitions do not build; the
# default channel 16 and channel 26 with the opt-in do.
# Nothing here builds an image: the compiler runs -fsyntax-only, make runs
# -n on `clean`, and CMake stops before project() for a refused value.
# Usage: test_channel_build.sh <scratch dir>
set -euo pipefail
T="$1"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-gcc}"
rm -rf "$T"
mkdir -p "$T"
fails=0
checks=0
ok() { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL test_channel_build: $*"; }

# cc <expect: pass|fail> <grep pattern or ""> <file> <defines...>
cc() {
    local want="$1" pat="$2" file="$3"
    shift 3
    local rc=0
    "$CC" -std=c11 -fsyntax-only -Wall -Wextra -Werror -Wundef -I"$ROOT/src" \
        -I"$ROOT/tests/host" -I"$ROOT/vendor/mbedtls/include" \
        -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"' -DRADIO_TX_ALLOWED=1 \
        "$@" "$ROOT/src/$file" >"$T/cc.txt" 2>&1 || rc=$?
    if [[ $want == pass && $rc == 0 ]] || [[ $want == fail && $rc != 0 ]]; then
        if [[ -n $pat ]] && ! grep -q -- "$pat" "$T/cc.txt"; then
            bad "cc $file $*: no '$pat' in: $(head -c 400 "$T/cc.txt")"
        else
            ok
        fi
    else
        bad "cc $file $*: expected $want, rc $rc: $(head -c 400 "$T/cc.txt")"
    fi
}

# The gate itself, and the store (also compiled into the bootloader).
for f in txgate.c store.c; do
    cc pass "" "$f" -DRADIO_NET_CHANNEL=16 -DRADIO_ALLOW_CH26=0
    cc pass "" "$f" -DRADIO_NET_CHANNEL=26 -DRADIO_ALLOW_CH26=1
    cc fail "protected channel" "$f" -DRADIO_NET_CHANNEL=26 -DRADIO_ALLOW_CH26=0
    cc fail "RADIO_NET_CHANNEL comes from the build" "$f" -DRADIO_ALLOW_CH26=0
    cc fail "RADIO_ALLOW_CH26 comes from the build" "$f" -DRADIO_NET_CHANNEL=16
    cc fail "for a channel-26 build only" "$f" -DRADIO_NET_CHANNEL=16 -DRADIO_ALLOW_CH26=1
    cc fail "must be 0 or 1" "$f" -DRADIO_NET_CHANNEL=26 -DRADIO_ALLOW_CH26=2
    cc fail "0-49" "$f" -DRADIO_NET_CHANNEL=50 -DRADIO_ALLOW_CH26=0
done
# A neighbour of channel 26 passes the header but not the gate's own check.
cc fail "at least 5 channels" txgate.c -DRADIO_NET_CHANNEL=25 -DRADIO_ALLOW_CH26=0
cc fail "at least 5 channels" txgate.c -DRADIO_NET_CHANNEL=22 -DRADIO_ALLOW_CH26=0
cc pass "" txgate.c -DRADIO_NET_CHANNEL=21 -DRADIO_ALLOW_CH26=0

# The Makefile: refused at parse time, before anything runs.
mk() {
    local want="$1" pat="$2"
    shift 2
    local rc=0
    make -n --no-print-directory -C "$ROOT" clean BUILD_DIR="$T/build" "$@" >"$T/mk.txt" 2>&1 || rc=$?
    if [[ $want == pass && $rc == 0 ]] || [[ $want == fail && $rc != 0 ]]; then
        if [[ -n $pat ]] && ! grep -q -- "$pat" "$T/mk.txt"; then
            bad "make $*: no '$pat' in: $(head -c 400 "$T/mk.txt")"
        else
            ok
        fi
    else
        bad "make $*: expected $want, rc $rc: $(head -c 400 "$T/mk.txt")"
    fi
}
mk pass ""
mk pass "" RADIO_NET_CHANNEL=26 RADIO_ALLOW_CH26=1
mk fail "protected channel" RADIO_NET_CHANNEL=26
mk fail "protected channel" RADIO_NET_CHANNEL=26 RADIO_ALLOW_CH26=0
mk fail "RADIO_NET_CHANNEL=26 only" RADIO_ALLOW_CH26=1
mk fail "must be 0 or 1" RADIO_NET_CHANNEL=26 RADIO_ALLOW_CH26=yes

# CMake: a refused value stops the configure before project(), so no
# toolchain is needed and nothing is built.
cm() {
    local pat="$1"
    shift
    local rc=0
    rm -rf "$T/cmake"
    cmake -S "$ROOT/cmake" -B "$T/cmake" "$@" >"$T/cm.txt" 2>&1 || rc=$?
    if [[ $rc != 0 ]] && grep -q -- "$pat" "$T/cm.txt"; then
        ok
    else
        bad "cmake $*: expected a refusal with '$pat', rc $rc: $(head -c 600 "$T/cm.txt")"
    fi
}
cm "protected channel" -DRADIO_NET_CHANNEL=26
cm "protected channel" -DRADIO_NET_CHANNEL=26 -DRADIO_ALLOW_CH26=OFF
cm "for RADIO_NET_CHANNEL=26 only" -DRADIO_ALLOW_CH26=ON
cm "within 5 channels" -DRADIO_NET_CHANNEL=25
cm "within 5 channels" -DRADIO_NET_CHANNEL=22
cm "0-49" -DRADIO_NET_CHANNEL=50
cm "0-49" -DRADIO_NET_CHANNEL=016
cm "0-49" -DRADIO_NET_CHANNEL=x
cm "must be ON or OFF" -DRADIO_NET_CHANNEL=26 -DRADIO_ALLOW_CH26=maybe
rm -rf "$T/cmake"

echo "test_channel_build: $checks checks, $fails failed"
[[ $fails == 0 ]]
