#ifndef _NETINET_IN_H
#define _NETINET_IN_H
/* Types/constants only — cc9 has no BSD socket layer (Plan 9 networking is
 * /net); these let socket-flavored code (libuv tcp/udp) COMPILE. The calls
 * themselves are ENOSYS stubs in posix_llvm.c. */
#include <sys/socket.h>
typedef unsigned short in_port_t;
typedef unsigned int in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in {
	sa_family_t sin_family; in_port_t sin_port;
	struct in_addr sin_addr; char sin_zero[8];
};
struct sockaddr_in6 {
	sa_family_t sin6_family; in_port_t sin6_port;
	unsigned int sin6_flowinfo; struct in6_addr sin6_addr;
	unsigned int sin6_scope_id;
};
struct ip_mreq { struct in_addr imr_multiaddr, imr_interface; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned int ipv6mr_interface; };
struct ip_mreq_source { struct in_addr imr_multiaddr, imr_interface, imr_sourceaddr; };
struct group_source_req { unsigned int gsr_interface; struct sockaddr_storage gsr_group, gsr_source; };
extern const struct in6_addr in6addr_any;
#define INADDR_ANY       0u
#define INADDR_LOOPBACK  0x7f000001u
#define INADDR_BROADCAST 0xffffffffu
#define IN6ADDR_ANY_INIT {{0}}
#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IP_TOS             1
#define IP_TTL             2
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36
#define IP_ADD_SOURCE_MEMBERSHIP  39
#define IP_DROP_SOURCE_MEMBERSHIP 40
#define MCAST_JOIN_SOURCE_GROUP   46
#define MCAST_LEAVE_SOURCE_GROUP  47
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_V6ONLY         26
#define IPV6_ADD_MEMBERSHIP  IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
/* x86-64 target is little-endian, always swap */
#define htons(x) __builtin_bswap16((unsigned short)(x))
#define ntohs(x) __builtin_bswap16((unsigned short)(x))
#define htonl(x) __builtin_bswap32((unsigned int)(x))
#define ntohl(x) __builtin_bswap32((unsigned int)(x))
#endif
