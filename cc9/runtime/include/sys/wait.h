#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>
/* Process status. cc9 has no fork/exec semantics yet; waitpid is a stub (the
 * macros let LLVM's Program.inc compile; it isn't exercised by compute paths). */
#define WNOHANG   1
#define WUNTRACED 2
#define WIFEXITED(s)    (((s)&0x7f)==0)
#define WEXITSTATUS(s)  (((s)>>8)&0xff)
#define WIFSIGNALED(s)  (((s)&0x7f)!=0 && ((s)&0x7f)!=0x7f)
#define WTERMSIG(s)     ((s)&0x7f)
#define WIFSTOPPED(s)   (((s)&0xff)==0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WCOREDUMP(s)    ((s)&0x80)
#ifdef __cplusplus
extern "C" {
#endif
struct rusage;
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
pid_t wait4(pid_t, int *, int, struct rusage *);
#ifdef __cplusplus
}
#endif
#endif
