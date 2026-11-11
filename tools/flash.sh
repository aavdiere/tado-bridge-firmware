#!/bin/sh
# Flash an ELF to the IB01 over ST-Link with OpenOCD: program, verify, reset.
# No NRST on the test points: no st-flash, no connect-under-reset.
# Does not touch option bytes. Usage: tools/flash.sh [build/tado-bridge.elf]
set -eu
cd "$(dirname "$0")/.."
ELF="${1:-build/tado-bridge.elf}"
exec openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
    -c "program $ELF verify reset exit"
