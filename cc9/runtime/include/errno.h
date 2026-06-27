#ifndef _ERRNO_H
#define _ERRNO_H
#ifdef __cplusplus
extern "C" {
#endif
int *__n9_errno(void);
#ifdef __cplusplus
}
#endif
#define errno (*__n9_errno())
/* Linux/glibc errno values (what libc++'s generic_category expects). */
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOSPC 28
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EOVERFLOW 75
#define EILSEQ 84
#define ENOTSUP 95
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
/* Largest errno + 1, so libc++'s config_elast.h sets _LIBCPP_ELAST and
 * std::system_category()::default_error_condition works on this target. */
#define ELAST 4095
#endif
