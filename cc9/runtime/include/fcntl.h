#ifndef _FCNTL_H
#define _FCNTL_H
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0x100
#define O_TRUNC   0x200
#define O_EXCL    0x400
#define O_APPEND  0x800
#define O_NONBLOCK 0x1000
#define O_CLOEXEC 0x2000
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW 0x8000
#define O_NOCTTY 0
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
/* fcntl commands + flags (LLVM Unix .inc sets FD_CLOEXEC / O_NONBLOCK). */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define FD_CLOEXEC 1
/* advisory record locks (LLVM Path.inc lockFile/unlockFile). cc9's fcntl is a
 * stub returning 0, so locks always "succeed" — single-writer is the norm. */
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
struct flock { short l_type, l_whence; long l_start, l_len; int l_pid; };
#ifdef __cplusplus
extern "C" {
#endif
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int fcntl(int, int, ...);
#ifdef __cplusplus
}
#endif
#endif
