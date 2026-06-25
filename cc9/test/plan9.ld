/* Lay out a freestanding x86_64 image for Plan 9 amd64 a.out conversion.
 * Text (code + rodata) at 0x200028 = UTZERO(0x200000) + 40-byte a.out header,
 * in its OWN PT_LOAD that does NOT include the ELF ehdr/phdrs (so the ELF stays
 * valid and objcopy -O binary yields exactly the bytes Plan 9 maps at 0x200028).
 */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }   /* R+X */
SECTIONS {
	. = 0x200028;
	.text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) } :text
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.eh_frame_hdr) *(.data*) *(.bss*) }
}
