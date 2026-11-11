# Bench I/O check for the IB01: drives the four LEDs one at a time and logs
# button presses as pin changes. Peripheral registers only; nothing is written
# to flash, and the final reset undoes every change. PA13/PA14 (SWD), PC14
# (ENC28J60 reset) and PA0/PA1 (radio GDO outputs) are not touched or watched.
#
#   openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "set DURATION_MS 180000" \
#     -f tools/bench/io_check.tcl
#
# LED order, 2 s each, repeating every 12 s: power PB1, router PB0, cloud PB5,
# pairing PB8, all four on, all off.

if {![info exists DURATION_MS]} { set DURATION_MS 180000 }
init
reset halt

# GPIOA, GPIOB, GPIOC clocks on (RCC_AHB1ENR)
mmw 0x40023830 0x7 0
# Pull-downs on free candidate pins; pull-up on PB10 (factory reset button, active low)
mmw 0x4002000C 0x82AA00A0 0x41550050
mmw 0x4002040C 0x0000A280 0x00005140
mmw 0x4002040C 0x00100000 0x00200000
mmw 0x4002080C 0x88000000 0x44000000
# LED pins PB0, PB1, PB5, PB8: push-pull outputs, no pull, all off
mmw 0x4002040C 0 0x00030C0F
mww 0x40020418 0x01230000
mmw 0x40020400 0x00010405 0x0002080A

proc idr {addr} { return [lindex [read_memory $addr 32 1] 0] }
proc led_step {k} {
    set off 0x01230000
    switch $k {
        0 { mww 0x40020418 [expr {$off | (1<<1)}] ; return "power PB1" }
        1 { mww 0x40020418 [expr {$off | (1<<0)}] ; return "router PB0" }
        2 { mww 0x40020418 [expr {$off | (1<<5)}] ; return "cloud PB5" }
        3 { mww 0x40020418 [expr {$off | (1<<8)}] ; return "pairing PB8" }
        4 { mww 0x40020418 0x00000123 ; return "all on" }
        default { mww 0x40020418 $off ; return "all off" }
    }
}

set masks [dict create A 0x9F0C B 0x04DC C 0xA000]
set addrs [dict create A 0x40020010 B 0x40020410 C 0x40020810]
set prev [dict create]
foreach p {A B C} { dict set prev $p [expr {[idr [dict get $addrs $p]] & [dict get $masks $p]}] }
puts [format "IOCHECK start A=0x%04X B=0x%04X C=0x%04X" [dict get $prev A] [dict get $prev B] [dict get $prev C]]
set t0 [ms]
set last_step -1
while {[expr {[ms] - $t0}] < $DURATION_MS} {
    set t [expr {[ms] - $t0}]
    set step [expr {($t / 2000) % 6}]
    if {$step != $last_step} { puts [format "IOCHECK %6d ms LED %s" $t [led_step $step]]; set last_step $step }
    foreach p {A B C} {
        set v [expr {[idr [dict get $addrs $p]] & [dict get $masks $p]}]
        set d [expr {$v ^ [dict get $prev $p]}]
        if {$d} {
            for {set i 0} {$i < 16} {incr i} {
                if {$d & (1<<$i)} { puts [format "IOCHECK %6d ms P%s%d -> %d" $t $p $i [expr {($v >> $i) & 1}]] }
            }
            dict set prev $p $v
        }
    }
    sleep 50
}
mww 0x40020418 0x01230000
puts "IOCHECK done"
reset halt
shutdown
