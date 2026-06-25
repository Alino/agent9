// cc9 setjmp/longjmp for amd64 (System V). Saves the callee-saved registers +
// stack pointer + return address into jmp_buf, so a later longjmp resumes at
// the setjmp call site. cc9 has no exceptions; this is the standard C escape.
	.text
	.globl	setjmp
setjmp:                          // rdi = jmp_buf
	movq	%rbx, 0(%rdi)
	movq	%rbp, 8(%rdi)
	movq	%r12, 16(%rdi)
	movq	%r13, 24(%rdi)
	movq	%r14, 32(%rdi)
	movq	%r15, 40(%rdi)
	leaq	8(%rsp), %rax        // caller's rsp (past the return address)
	movq	%rax, 48(%rdi)
	movq	(%rsp), %rax         // return address
	movq	%rax, 56(%rdi)
	xorl	%eax, %eax           // setjmp returns 0 directly
	ret

	.globl	longjmp
longjmp:                         // rdi = jmp_buf, rsi = val
	movq	0(%rdi), %rbx
	movq	8(%rdi), %rbp
	movq	16(%rdi), %r12
	movq	24(%rdi), %r13
	movq	32(%rdi), %r14
	movq	40(%rdi), %r15
	movq	48(%rdi), %rsp
	movq	56(%rdi), %rdx       // saved return address
	movl	%esi, %eax           // return val...
	testl	%eax, %eax
	jnz	1f
	incl	%eax                 // ...but never 0 (return 1 instead)
1:	jmp	*%rdx
