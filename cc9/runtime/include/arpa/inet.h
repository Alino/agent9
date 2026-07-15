#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
#ifdef __cplusplus
extern "C" {
#endif
const char *inet_ntop(int, const void *, char *, socklen_t);
int inet_pton(int, const char *, void *);
in_addr_t inet_addr(const char *);
int inet_aton(const char *, struct in_addr *);
char *inet_ntoa(struct in_addr);
#ifdef __cplusplus
}
#endif
#endif
