/* Lay out a freestanding x86_64 image for Plan 9 amd64 a.out conversion.
 *   text (code+rodata) at 0x200028 = UTZERO(0x200000)+40-byte header, R+X.
 *   data+bss at roundup(text_end, 0x200000) — where the 9front kernel maps the
 *   data segment (0x400000 for text<2MB, 0x600000 for the next 2MB, ...). Using
 *   ALIGN keeps large-text binaries (>2MB code) from colliding with data.
 * Each segment in its own PT_LOAD; the ELF ehdr/phdrs are NOT mapped, so the
 * ELF stays valid and elf2aout.py repackages by vaddr.
 */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
	. = 0x200028;
	.text : {
		*(.text) *(.text.*) *(.rodata) *(.rodata.*)
		. = ALIGN(8);
		__init_array_start = .; *(.init_array) *(.init_array.*) __init_array_end = .;
		__fini_array_start = .; *(.fini_array) *(.fini_array.*) __fini_array_end = .;
	} :text
	. = ALIGN(0x200000);   /* data at the next 2MB boundary after text */
	.data : { *(.data) *(.data.*) } :data
	.bss  : { *(.bss) *(.bss.*) *(COMMON) } :data
	end = .; _end = .;      /* heap (sbrk) starts here */
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.eh_frame_hdr) }
}
