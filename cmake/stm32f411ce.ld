/* Linker script for the application on the STM32F411CE (IB01), 128K RAM,
 * no ram2. Since P7 (#24) the application starts at 0x08010000, sector 4,
 * behind the 64 KB bootloader in sectors 0-3 (cmake/boot.ld), and fills
 * sectors 4-7, 448 KB (docs/design.md "Internal flash layout";
 * FWIMG_APP_BASE and FWIMG_APP_MAX in src/fwimg.h). The bootloader sets
 * VTOR before the jump, and src/main.c sets it again first thing. */

MEMORY
{
	rom (rx)  : ORIGIN = 0x08010000, LENGTH = 448K
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* The heap (newlib _sbrk) may grow up to here; the rest of RAM is the main
 * (MSP) stack, which also serves ISRs and the scheduler start. */
_main_stack_size = 4K;
_heap_limit = ORIGIN(ram) + LENGTH(ram) - _main_stack_size;

INCLUDE sections.ld

/* src/image_desc.c must be in the image: the bootloader starts nothing
 * without it. */
EXTERN(image_desc)
ASSERT(DEFINED(image_desc), "no image descriptor (src/image_desc.c)")
