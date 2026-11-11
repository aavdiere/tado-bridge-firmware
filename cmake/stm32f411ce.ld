/* Linker script for STM32F411CE (IB01), 512K flash, 128K RAM, no ram2.
 * Single image at the start of flash for now; the app moves to 0x08010000
 * in P7 (bootloader). */

MEMORY
{
	rom (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* The heap (newlib _sbrk) may grow up to here; the rest of RAM is the main
 * (MSP) stack, which also serves ISRs and the scheduler start. */
_main_stack_size = 4K;
_heap_limit = ORIGIN(ram) + LENGTH(ram) - _main_stack_size;

INCLUDE cortex-m-generic.ld
