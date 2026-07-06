#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
/* C freestanding signal surface + a POSIX shim wide enough for LLVM's
 * Signals.inc / CrashRecoveryContext to compile and link. cc9 has no real
 * signal delivery: sigaction/sigprocmask/kill are stubs (return 0 / -1), so
 * crash handlers simply never fire — fine for the compiler's compute paths. */
typedef int sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*__sighandler_t)(int);
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGURG   23
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGXCPU  24
#define SIGXFSZ  25
#define NSIG     32

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 1
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_SIGINFO   4
#define SA_RESETHAND 0x80000000
#define SA_NODEFER   0x40000000

typedef struct { int si_signo, si_code, si_errno; pid_t si_pid; uid_t si_uid; void *si_addr; int si_status; } siginfo_t;
struct sigaction {
  union { __sighandler_t sa_handler; void (*sa_sigaction)(int, siginfo_t *, void *); };
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};

#ifdef __cplusplus
extern "C" {
#endif
__sighandler_t signal(int, __sighandler_t);
int raise(int);
int kill(int, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
#ifdef __cplusplus
}
#endif
#endif
