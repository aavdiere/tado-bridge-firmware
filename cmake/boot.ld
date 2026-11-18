/* Linker script for the bootloader (#24): sectors 0-3 of the STM32F411CE,
 * 64 KB at 0x08000000, where the core starts (BOOT0 is pulled low,
 * docs/hardware.md). The application follows at 0x08010000
 * (cmake/stm32f411ce.ld). RAM: the bootloader uses its first part only and
 * leaves nothing behind that the application relies on. */

MEMORY
{
	rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_main_stack_size = 4K;
_heap_limit = ORIGIN(ram) + LENGTH(ram) - _main_stack_size;

INCLUDE sections.ld
