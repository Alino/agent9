#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
/* select(2) emulated over the cc9 poll layer (runtime/poll.c). */
#include <sys/time.h>

#define FD_SETSIZE 512
typedef struct { unsigned long fds_bits[FD_SETSIZE/64]; } fd_set;

#define FD_ZERO(s)      do{ int __i; for(__i=0;__i<FD_SETSIZE/64;__i++) (s)->fds_bits[__i]=0; }while(0)
#define FD_SET(d, s)    ((s)->fds_bits[(d)/64] |= 1UL << ((d)%64))
#define FD_CLR(d, s)    ((s)->fds_bits[(d)/64] &= ~(1UL << ((d)%64)))
#define FD_ISSET(d, s)  (((s)->fds_bits[(d)/64] & (1UL << ((d)%64))) != 0)

#ifdef __cplusplus
extern "C" {
#endif
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#ifdef __cplusplus
}
#endif
#endif
