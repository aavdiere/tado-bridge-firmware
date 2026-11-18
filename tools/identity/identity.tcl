# Identity record sectors over SWD (#21): read both, or write one, through
# tanoclo's SRAM stub for the MX25R1635F (tanoclo/patch_ib_firmware/
# spi_stub.c, the stub the P0 backup was read with). Run by
# tools/identity/provision.sh, which checks the stub's symbols first; not
# meant to be run by hand.
#
#   openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
#     -c "set ID_STUB_DIR <dir>; set ID_MODE read; set ID_DIR <dir>" \
#     -f tools/identity/identity.tcl
#   ... -c "set ID_MODE write; set ID_ADDR 0x101000; set ID_FILE <file>" ...
#
# Guards: only the two identity sectors 0x100000 and 0x101000 are ever
# written (docs/design.md "Layout (external flash)"), one whole 4 KB sector
# per run; the stub erases it, programs all 16 pages and compares each page
# with the buffer (spi_stub.c mx25_write_sector_full_verify). No option
# byte, no internal flash, no other sector. PA13/PA14 (SWD) are never
# touched: the only GPIO write here is PA4's fields, and the stub changes
# only PA5-PA7 and PB9 (spi_stub.c spi1_init).

foreach v {ID_STUB_DIR ID_MODE} {
    if {![info exists $v]} {
        puts "identity.tcl: $v not set"
        shutdown error
    }
}

set STUB_ELF "$ID_STUB_DIR/spi_stub.elf"
source "$ID_STUB_DIR/spi_stub_addrs.tcl"

set SECTOR    0x1000
set ID_SLOT_A 0x100000
set ID_SLOT_B 0x101000
set MAX_TRIES 16

# The stub's status block g_out: A5 5A then the status, 0x11 read done,
# 0x22 write done, 0xEE JEDEC ID wrong, 0xEF bad parameters, 0xE1/0xE2
# verify failed (spi_stub.c).
proc stub_run {op addr done} {
    global G_OP G_ADDR G_LEN G_OUT SECTOR STUB_ENTRY
    mww $G_OP $op
    mww $G_ADDR $addr
    mww $G_LEN $SECTOR
    mww $G_OUT 0
    resume $STUB_ENTRY
    # A 4 KB erase is at most 240 ms (mx25r1635f.txt:3340) and 16 page
    # programs at most 10 ms each (:3337); 5 s leaves room for the stub's
    # 500 kHz SPI.
    for {set t 0} {$t < 5000} {incr t 5} {
        set d [read_memory $G_OUT 8 3]
        if {[lindex $d 0] == 0xA5 && [lindex $d 1] == 0x5A} {
            set st [lindex $d 2]
            if {$st == $done} {
                halt
                return 1
            }
            if {$st == 0xEE || $st == 0xEF || $st == 0xE1 || $st == 0xE2} {
                halt
                puts [format "identity.tcl: stub status 0x%02X at 0x%06X" $st $addr]
                return 0
            }
        }
        sleep 5
    }
    halt
    puts [format "identity.tcl: stub timeout at 0x%06X" $addr]
    return 0
}

proc same_file {a b} {
    set fa [open $a rb]; set da [read $fa]; close $fa
    set fb [open $b rb]; set db [read $fb]; close $fb
    return [string equal $da $db]
}

# One sector into $out, read until two reads in a row agree: MISO on this
# board can be marginal (tanoclo dump_external_flash.tcl).
proc read_sector {addr out} {
    global BUF_ADDR SECTOR MAX_TRIES
    set have 0
    for {set try 1} {$try <= $MAX_TRIES} {incr try} {
        if {![stub_run 0 $addr 0x11]} {
            set have 0
            continue
        }
        if {!$have} {
            dump_image $out $BUF_ADDR $SECTOR
            set have 1
            continue
        }
        dump_image "$out.chk" $BUF_ADDR $SECTOR
        if {[same_file $out "$out.chk"]} {
            file delete "$out.chk"
            return 1
        }
        file rename -force "$out.chk" $out
    }
    puts [format "identity.tcl: 0x%06X never read the same twice" $addr]
    return 0
}

init
reset halt

# Radio CSn (PA4) high, push-pull output, before the stub drives SPI1: after
# reset PA4 floats as an input and the CC110L may see itself selected (#28,
# src/board.c board_cs_early()). RCC_AHB1ENR 0x40023830 GPIOAEN bit 0
# (rm0383.txt:1723, 5900-5901); GPIOA at 0x40020000 (rm0383.txt:1732):
# BSRR +0x18 BS4 (:8222-8223), OTYPER +0x04 bit 4 clear (:8092-8094), MODER
# +0x00 field 9:8 = 01 (:8064-8065). mmw is a read-modify-write of those
# bits only; MODER bits 29:26 (PA13, PA14) are kept.
mmw 0x40023830 0x00000001 0
mww 0x40020018 0x00000010
mmw 0x40020004 0 0x00000010
mmw 0x40020000 0x00000100 0x00000200

load_image $STUB_ELF
reg sp 0x20020000

if {$ID_MODE eq "read"} {
    if {![read_sector $ID_SLOT_A "$ID_DIR/slot_a.bin"] || ![read_sector $ID_SLOT_B "$ID_DIR/slot_b.bin"]} {
        shutdown error
    }
    puts "identity.tcl: read 0x100000 and 0x101000"
} elseif {$ID_MODE eq "write"} {
    if {$ID_ADDR != $ID_SLOT_A && $ID_ADDR != $ID_SLOT_B} {
        puts [format "identity.tcl: refused: 0x%06X is not an identity sector" $ID_ADDR]
        shutdown error
    }
    if {[file size $ID_FILE] != $SECTOR} {
        puts "identity.tcl: refused: $ID_FILE is not one 4096-byte sector"
        shutdown error
    }
    # load_image <file> <offset> bin <min> <len>: the whole file into g_buf.
    load_image $ID_FILE $BUF_ADDR bin $BUF_ADDR $SECTOR
    if {![stub_run 1 $ID_ADDR 0x22]} {
        puts "identity.tcl: write failed; the other slot is untouched"
        shutdown error
    }
    puts [format "identity.tcl: wrote and verified 0x%06X" $ID_ADDR]
} else {
    puts "identity.tcl: ID_MODE must be read or write"
    shutdown error
}
shutdown
