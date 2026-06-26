#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
/* Minimal Berkeley sockets surface so LLVM's raw_socket_stream compiles. cc9
 * has no socket layer yet; the calls are stubs returning -1 (not on any
 * compiler compute path). Real Plan 9 networking is via /net, not BSD sockets. */
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOL_SOCKET  1
#define SO_REUSEADDR 2
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __pad[126]; };
struct msghdr { void *msg_name; socklen_t msg_namelen; void *msg_iov; int msg_iovlen;
                void *msg_control; socklen_t msg_controllen; int msg_flags; };
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
int setsockopt(int, int, int, const void *, socklen_t);
int shutdown(int, int);
#ifdef __cplusplus
}
#endif
#endif
