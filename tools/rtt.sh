#!/bin/sh
# Start OpenOCD with an RTT server on TCP 19021 (channel 0). Leave it running,
# then in another terminal:   nc localhost 19021     (or: telnet localhost 19021)
# RTT block is searched in RAM (0x20000000, 128 KB) by its "SEGGER RTT" ID.
# The target keeps running; this does not halt or reset it.
# Safe to start during the firmware's 2 s startup delay: OpenOCD 0.12 searches
# for the control block only once per `rtt start` (verified on the bench
# 2026-11-11), so the proc below retries every 200 ms for up to 10 s.
# Usage: tools/rtt.sh        (port: $RTT_PORT, default 19021)
set -eu
PORT="${RTT_PORT:-19021}"
exec openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
    -c "init" \
    -c "rtt setup 0x20000000 0x20000 \"SEGGER RTT\"" \
    -c "proc rtt_wait {} {
          for {set i 0} {\$i < 50} {incr i} {
            catch {rtt start}
            if {![catch {rtt channels}]} { return }
            catch {rtt stop}
            sleep 200
          }
          echo \"rtt: no control block after 10 s; is the firmware running?\"
        }" \
    -c "rtt_wait" \
    -c "rtt server start $PORT 0"
