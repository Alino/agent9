#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
/* Just enough for LLVM Process.inc's terminal-width query. cc9's ioctl is a
 * stub returning -1, so getColumns() falls back to 0/default — fine. */
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCSWINSZ 0x5414
#define TIOCGWINSZ 0x5413
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int, unsigned long, ...);
#ifdef __cplusplus
}
#endif
#endif
