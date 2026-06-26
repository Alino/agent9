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
		*(.gcc_except_table) *(.gcc_except_table.*)   /* exception LSDAs */
		/* DWARF unwind tables, found at runtime via these symbols (bare-metal
		 * libunwind — no dynamic loader on a.out). */
		. = ALIGN(8);
		__eh_frame_start = .; KEEP(*(.eh_frame)) __eh_frame_end = .;
		. = ALIGN(8);
		/* prioritized ctors (init_priority, sorted ascending) must run BEFORE the
		 * default ones; crt0 iterates start->end (fini is reversed by crt0). */
		__init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
		__fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .;
	} :text
	. = ALIGN(0x200000);   /* data at the next 2MB boundary after text */
	.data : { *(.data) *(.data.*) } :data
	.bss  : { *(.bss) *(.bss.*) *(COMMON) } :data
	end = .; _end = .;      /* heap (sbrk) starts here */
	/* No .eh_frame_hdr index — libunwind scans .eh_frame linearly. */
	__eh_frame_hdr_start = 0;
	__eh_frame_hdr_end = 0;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame_hdr) }
}
