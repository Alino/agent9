#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#include <sys/types.h>
struct iovec { void *iov_base; size_t iov_len; };
#ifdef __cplusplus
extern "C" {
#endif
long writev(int, const struct iovec *, int);
long readv(int, const struct iovec *, int);
#ifdef __cplusplus
}
#endif
#endif
