# ENC28J60 bus probe over SWD (#12). Halts the core (the IWDG is frozen while
# halted, docs/build.md), runs SPI2 transactions by hand, restores every
# register it touched, and resumes. Usage:
#   openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c init \
#       -f tools/bench/enc28j60_probe.tcl -c shutdown
# Needs a running image that has set up SPI2 (P3 #12 or later).
#
# 1. SCK sweep: reads ERXSTL, ERXSTH, ERXNDL, ERXNDH (bank 0) 20 times per
#    SPI2 divider; the reset values are fa 05 ff 1f (datasheets/enc28j60.txt
#    register table), so "fa05ff1f 20" is a clean bus.
# 2. EREVID (bank 3, 0x12) at the slowest clock: 06 = B7 (errata Table 1).
# 3. Free-pin sweep: each listed pin low on its own (PB12 high), MISO pulled
#    up; any result other than ffffffff means that pin selects something.
# Never touches PA13/PA14 (SWD), PB6/PB7 (HomeKit I2C), PA0/PA1, PA4-PA7,
# PB9 (radio and flash), PB2 (ENC INT), PC14 (ENC RESET) or PH0/PH1 (HSE).
set base(A) 0x40020000
set base(B) 0x40020400
set base(C) 0x40020800
set SPI2    0x40003800
set free_pins {{A 2} {A 3} {A 8} {A 9} {A 10} {A 11} {A 12} {A 15} {B 3} {B 4} {C 13}}

proc field {port pin off val} {
    global base
    set a [expr {$base($port) + $off}]
    set v [mrw $a]
    mww $a [expr {(($v & ~(3 << (2*$pin))) | ($val << (2*$pin))) & 0xffffffff}]
}
proc bsrr {port pin level} {
    global base
    if {$level} { mww [expr {$base($port)+0x18}] [expr {1 << $pin}] } \
    else        { mww [expr {$base($port)+0x18}] [expr {1 << ($pin+16)}] }
}
proc xfer {b} {
    global SPI2
    mrw [expr {$SPI2+0x0c}]
    mww [expr {$SPI2+0x0c}] $b
    for {set i 0} {$i < 100} {incr i} { if {[mrw [expr {$SPI2+0x08}]] & 1} break }
    return [expr {[mrw [expr {$SPI2+0x0c}]] & 0xff}]
}
proc op {bytes} {
    bsrr B 12 0
    set r 0
    foreach b $bytes { set r [xfer $b] }
    bsrr B 12 1
    return $r
}
proc regs {} {
    set out ""
    foreach r {0x08 0x09 0x0a 0x0b} { append out [format %02x [op [list $r 0]]] }
    return $out
}
proc hist {n} {
    set h [dict create]
    for {set k 0} {$k < $n} {incr k} { dict incr h [regs] }
    return $h
}
proc set_br {cr1 br} {
    global SPI2
    mww $SPI2 [expr {$cr1 & ~0x40}]
    mww $SPI2 [expr {(($cr1 & ~0x78) | ($br << 3)) | 0x40}]
}

halt
foreach p {A B C} { foreach off {0x00 0x0c 0x14} { set save($p,$off) [mrw [expr {$base($p)+$off}]] } }
set cr1 [mrw $SPI2]
set apb1_khz 48000
field B 14 0x0c 1 ;# MISO pull-up: an undriven line reads ff

# 1. SCK sweep
foreach br {1 2 3 5 7} {
    set_br $cr1 $br
    echo [format "SCK %5d kHz: %s" [expr {$apb1_khz / (2 << $br)}] [hist 20]]
}
# 2. EREVID at the slowest setting (br 7 still set)
op {0x9f 0x03}
set rev [format %02x [op {0x12 0}]]
op {0xbf 0x03}
echo "EREVID $rev (06 = B7)"
# 3. Free-pin sweep at 3 MHz
set_br $cr1 3
foreach c $free_pins {
    lassign $c port pin
    field $port $pin 0x00 0
    field $port $pin 0x0c 1; set up [expr {([mrw [expr {$base($port)+0x10}]] >> $pin) & 1}]
    field $port $pin 0x0c 2; set dn [expr {([mrw [expr {$base($port)+0x10}]] >> $pin) & 1}]
    field $port $pin 0x0c 0
    if {!($up == 1 && $dn == 0)} { echo "P$port$pin is driven by something (pull-up $up, pull-down $dn): skipped"; continue }
    bsrr $port $pin 1; field $port $pin 0x00 1
    set h [dict create]
    for {set k 0} {$k < 5} {incr k} {
        bsrr $port $pin 0
        xfer 0x08
        dict incr h [format %02x [xfer 0]]
        bsrr $port $pin 1
    }
    field $port $pin 0x00 0
    echo "P$port$pin low as CS: $h"
}

# Restore
mww $SPI2 [expr {$cr1 & ~0x40}]
mww $SPI2 $cr1
foreach p {A B C} {
    mww [expr {$base($p)+0x14}] $save($p,0x14)
    mww [expr {$base($p)+0x0c}] $save($p,0x0c)
    mww [expr {$base($p)+0x00}] $save($p,0x00)
}
set ok 1
foreach p {A B C} { foreach off {0x00 0x0c 0x14} {
    if {[mrw [expr {$base($p)+$off}]] != $save($p,$off)} { set ok 0; echo "RESTORE MISMATCH GPIO$p +$off" }
} }
if {[mrw $SPI2] != $cr1} { set ok 0; echo "RESTORE MISMATCH SPI2_CR1" }
echo [expr {$ok ? "restored" : "NOT fully restored: reset the board"}]
resume
