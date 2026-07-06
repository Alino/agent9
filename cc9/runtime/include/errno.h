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
#define E2BIG 7
#define ECHILD 10
#define ESPIPE 29
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
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
#define EDEADLK 35
#define ENOLCK 37
#define ENOMSG 42
#define EPROTO 71
#define EBADFD 77
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EPFNOSUPPORT 96
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ESHUTDOWN 108
#define ETOOMANYREFS 109
#define ECONNREFUSED 111
#define EHOSTDOWN 112
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ESTALE 116
#define EREMOTEIO 121
#define ECANCELED 125
#define EOWNERDEAD 130
#define ENOTSUP 95
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
/* Largest errno + 1, so libc++'s config_elast.h sets _LIBCPP_ELAST and
 * std::system_category()::default_error_condition works on this target. */
#define ELAST 4095
#endif
