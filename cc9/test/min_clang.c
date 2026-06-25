/*
 * Freestanding exits(nil), compiled by clang for x86_64. The `naked` function
 * makes clang emit exactly this asm (no prologue/epilogue), so .text is the
 * Plan 9 syscall sequence: number in rbp (RARG), args at rsp+8, SYSCALL.
 * This is the first step of the LLVM path: clang produces the 9front code.
 */
__attribute__((naked)) void _start(void)
{
	__asm__(
		"subq $16, %rsp\n"
		"movq $0, 8(%rsp)\n"   /* arg0 = nil exits message */
		"movq $8, %rbp\n"      /* EXITS = 8 in RARG */
		"syscall\n"
		"addq $16, %rsp\n"
		"ret\n"
	);
}
