#ifndef _SETJMP_H
#define _SETJMP_H
/* jmp_buf holds rbx,rbp,r12,r13,r14,r15,rsp,rip — 8 quadwords (see setjmp.s). */
typedef long jmp_buf[8];
#ifdef __cplusplus
extern "C" {
#endif
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));

/* POSIX sigsetjmp/siglongjmp. Plan 9 has notes, not a signal mask, so there is
 * no mask to save or restore: savemask is accepted and ignored, and these are
 * exactly setjmp/longjmp. Callers that only want the jump (the common case)
 * get correct behaviour; nothing here can restore a mask that doesn't exist. */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)
#ifdef __cplusplus
}
#endif
#endif
