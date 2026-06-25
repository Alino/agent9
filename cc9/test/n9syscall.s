// Plan 9 amd64 syscall thunks. Marshal System V register args (what clang
// emits) into the Plan 9 ABI: args on the stack at rsp+8.., number in rbp,
// SYSCALL. Return value comes back in rax (matches SysV return reg).
	.text

// long n9_pwrite(int fd, const void *buf, long n, long long off)
//   SysV in: edi=fd, rsi=buf, edx=n, rcx=off
	.globl n9_pwrite
n9_pwrite:
	pushq	%rbp
	subq	$48, %rsp
	movl	%edi, 8(%rsp)      // fd     @ rsp+8
	movq	%rsi, 16(%rsp)     // buf    @ rsp+16
	movl	%edx, 24(%rsp)     // n      @ rsp+24
	movq	%rcx, 32(%rsp)     // off    @ rsp+32
	movq	$51, %rbp          // PWRITE
	syscall
	addq	$48, %rsp
	popq	%rbp
	ret

// void n9_exits(const char *msg)   SysV in: rdi=msg
	.globl n9_exits
n9_exits:
	pushq	%rbp
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)      // msg @ rsp+8
	movq	$8, %rbp           // EXITS
	syscall
	addq	$16, %rsp
	popq	%rbp
	ret

// long n9_brk(void *addr)  — BRK_ = 24; set the program break. SysV: rdi=addr.
	.globl n9_brk
n9_brk:
	pushq	%rbp
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$24, %rbp
	syscall
	addq	$16, %rsp
	popq	%rbp
	ret
