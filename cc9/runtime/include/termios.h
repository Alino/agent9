#ifndef _TERMIOS_H
#define _TERMIOS_H
/* Decorative termios — there is no tty discipline to program on Plan 9; the
 * terminal (alacritty9/vts) keys raw mode on the alt-screen escape instead.
 * tcgetattr fills a sane cooked struct, tcsetattr/cfmakeraw succeed as no-ops
 * (poll.c/fs.c never consult these flags). */
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
/* tcsetattr actions */
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
#ifdef __cplusplus
}
#endif
#endif
