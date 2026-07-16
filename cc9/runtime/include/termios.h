#ifndef _TERMIOS_H
#define _TERMIOS_H
/* termios over /dev/consctl — Plan 9's console has one raw/cooked switch
 * ("rawon"/"rawoff", cons(3)) and no per-flag discipline, so tcsetattr maps
 * ECHO|ICANON onto it and stores the rest of the struct verbatim for tcgetattr
 * to hand back. This is REAL: getpass and friends turn echo off and echo really
 * goes off. Everything else in the struct is inert (poll.c/fs.c never consult
 * these flags), and both calls fail with ENOTTY when the fd isn't a console —
 * e.g. under alacritty9/vts, whose "tty" is a pipe with no consctl and which
 * keys raw mode on the alt-screen escape instead. See posix_llvm.c. */
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 32
struct termios {
	tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
	cc_t c_cc[NCCS];
	speed_t c_ispeed, c_ospeed;
};
/* c_iflag */
#define IGNBRK 0001
#define BRKINT 0002
#define IGNPAR 0004
#define PARMRK 0010
#define INPCK  0020
#define ISTRIP 0040
#define INLCR  0100
#define IGNCR  0200
#define ICRNL  0400
#define IXON   02000
#define IXOFF  010000
/* c_oflag */
#define OPOST  0001
#define ONLCR  0004
/* c_cflag */
#define CSIZE  0060
#define CS8    0060
#define CSTOPB 0100
#define CREAD  0200
#define PARENB 0400
#define HUPCL  02000
/* c_lflag */
#define ISIG   0001
#define ICANON 0002
#define ECHO   0010
#define ECHOE  0020
#define ECHOK  0040
#define ECHONL 0100
#define NOFLSH 0200
#define IEXTEN 0100000
/* c_cc indices */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSTART 8
#define VSTOP  9
#define VSUSP  10
#define VEOL   11
#define VEOL2  16
#define VREPRINT 12
#define VWERASE  14
#define VLNEXT   15
#define VDISCARD 13
/* tcsetattr actions */
#define _POSIX_VDISABLE 0
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#ifdef __cplusplus
extern "C" {
#endif
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
void cfmakeraw(struct termios *);
int tcflush(int, int);
int tcdrain(int);
int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
#ifdef __cplusplus
}
#endif
#endif
