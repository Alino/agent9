#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H
#define TCP_NODELAY  1
/* Linux's value. net9.c does not implement it (Plan 9's /net exposes no MSS
 * knob), so setsockopt will report failure — but socket2 references the constant
 * unconditionally and will not compile without it. */
#define TCP_MAXSEG   2
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT  6
#endif
