/* Linker script for STM32F411CE (IB01), 512K flash, 128K RAM, no ram2.
 * Single image at the start of flash for now; the app moves to 0x08010000
 * in P7 (bootloader). */

MEMORY
{
	rom (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

INCLUDE cortex-m-generic.ld
