// Plan 9 amd64 syscall thunks. Marshal System V register args (what clang
// emits) into the Plan 9 ABI: args on the stack at rsp+8.., number in rbp,
// SYSCALL. Return value comes back in rax (matches SysV return reg).
//
// CRITICAL: the Plan 9 amd64 SYSCALL path does NOT preserve the SysV
// callee-saved registers — empirically it clobbers %rbx, %rbp and %r13 (the
// kernel leaves kernel-space values in them; see test/regprobe.s). SysV code
// (clang) keeps live values in those registers across calls, so every thunk
// must save/restore all callee-saved regs (rbx,rbp,r12,r13,r14,r15) around the
// syscall. (rbp also carries the syscall number, so it must be saved anyway.)
// Missing this corrupts e.g. the `this` pointer across a malloc/pwrite syscall.
	.text

// Save/restore all SysV callee-saved registers. 6 pushes = 48 bytes (16-aligned
// neutral); args are addressed relative to rsp AFTER the per-thunk subq, so the
// 8(%rsp).. offsets below are unaffected by these pushes.
	.macro SAVE_CALLEE
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	.endm
	.macro REST_CALLEE
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.endm

// long n9_pwrite(int fd, const void *buf, long n, long long off)
//   SysV in: edi=fd, rsi=buf, edx=n, rcx=off
	.globl n9_pwrite
n9_pwrite:
	SAVE_CALLEE
	subq	$48, %rsp
	movl	%edi, 8(%rsp)      // fd     @ rsp+8
	movq	%rsi, 16(%rsp)     // buf    @ rsp+16
	movl	%edx, 24(%rsp)     // n      @ rsp+24
	movq	%rcx, 32(%rsp)     // off    @ rsp+32
	movq	$51, %rbp          // PWRITE
	syscall
	addq	$48, %rsp
	REST_CALLEE
	ret

// void n9_exits(const char *msg)   SysV in: rdi=msg
	.globl n9_exits
n9_exits:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)      // msg @ rsp+8
	movq	$8, %rbp           // EXITS
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_brk(void *addr)  — BRK_ = 24; set the program break. SysV: rdi=addr.
	.globl n9_brk
n9_brk:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$24, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_open(const char *name, int mode)   OPEN=14. SysV: rdi=name, esi=mode.
	.globl n9_open
n9_open:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)      // name @ rsp+8
	movl	%esi, 16(%rsp)     // mode @ rsp+16
	movq	$14, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_pread(int fd, void *buf, long n, long long off)  PREAD=50.
//   SysV in: edi=fd, rsi=buf, edx=n, rcx=off
	.globl n9_pread
n9_pread:
	SAVE_CALLEE
	subq	$48, %rsp
	movl	%edi, 8(%rsp)      // fd  @ rsp+8
	movq	%rsi, 16(%rsp)     // buf @ rsp+16
	movl	%edx, 24(%rsp)     // n   @ rsp+24
	movq	%rcx, 32(%rsp)     // off @ rsp+32
	movq	$50, %rbp
	syscall
	addq	$48, %rsp
	REST_CALLEE
	ret

// long n9_close(int fd)   CLOSE=4. SysV: edi=fd.
	.globl n9_close
n9_close:
	SAVE_CALLEE
	subq	$16, %rsp
	movl	%edi, 8(%rsp)
	movq	$4, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_create(const char *name, int omode, unsigned long perm)  CREATE=22.
	.globl n9_create
n9_create:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)      // name
	movl	%esi, 16(%rsp)     // omode
	movl	%edx, 24(%rsp)     // perm (DMDIR|0777 etc.)
	movq	$22, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_seek(long long *ret, int fd, long long off, int whence)  SEEK=39.
	.globl n9_seek
n9_seek:
	SAVE_CALLEE
	subq	$48, %rsp
	movq	%rdi, 8(%rsp)      // ret ptr
	movl	%esi, 16(%rsp)     // fd
	movq	%rdx, 24(%rsp)     // off (vlong)
	movl	%ecx, 32(%rsp)     // whence
	movq	$39, %rbp
	syscall
	addq	$48, %rsp
	REST_CALLEE
	ret

// long n9_stat(const char *name, unsigned char *edir, int nedir)  STAT=42.
	.globl n9_stat
n9_stat:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)
	movq	%rsi, 16(%rsp)
	movl	%edx, 24(%rsp)
	movq	$42, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_fstat(int fd, unsigned char *edir, int nedir)  FSTAT=43.
	.globl n9_fstat
n9_fstat:
	SAVE_CALLEE
	subq	$32, %rsp
	movl	%edi, 8(%rsp)
	movq	%rsi, 16(%rsp)
	movl	%edx, 24(%rsp)
	movq	$43, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_remove(const char *name)  REMOVE=25.
	.globl n9_remove
n9_remove:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$25, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_chdir(const char *path)  CHDIR=3.
	.globl n9_chdir
n9_chdir:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$3, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_wstat(const char *name, unsigned char *edir, int nedir)  WSTAT=44.
	.globl n9_wstat
n9_wstat:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)
	movq	%rsi, 16(%rsp)
	movl	%edx, 24(%rsp)
	movq	$44, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_fwstat(int fd, unsigned char *edir, int nedir)  FWSTAT=45.
	.globl n9_fwstat
n9_fwstat:
	SAVE_CALLEE
	subq	$32, %rsp
	movl	%edi, 8(%rsp)
	movq	%rsi, 16(%rsp)
	movl	%edx, 24(%rsp)
	movq	$45, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_fd2path(int fd, char *buf, int nbuf)  FD2PATH=23.
	.globl n9_fd2path
n9_fd2path:
	SAVE_CALLEE
	subq	$32, %rsp
	movl	%edi, 8(%rsp)
	movq	%rsi, 16(%rsp)
	movl	%edx, 24(%rsp)
	movq	$23, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_sleep(long millisecs)   SLEEP=17 (sleep(0) yields the CPU)
	.globl n9_sleep
n9_sleep:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$17, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// int n9_semacquire(int *addr, int block)   SEMACQUIRE=37
	.globl n9_semacquire
n9_semacquire:
	SAVE_CALLEE
	subq	$24, %rsp
	movq	%rdi, 8(%rsp)      // addr
	movl	%esi, 16(%rsp)     // block
	movq	$37, %rbp
	syscall
	addq	$24, %rsp
	REST_CALLEE
	ret

// int n9_semrelease(int *addr, int count)   SEMRELEASE=38
	.globl n9_semrelease
n9_semrelease:
	SAVE_CALLEE
	subq	$24, %rsp
	movq	%rdi, 8(%rsp)
	movl	%esi, 16(%rsp)
	movq	$38, %rbp
	syscall
	addq	$24, %rsp
	REST_CALLEE
	ret

// void* n9_segattach(int attr, char *class, void *va, ulong len)  SEGATTACH=30
//   SysV in: edi=attr, rsi=class, rdx=va, rcx=len. Returns the va in rax.
//   With the W^X kernel patch, attr=SG_EXEC (0x800) requests executable memory
//   (only honored when plan9.ini wxallow=1) — used for JIT.
	.globl n9_segattach
n9_segattach:
	SAVE_CALLEE
	subq	$48, %rsp
	movl	%edi, 8(%rsp)      // attr
	movq	%rsi, 16(%rsp)     // class
	movq	%rdx, 24(%rsp)     // va
	movq	%rcx, 32(%rsp)     // len
	movq	$30, %rbp
	syscall
	addq	$48, %rsp
	REST_CALLEE
	ret

// long n9_rfork_thread(void *stacktop, void (*fn)(void*), void *arg)
//   Creates a thread (RFPROC|RFMEM|RFNOWAIT = 112) running fn(arg) on a fresh
//   stack. Returns the child pid in the parent; the child never returns.
//   The Plan 9 rfork syscall does NOT preserve general registers into the
//   child, so fn/arg/stack are handed off through RFMEM-shared globals (the
//   pthread layer holds a lock around the call to serialize the handoff). The
//   child switches to the new stack before any shared-stack access, so it never
//   races the parent on the original stack.
	.data
	.globl n9_th_stack
	.globl n9_th_fn
	.globl n9_th_arg
n9_th_stack: .quad 0
n9_th_fn:    .quad 0
n9_th_arg:   .quad 0
	.text
	.globl n9_rfork_thread
n9_rfork_thread:                  // rdi=stacktop, rsi=fn, rdx=arg
	movq	%rdi, n9_th_stack(%rip)
	movq	%rsi, n9_th_fn(%rip)
	movq	%rdx, n9_th_arg(%rip)
	SAVE_CALLEE                // parent must get rbx/r13/... back intact
	subq	$16, %rsp
	movl	$112, 8(%rsp)      // RFPROC|RFMEM|RFNOWAIT
	movq	$19, %rbp          // RFORK
	syscall
	testq	%rax, %rax         // rax==0 in child; test touches no stack
	jz	1f
	addq	$16, %rsp
	REST_CALLEE
	ret
1:	// child: read the handoff from shared memory, switch stack, call fn(arg)
	movq	n9_th_stack(%rip), %rsp
	movq	n9_th_arg(%rip), %rdi
	movq	n9_th_fn(%rip), %rax
	callq	*%rax
	subq	$16, %rsp
	movq	$0, 8(%rsp)
	movq	$8, %rbp           // EXITS
	syscall
2:	jmp	2b

// long n9_alarm(unsigned long ms)  ALARM=6. Posts an "alarm" note after ms.
	.globl n9_alarm
n9_alarm:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$6, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_notify(void *handler)  NOTIFY=28. Registers a Plan 9 note handler.
	.globl n9_notify
n9_notify:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$28, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_dup(int old, int new)  DUP=5. Dups `old` onto `new` (new>=0, returns
// new) or to the lowest free fd (new<0). Returns the new fd / -1.
	.globl n9_dup
n9_dup:
	SAVE_CALLEE
	subq	$32, %rsp
	movl	%edi, 8(%rsp)      // old
	movl	%esi, 16(%rsp)     // new
	movq	$5, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_pipe(int fd[2])  PIPE=21. Creates a pipe; fills fd[0]/fd[1] with the
// two ends, returns 0 / -1.
	.globl n9_pipe
n9_pipe:
	SAVE_CALLEE
	subq	$16, %rsp
	movq	%rdi, 8(%rsp)
	movq	$21, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_noted(int disposition)  NOTED=29. Returns from a note (NDFLT=1 dies,
// NCONT=0 resumes). Does not return to the caller on NDFLT.
	.globl n9_noted
n9_noted:
	subq	$16, %rsp
	movl	%edi, 8(%rsp)
	movq	$29, %rbp
	syscall
	addq	$16, %rsp
	ret

// cc9_notetramp — the registered note handler. The kernel enters here (Plan 9
// note frame on the stack) when a note is posted. We don't know the exact amd64
// frame offsets without the kernel source, so we hand the raw entry %rsp to a
// SysV C scanner that dumps the note string (it contains pc=/addr=) to fd 2,
// then terminate with noted(NDFLT). Never returns.
	.globl cc9_notetramp
cc9_notetramp:
	movq	%rsp, %rdi         // raw note-frame base -> SysV arg0
	andq	$-16, %rsp         // align for the SysV call (we never return here)
	call	cc9_note_handler   // returns disposition in eax: 0=NCONT, 1=NDFLT
	movl	%eax, %edi         // FP faults re-mask + resume (NCONT); others die (NDFLT)
	call	n9_noted
3:	jmp	3b

// long n9_rfork(int flags)   RFORK=19. Plain fork-family primitive (system()
// uses RFPROC|RFFDG). Unlike n9_rfork_thread this returns to BOTH parent (pid)
// and child (0) — the caller branches on the result.
	.globl n9_rfork
n9_rfork:
	SAVE_CALLEE
	subq	$16, %rsp
	movl	%edi, 8(%rsp)      // flags
	movq	$19, %rbp
	syscall
	addq	$16, %rsp
	REST_CALLEE
	ret

// long n9_exec(char *name, char **argv)   EXEC=7. Replaces the process image;
// returns only on failure.
	.globl n9_exec
n9_exec:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)      // name
	movq	%rsi, 16(%rsp)     // argv
	movq	$7, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_await(char *buf, int n)   AWAIT=47. Fills buf with the wait message
// of an exited child ("pid utime stime rtime status"); returns length or -1.
	.globl n9_await
n9_await:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)      // buf
	movl	%esi, 16(%rsp)     // n
	movq	$47, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret

// long n9_tsemacquire(long *addr, long ms)   TSEMACQUIRE=52. Like semacquire
// but with a millisecond timeout: returns 1 (acquired), 0 (timed out), -1 (err).
// ms==0 polls non-blocking. Backs condition_variable::wait_for / timed_mutex.
	.globl n9_tsemacquire
n9_tsemacquire:
	SAVE_CALLEE
	subq	$32, %rsp
	movq	%rdi, 8(%rsp)      // addr
	movq	%rsi, 16(%rsp)     // ms
	movq	$52, %rbp
	syscall
	addq	$32, %rsp
	REST_CALLEE
	ret
