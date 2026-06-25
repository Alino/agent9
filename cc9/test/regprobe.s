// cc9_regprobe(out) — set SysV callee-saved regs to sentinels, do ONE Plan 9
// PWRITE syscall (writes "P" to fd 1), then store the regs' values after the
// syscall into out[0..5] = {rbx,rbp,r12,r13,r14,r15}. Isolates exactly what the
// kernel SYSCALL path clobbers.
	.text
	.globl cc9_regprobe
	.data
pchar:	.byte 0x50      // 'P'
	.text
cc9_regprobe:                 // rdi = out (unsigned long[6])
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushq	%rdi              // save out ptr across the syscall (on stack, not a reg)
	// load sentinels
	movq	$0x1111111111111111, %rbx
	movq	$0x2222222222222222, %r12
	movq	$0x3333333333333333, %r13
	movq	$0x4444444444444444, %r14
	movq	$0x5555555555555555, %r15
	movq	$0x6666666666666666, %rbp
	// PWRITE(fd=1, buf=pchar, n=1, off=-1): args on stack at rsp+8..
	subq	$48, %rsp
	movl	$1, 8(%rsp)
	leaq	pchar(%rip), %rax
	movq	%rax, 16(%rsp)
	movl	$1, 24(%rsp)
	movq	$-1, 32(%rsp)
	movq	$51, %rbp         // PWRITE (rbp is also a sentinel target; set last)
	syscall
	addq	$48, %rsp
	// capture regs AFTER the syscall
	movq	(%rsp), %rdi      // reload out ptr (rsp now points at saved rdi)
	movq	%rbx, 0(%rdi)
	movq	%rbp, 8(%rdi)
	movq	%r12, 16(%rdi)
	movq	%r13, 24(%rdi)
	movq	%r14, 32(%rdi)
	movq	%r15, 40(%rdi)
	popq	%rdi
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
