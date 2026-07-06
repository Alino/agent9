#ifndef _IFADDRS_H
#define _IFADDRS_H
#include <sys/socket.h>
struct ifaddrs {
	struct ifaddrs *ifa_next;
	char *ifa_name;
	unsigned int ifa_flags;
	struct sockaddr *ifa_addr, *ifa_netmask, *ifa_dstaddr;
	void *ifa_data;
};
#define ifa_broadaddr ifa_dstaddr
#ifdef __cplusplus
extern "C" {
#endif
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
#ifdef __cplusplus
}
#endif
#endif
