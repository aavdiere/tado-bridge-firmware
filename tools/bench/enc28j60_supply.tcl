# ENC28J60 supply check over SWD (#31). Halts the core between two SPI2
# transactions (PB12 high; the IWDG is frozen while halted, docs/build.md),
# measures VDDA with the MCU's VREFINT and counts good SPI2 reads per SCK,
# first with the ENC28J60 PHY running, then with the chip in power save
# (ECON2.PWRSV, standby 1.2 mA, datasheets/enc28j60.txt:4022; ETH registers
# stay readable, :3762-3765), then running again. Restores every register it
# touched, the chip's bank select and RXEN included, and resumes. Usage:
#   openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c init \
#       -f tools/bench/enc28j60_supply.tcl -c shutdown
# Needs a running image that has set up SPI2 (P3 #12 or later).
#
# VDDA = 3.3 V * VREFIN_CAL / reading (VREFIN_CAL taken at VDDA 3.3 V,
# datasheets/stm32f411ce.txt:6903-6906). ADC1 at PCLK2/4 = 24 MHz (ADCPRE,
# rm0383.txt:12812-12818), channel 17 sampled 480 cycles = 20 us (minimum
# 10 us, stm32f411ce.txt:6890). The ENC28J60 needs VDD 3.10-3.60 V
# (enc28j60.txt:3988).
#
# A good read is ERXSTL..ERXNDH equal to the reference read at 187 kHz, which
# must be one of two known patterns: the reset values fa 05 ff 1f
# (enc28j60.txt:771-774), before any bring-up, or the firmware's own 00 00 ff
# 19 (RXST/RXND, src/enc28j60.c), after a successful bring-up.
#
# Power save follows the datasheet sequence (:3749-3770): RXEN off, wait for
# RXBUSY and TXRTS clear, PWRSV; then PWRSV off, wait for CLKRDY, RXEN back.
# The PHY drops the link meanwhile; the firmware sees a link change.
# Never touches PA13/PA14 (SWD), PB6/PB7 (HomeKit I2C), the radio or the flash.
set GB      0x40020400
set SPI2    0x40003800
set ADC     0x40012000
set CCR     0x40012304
set APB2ENR 0x40023844

proc cs {level} {
    global GB
    mww [expr {$GB+0x18}] [expr {$level ? (1 << 12) : (1 << 28)}]
}
proc xfer {b} {
    global SPI2
    mrw [expr {$SPI2+0x0c}]
    mww [expr {$SPI2+0x0c}] $b
    for {set i 0} {$i < 100} {incr i} { if {[mrw [expr {$SPI2+0x08}]] & 1} break }
    return [expr {[mrw [expr {$SPI2+0x0c}]] & 0xff}]
}
proc op {bytes} {
    cs 0
    set r 0
    foreach b $bytes { set r [xfer $b] }
    cs 1
    return $r
}
proc rd {a} { return [format %02x [op [list $a 0]]] }
proc regs {} {
    set out ""
    foreach r {0x08 0x09 0x0a 0x0b} { append out [rd $r] }
    return $out
}
proc set_br {br} {
    global SPI2 cr1
    mww $SPI2 [expr {$cr1 & ~0x40}]
    mww $SPI2 [expr {(($cr1 & ~0x78) | ($br << 3)) | 0x40}]
}
proc count {br n} {
    global want
    set_br $br
    set good 0
    for {set k 0} {$k < $n} {incr k} { if {[regs] eq $want} { incr good } }
    set_br 7
    return "$good/$n"
}
proc vdda {} {
    global ADC
    set cal [expr {([mrw 0x1FFF7A28] >> 16) & 0xffff}]
    set sum 0
    for {set k 0} {$k < 8} {incr k} {
        mww [expr {$ADC+0x08}] [expr {1 | (1 << 30)}]
        for {set i 0} {$i < 50} {incr i} { if {[mrw $ADC] & 2} break }
        incr sum [expr {[mrw [expr {$ADC+0x4c}]] & 0xfff}]
    }
    return [format "%.2f V" [expr {3.3 * $cal * 8.0 / $sum}]]
}
proc phase {name} {
    echo "$name: ESTAT [rd 0x1d] ECON2 [rd 0x1e], VDDA [vdda]; good reads: 12 MHz [count 1 40], 6 MHz [count 2 40], 3 MHz [count 3 40], 1.5 MHz [count 4 40]"
}
proc wait_bit {reg mask want what} {
    for {set i 0} {$i < 50} {incr i} {
        if {([expr 0x[rd $reg]] & $mask) == $want} { return 1 }
        sleep 1
    }
    echo "WARNING: $what"
    return 0
}

# Halt between two firmware transactions: CS (PB12 ODR) high means no SPI2
# transfer is open (src/enc28j60.c cs_high() raises CS only after BSY clears).
set halted 0
for {set t 0} {$t < 20} {incr t} {
    halt
    if {[mrw [expr {$GB+0x14}]] & (1 << 12)} { set halted 1; break }
    resume
    sleep 3
}
if {!$halted} {
    echo "ERROR: CS never high at halt; nothing changed"
    resume
    return
}
set save_moder [mrw $GB]
set save_pupdr [mrw [expr {$GB+0x0c}]]
set cr1        [mrw $SPI2]
set save_apb2  [mrw $APB2ENR]
mww $APB2ENR [expr {$save_apb2 | (1 << 8)}]
set save_ccr   [mrw $CCR]
foreach off {0x04 0x08 0x0c 0x2c 0x34} { set sadc($off) [mrw [expr {$ADC+$off}]] }
mww $CCR [expr {($save_ccr & ~(3 << 16)) | (1 << 16) | (1 << 23)}]
mww [expr {$ADC+0x0c}] [expr {7 << 21}]
mww [expr {$ADC+0x2c}] 0
mww [expr {$ADC+0x34}] 17
mww [expr {$ADC+0x08}] 1
sleep 1
mww [expr {$GB+0x0c}] [expr {($save_pupdr & ~(3 << 28)) | (1 << 28)}] ;# MISO pull-up
set_br 7          ;# 187 kHz for the reference and the control writes
set econ1 [expr 0x[rd 0x1f]]
op {0xbf 0x03}    ;# BFC ECON1 BSEL: bank 0
set want [regs]
switch -- $want {
    fa05ff1f { echo "reference ERXST/ERXND $want: reset values (no bring-up yet)" }
    0000ff19 { echo "reference ERXST/ERXND $want: the firmware's RXST/RXND (bring-up done)" }
    default  { echo "WARNING: reference ERXST/ERXND $want is neither fa05ff1f nor 0000ff19; counting against fa05ff1f"
               set want fa05ff1f }
}

phase "PHY on     "
set rxen [expr {$econ1 & 0x04}]
if {$rxen} { op {0xbf 0x04} }           ;# BFC ECON1 RXEN
wait_bit 0x1d 0x04 0 "ESTAT.RXBUSY still set"
if {[wait_bit 0x1f 0x08 0 "ECON1.TXRTS still set; power save skipped"]} {
    op {0x9e 0x20}                      ;# BFS ECON2 PWRSV
    sleep 100
    phase "power save "
    op {0xbe 0x20}                      ;# BFC ECON2 PWRSV
    wait_bit 0x1d 0x01 1 "ESTAT.CLKRDY not set after leaving power save"
    sleep 100
}
if {$rxen} { op {0x9f 0x04} }           ;# BFS ECON1 RXEN
phase "PHY on     "
if {[rd 0x1e] ne "80"} { echo "WARNING: ECON2 not back to 80; reset the board" }
op {0xbf 0x03}
if {$econ1 & 0x03} { op [list 0x9f [expr {$econ1 & 0x03}]] }  ;# firmware's bank back
set econ1_now [expr 0x[rd 0x1f]]

mww [expr {$ADC+0x08}] 0
foreach off {0x04 0x08 0x0c 0x2c 0x34} { mww [expr {$ADC+$off}] $sadc($off) }
mww $CCR $save_ccr
mww $APB2ENR $save_apb2
mww $SPI2 [expr {$cr1 & ~0x40}]
mww $SPI2 $cr1
mww [expr {$GB+0x0c}] $save_pupdr
set ok [expr {[mrw $GB] == $save_moder && [mrw [expr {$GB+0x0c}]] == $save_pupdr
              && [mrw $SPI2] == $cr1 && [mrw $APB2ENR] == $save_apb2 && [mrw $CCR] == $save_ccr
              && ([mrw [expr {$GB+0x14}]] & (1 << 12)) && ($econ1_now & 0x07) == ($econ1 & 0x07)}]
echo [format "ECON1 %02x at start, %02x at end" $econ1 $econ1_now]
echo [expr {$ok ? "restored" : "NOT fully restored: reset the board"}]
resume
