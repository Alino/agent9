#ifndef NODE9_CC9_COMPAT_H
#define NODE9_CC9_COMPAT_H
/* Small gaps between quickjs-libc's expectations and cc9's runtime.
 * Injected with -include, so the engine sources stay pristine. */
#include <stdio.h>
#include <errno.h>
#include <unistd.h>     /* before the sysconf macro below, or it mangles the prototype */

/* popen/pclose: cc9 has no shell-pipe helper. std.popen() is the only caller and
 * nothing node9 ships uses it (child_process goes through os.exec), so fail
 * honestly rather than pretend. */
#ifndef N9_HAVE_POPEN
static inline FILE *popen(const char *cmd, const char *mode)
{
    (void)cmd; (void)mode;
    errno = ENOSYS;
    return NULL;
}
static inline int pclose(FILE *f) { (void)f; errno = ENOSYS; return -1; }
#endif

/* sysconf(_SC_OPEN_MAX) is used once, to close inherited fds before exec. cc9's
 * sysconf does not know the key; 1024 covers the fd table these programs use and
 * only costs a few close(2) calls on a path that already forks. */
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX 4
#endif
#ifndef N9_OPEN_MAX
#define N9_OPEN_MAX 1024
#endif
#define sysconf(k) ((k) == _SC_OPEN_MAX ? (long)N9_OPEN_MAX : sysconf(k))

#endif /* NODE9_CC9_COMPAT_H */
