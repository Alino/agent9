#ifndef _SETJMP_H
#define _SETJMP_H
/* jmp_buf holds rbx,rbp,r12,r13,r14,r15,rsp,rip — 8 quadwords (see setjmp.s). */
typedef long jmp_buf[8];
#ifdef __cplusplus
extern "C" {
#endif
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#endif
