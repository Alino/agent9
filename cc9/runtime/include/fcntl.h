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
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#ifdef __cplusplus
extern "C" {
#endif
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
#ifdef __cplusplus
}
#endif
#endif
