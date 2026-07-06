#ifndef _NET_IF_H
#define _NET_IF_H
#define IF_NAMESIZE 16
#ifdef __cplusplus
extern "C" {
#endif
unsigned int if_nametoindex(const char *);
char *if_indextoname(unsigned int, char *);
#ifdef __cplusplus
}
#endif
#endif
