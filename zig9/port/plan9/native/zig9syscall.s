// zig9: generic Plan 9 amd64 syscall thunk for the CBE-built (clang/cc9) zig.
// The Zig std plan9 syscall shims call this under zig_backend == .stage2_c
// instead of their inline asm (whose output/input registers double as clobbers,
// which clang rejects).
//
// SysV in: rdi=num, rsi=a0, rdx=a1, rcx=a2, r8=a3
// Plan 9 ABI: args on the stack at rsp+8.., syscall number in rbp, SYSCALL;
// return in rax (matches SysV). Always marshals 4 arg slots — the kernel reads
// only what the syscall needs.
//
// CRITICAL (from cc9/test/n9syscall.s): the Plan 9 amd64 SYSCALL path clobbers
// the SysV callee-saved regs rbx/rbp/r13, so save/restore all of them.
	.text
	.globl zig9_syscall
zig9_syscall:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	subq	$48, %rsp
	movq	%rsi, 8(%rsp)      // a0 @ rsp+8
	movq	%rdx, 16(%rsp)     // a1 @ rsp+16
	movq	%rcx, 24(%rsp)     // a2 @ rsp+24
	movq	%r8, 32(%rsp)      // a3 @ rsp+32
	movq	%rdi, %rbp         // syscall number
	syscall
	addq	$48, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
