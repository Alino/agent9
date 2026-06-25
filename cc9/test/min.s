/* Freestanding Plan 9 amd64: exits(nil). Ground-truth reference for the
 * syscall encoding + entry, to mirror in the host a.out builder.
 * Kernel reads syscall number from BP (RARG), args from the stack at RSP+8.
 */
TEXT _main(SB), 1, $16
	MOVQ	$0, 8(SP)	/* arg0 = nil exits-message pointer */
	MOVQ	$8, BP		/* EXITS = 8 in RARG */
	SYSCALL
	RET
