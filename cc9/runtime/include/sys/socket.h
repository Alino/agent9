#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include <sys/uio.h>   /* struct iovec — msghdr aggregates it; uv.h expects it here */
/* Minimal Berkeley sockets surface so LLVM's raw_socket_stream compiles. cc9
 * has no socket layer yet; the calls are stubs returning -1 (not on any
 * compiler compute path). Real Plan 9 networking is via /net, not BSD sockets. */
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10
#define PF_UNIX   AF_UNIX
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_CLOEXEC  0x2000   /* == O_CLOEXEC (poll.c fd-flag table) */
#define SOCK_NONBLOCK 0x1000   /* == O_NONBLOCK */
#define SOL_SOCKET  1
#define SO_REUSEADDR 2
#define SO_ERROR     4
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_BROADCAST 6
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15
/* Linux's values. /net has no per-socket timeout knob, so net9's setsockopt
 * rejects these — declared because socket2 references them unconditionally.
 * Keep in sync with pyo39/vendor/libc/src/plan9.rs. */
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_TYPE      3
#define SCM_RIGHTS   1
#define MSG_NOSIGNAL 0x4000
#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL  0x100
#define MSG_OOB      0x01
#define MSG_EOR      0x80   /* Linux's value; referenced by socket2, unused by /net */
#define MSG_TRUNC    0x20
#define MSG_CMSG_CLOEXEC 0
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct linger { int l_onoff; int l_linger; };
struct sockaddr_storage { sa_family_t ss_family; char __pad[126]; };
#define SOMAXCONN 128
struct msghdr { void *msg_name; socklen_t msg_namelen; void *msg_iov; int msg_iovlen;
                void *msg_control; socklen_t msg_controllen; int msg_flags; };
struct cmsghdr { socklen_t cmsg_len; int cmsg_level; int cmsg_type; };
/* linux-style cmsg macros over the struct above (fd-passing callers compile;
 * sendmsg/recvmsg are ENOSYS stubs — Plan 9 fd passing would go via /srv). */
#define CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_LEN(n)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_DATA(c)  ((unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? \
                          (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(m, c) cc9_cmsg_nxthdr(m, c)
static inline struct cmsghdr *cc9_cmsg_nxthdr(struct msghdr *m, struct cmsghdr *c) {
	char *p = (char *)c + CMSG_ALIGN(c->cmsg_len);
	char *end = (char *)m->msg_control + m->msg_controllen;
	return p + sizeof(struct cmsghdr) > end ? 0 : (struct cmsghdr *)p;
}
#ifdef __cplusplus
extern "C" {
#endif
int socket(int, int, int);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr *, socklen_t *);
int connect(int, const struct sockaddr *, socklen_t);
long send(int, const void *, unsigned long, int);
long recv(int, void *, unsigned long, int);
long sendto(int, const void *, unsigned long, int, const void *, socklen_t);
long recvfrom(int, void *, unsigned long, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
int getsockopt(int, int, int, void *, socklen_t *);
int getsockname(int, struct sockaddr *, socklen_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
int socketpair(int, int, int, int[2]);
long sendmsg(int, const struct msghdr *, int);
long recvmsg(int, struct msghdr *, int);
int shutdown(int, int);
#ifdef __cplusplus
}
#endif
#endif
