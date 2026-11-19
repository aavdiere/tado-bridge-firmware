/* Output sections for both images (#24): the bootloader (cmake/boot.ld)
 * and the application (cmake/stm32f411ce.ld). Written for this project in
 * place of libopencm3's cortex-m-generic.ld, so that the image descriptor
 * can sit at a fixed offset. It defines the symbols libopencm3's
 * reset_handler uses (vendor/libopencm3/lib/cm3/vector.c: _data_loadaddr,
 * _data, _edata, _ebss, the init/fini arrays; _stack) and newlib's `end`
 * (src/syscalls.c), plus:
 *   - the image descriptor (src/fwimg.h, section .image_desc) at offset
 *     0x200 (FWIMG_DESC_OFFSET), right after the vector table;
 *   - _image_start and _image_end: the load image as the .bin holds it,
 *     from the vector table to the end of .data's initial values.
 * The including script defines MEMORY (rom, ram). */

EXTERN(vector_table)
ENTRY(reset_handler)

SECTIONS
{
	.text : {
		_image_start = .;
		KEEP(*(.vectors))
		_vectors_end = .;
		. = _image_start + 0x200;	/* FWIMG_DESC_OFFSET */
		KEEP(*(.image_desc))
		*(.text*)
		. = ALIGN(4);
		*(.rodata*)
		. = ALIGN(4);
	} >rom

	.preinit_array : {
		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP(*(.preinit_array))
		__preinit_array_end = .;
	} >rom
	.init_array : {
		. = ALIGN(4);
		__init_array_start = .;
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		__init_array_end = .;
	} >rom
	.fini_array : {
		. = ALIGN(4);
		__fini_array_start = .;
		KEEP(*(.fini_array))
		KEEP(*(SORT(.fini_array.*)))
		__fini_array_end = .;
	} >rom

	.ARM.extab : {
		*(.ARM.extab*)
	} >rom
	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx*)
		__exidx_end = .;
	} >rom

	. = ALIGN(4);
	_etext = .;

	.noinit (NOLOAD) : {
		*(.noinit*)
	} >ram
	. = ALIGN(4);

	.data : {
		_data = .;
		*(.data*)
		*(.ramtext*)
		. = ALIGN(4);
		_edata = .;
	} >ram AT >rom
	_data_loadaddr = LOADADDR(.data);
	_image_end = LOADADDR(.data) + SIZEOF(.data);

	.bss : {
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} >ram

	/DISCARD/ : { *(.eh_frame) }

	. = ALIGN(4);
	end = .;
}

PROVIDE(_stack = ORIGIN(ram) + LENGTH(ram));

ASSERT(_vectors_end <= _image_start + 0x200, "vector table runs into the image descriptor")
ASSERT(_image_end <= ORIGIN(rom) + LENGTH(rom), "image larger than its flash region")
ASSERT(_image_end % 4 == 0, "image size not a multiple of 4 (word programming)")
