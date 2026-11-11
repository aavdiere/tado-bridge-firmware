#!/bin/sh
# Start OpenOCD with an RTT server on TCP 9090 (channel 0). Leave it running,
# then in another terminal:   nc localhost 9090     (or: telnet localhost 9090)
# RTT block is searched in RAM (0x20000000, 128 KB) by its "SEGGER RTT" ID.
# The target keeps running; this does not halt or reset it. Start it after the
# firmware has booted (it waits ~2 s first, then initialises RTT).
# Usage: tools/rtt.sh [elf]   (elf only used to read the cb address, optional)
set -eu
PORT="${RTT_PORT:-9090}"
exec openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
    -c "init" \
    -c "rtt setup 0x20000000 0x20000 \"SEGGER RTT\"" \
    -c "rtt start" \
    -c "rtt server start $PORT 0"
