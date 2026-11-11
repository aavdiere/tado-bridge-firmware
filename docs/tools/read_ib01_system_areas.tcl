# Reads the STM32F411 areas that tanoclo's read.sh does not cover.
# Read-only: uses mdw, mdh and dump_image, and writes nothing to the device.
# Written without hardware access. Not yet tested on a real IB01.
#
# Power the board from the ST-Link 3V3 only: no USB, no Ethernet.
# Run from the folder that should hold the output:
#   openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
#     -f <path-to-repo>/docs/tools/read_ib01_system_areas.tcl \
#     2>&1 | tee ib01_system_areas.log
#
# FLASH_OPTCR bits 15:8 give the read protection level:
#   0xAA = level 0 (none), 0xCC = level 2 (permanent), anything else = level 1.

init
reset halt

echo "FLASH_OPTCR (read protection is bits 15:8):"
mdw 0x40023C14

echo "Flash size in KiB:"
mdh 0x1FFF7A22

# 16 bytes of option bytes, 0x1FFFC000-0x1FFFC00F (RM0383 section 3.4).
# The word at 0x1FFFC008 holds the sector write protection bits.
dump_image ib01_option_bytes.bin 0x1FFFC000 16

# 512 bytes of OTP plus 16 lock bytes
dump_image ib01_otp.bin 0x1FFF7800 528

# 96-bit unique device ID
dump_image ib01_uid.bin 0x1FFF7A10 12

echo "read_ib01_system_areas - Done."
shutdown
