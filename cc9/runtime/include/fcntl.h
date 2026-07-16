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
/* Plan 9 open(2) has no per-fd sync mode; accepted and ignored (like O_NOCTTY).
 * Distinct bits so F_GETFL round-trips honestly. */
#define O_SYNC  0x10000
#define O_DSYNC 0x20000
#define O_RSYNC 0x40000
#define O_NOCTTY 0
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
/* fcntl commands + flags (LLVM Unix .inc sets FD_CLOEXEC / O_NONBLOCK). */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
/* advisory record locks (LLVM Path.inc lockFile/unlockFile). Plan 9 has no
 * POSIX record locks and cc9 does not fake them: F_SETLK/F_SETLKW/F_GETLK all
 * fail with ENOLCK (F_UNLCK succeeds — we hold nothing to release). See the
 * lock arm of fcntl() in poll.c for why DMEXCL is not a substitute. */
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
