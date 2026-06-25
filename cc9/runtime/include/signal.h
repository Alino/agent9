#ifndef _SIGNAL_H
#define _SIGNAL_H
/* Minimal <signal.h> for <csignal>. cc9 has no POSIX signal delivery; this is
 * the C freestanding surface (types + the six standard signals) with a
 * raise/signal that just track a handler table — enough for the type/value
 * conformance tests. */
typedef int sig_atomic_t;
typedef void (*__sighandler_t)(int);
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)
#define SIGINT  2
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGSEGV 11
#define SIGTERM 15
#ifdef __cplusplus
extern "C" {
#endif
__sighandler_t signal(int, __sighandler_t);
int raise(int);
#ifdef __cplusplus
}
#endif
#endif
