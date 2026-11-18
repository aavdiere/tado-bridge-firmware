#!/usr/bin/env bash
# Flash ELF images to the IB01 over ST-Link with OpenOCD: each one is
# programmed (erasing only the sectors it covers) and verified, then the
# board is reset. No NRST on the test points: no st-flash, no
# connect-under-reset; OpenOCD resets through SYSRESETREQ
# (target/stm32f4x.cfg). Does not touch option bytes.
#
# Usage: tools/flash.sh [elf ...]
# Default, since P7 (#24): the bootloader (0x08000000, sectors 0-3) and the
# application (0x08010000, sectors 4-7) together, so the two always match.
# The application alone (tools/flash.sh build/tado-bridge.elf) is fine once
# a P7 bootloader is in sector 0; on a board still holding a pre-P7 image
# (linked at 0x08000000) it would leave that image's start in sectors 0-3
# with its code in sectors 4-5 overwritten, which does not boot (SWD still
# works: flash both). See docs/build.md "Bootloader and application (#24)".
set -euo pipefail
cd "$(dirname "$0")/.."
if [ $# -eq 0 ]; then
    set -- build/tado-bridge-boot.elf build/tado-bridge.elf
fi
args=()
i=0
for elf in "$@"; do
    i=$((i + 1))
    [ -f "$elf" ] || { echo "flash.sh: $elf not found" >&2; exit 1; }
    if [ "$i" -eq $# ]; then
        args+=(-c "program $elf verify reset exit")
    else
        args+=(-c "program $elf verify")
    fi
done
exec openocd -f interface/stlink.cfg -f target/stm32f4x.cfg "${args[@]}"
