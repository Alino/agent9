#ifndef _NETDB_H
#define _NETDB_H
/* getaddrinfo/getnameinfo are EAI_FAIL stubs (posix_llvm.c) — no resolver;
 * Plan 9 name resolution is /net/cs. Headers exist so callers compile. */
#include <sys/socket.h>
struct addrinfo {
	int ai_flags, ai_family, ai_socktype, ai_protocol;
	socklen_t ai_addrlen;
	struct sockaddr *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};
struct protoent { char *p_name; char **p_aliases; int p_proto; };
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
#define AI_PASSIVE     1
#define AI_CANONNAME   2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 8
#define AI_ADDRCONFIG  0x20
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD    8
#define NI_DGRAM       16
#define NI_MAXHOST     1025
#define NI_MAXSERV     32
#define EAI_ADDRFAMILY (-9)
#define EAI_AGAIN      (-3)
#define EAI_BADFLAGS   (-1)
#define EAI_FAIL       (-4)
#define EAI_FAMILY     (-6)
#define EAI_MEMORY     (-10)
#define EAI_NODATA     (-5)
#define EAI_NONAME     (-2)
#define EAI_OVERFLOW   (-12)
#define EAI_SERVICE    (-8)
#define EAI_SOCKTYPE   (-7)
#define EAI_SYSTEM     (-11)
#define MAXHOSTNAMELEN 256
#ifdef __cplusplus
extern "C" {
#endif
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
const char *gai_strerror(int);
struct protoent *getprotobyname(const char *);
struct protoent *getprotobynumber(int);
extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
const char *hstrerror(int);
struct hostent *gethostbyname(const char *);
struct hostent *gethostbyaddr(const void *, socklen_t, int);
struct servent *getservbyname(const char *, const char *);
struct servent *getservbyport(int, const char *);
#ifdef __cplusplus
}
#endif
#endif
