#ifndef _PTY_H
#define _PTY_H
/* No ptys on Plan 9. openpty/forkpty/login_tty fail with ENOSYS so pty
 * consumers (nvim :terminal) take their error branch at runtime. */
#include <termios.h>
#include <sys/ioctl.h>   /* struct winsize */
#ifdef __cplusplus
extern "C" {
#endif
int openpty(int *, int *, char *, const struct termios *, const struct winsize *);
int forkpty(int *, char *, const struct termios *, const struct winsize *);
int login_tty(int);
#ifdef __cplusplus
}
#endif
#endif
