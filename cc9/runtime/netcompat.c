/* netcompat.c — the no-kernel-needed slice of the BSD net API.
 *
 * inet_pton/ntop/aton/ntoa are pure string<->bytes conversions, implemented
 * for real. The resolver (gethostby-star, getservby-star) and datagram
 * send/recv are honest failures until the /net-backed socket layer lands
 * (they follow posix_llvm.c's "sockets are ENOSYS" line). */

#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>

extern int sprintf(char *, const char *, ...);
extern int snprintf(char *, unsigned long, const char *, ...);

int h_errno;

/* ---- IPv4 presentation <-> network ---- */

int inet_aton(const char *s, struct in_addr *a) {
	unsigned long parts[4];
	int n = 0;
	while (n < 4) {
		unsigned long v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') { v = v*10 + (*s++ - '0'); digits++; }
		if (!digits || v > 255) return 0;
		parts[n++] = v;
		if (*s == '.') { s++; continue; }
		break;
	}
	if (n != 4 || *s != '\0') return 0;
	a->s_addr = (in_addr_t)((parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3]);
	a->s_addr = ((a->s_addr & 0xff) << 24) | ((a->s_addr & 0xff00) << 8) |
	            ((a->s_addr >> 8) & 0xff00) | ((a->s_addr >> 24) & 0xff); /* to BE */
	return 1;
}

in_addr_t inet_addr(const char *s) {
	struct in_addr a;
	return inet_aton(s, &a) ? a.s_addr : (in_addr_t)-1;
}

char *inet_ntoa(struct in_addr a) {
	static char buf[16];
	unsigned char *p = (unsigned char *)&a.s_addr;
	sprintf(buf, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
	return buf;
}

int inet_pton(int af, const char *s, void *dst) {
	if (af == AF_INET) {
		struct in_addr a;
		if (!inet_aton(s, &a)) return 0;
		*(in_addr_t *)dst = a.s_addr;
		return 1;
	}
	if (af == AF_INET6) {
		/* groups of 16-bit hex, one optional "::" gap, optional v4 tail */
		unsigned char out[16];
		unsigned short grp[8];
		int ngrp = 0, gap = -1;
		if (s[0] == ':' && s[1] == ':') { gap = 0; s += 2; }
		while (*s && ngrp < 8) {
			const char *start = s;
			unsigned long v = 0;
			int digits = 0;
			while (digits < 4) {
				int c = *s;
				if (c >= '0' && c <= '9') v = v*16 + (c - '0');
				else if (c >= 'a' && c <= 'f') v = v*16 + (c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') v = v*16 + (c - 'A' + 10);
				else break;
				s++; digits++;
			}
			if (!digits) return 0;
			if (*s == '.') {                    /* v4-mapped tail */
				struct in_addr a4;
				if (ngrp > 6 || !inet_aton(start, &a4)) return 0;
				grp[ngrp++] = (unsigned short)(((unsigned char *)&a4.s_addr)[0] << 8 |
				                               ((unsigned char *)&a4.s_addr)[1]);
				grp[ngrp++] = (unsigned short)(((unsigned char *)&a4.s_addr)[2] << 8 |
				                               ((unsigned char *)&a4.s_addr)[3]);
				break;
			}
			grp[ngrp++] = (unsigned short)v;
			if (*s == ':') {
				s++;
				if (*s == ':') {
					if (gap >= 0) return 0;
					gap = ngrp; s++;
					if (!*s) break;
				} else if (!*s) return 0;
			} else if (*s) return 0;
		}
		if (*s) return 0;
		if (gap < 0 && ngrp != 8) return 0;
		if (gap >= 0 && ngrp >= 8) return 0;
		{
			int i, o = 0, pad = 8 - ngrp;
			for (i = 0; i < 8; i++) {
				unsigned short v;
				if (gap >= 0 && i >= gap && i < gap + pad) v = 0;
				else v = grp[i < gap || gap < 0 ? i : i - pad];
				out[o++] = (unsigned char)(v >> 8);
				out[o++] = (unsigned char)(v & 0xff);
			}
		}
		{ int i; for (i = 0; i < 16; i++) ((unsigned char *)dst)[i] = out[i]; }
		return 1;
	}
	errno = EAFNOSUPPORT;
	return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
	if (af == AF_INET) {
		const unsigned char *p = src;
		if (snprintf(dst, size, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]) >= (int)size) {
			errno = ENOSPC;
			return 0;
		}
		return dst;
	}
	if (af == AF_INET6) {
		/* longest zero run -> "::", RFC 5952-ish (no v4 special case) */
		const unsigned char *p = src;
		unsigned short g[8];
		int i, best = -1, bestlen = 0, run = -1, runlen = 0;
		char buf[46], *o = buf;
		for (i = 0; i < 8; i++) g[i] = (unsigned short)(p[2*i] << 8 | p[2*i+1]);
		for (i = 0; i < 8; i++) {
			if (g[i] == 0) { if (run < 0) { run = i; runlen = 0; } runlen++;
				if (runlen > bestlen) { best = run; bestlen = runlen; } }
			else run = -1;
		}
		if (bestlen < 2) best = -1;
		for (i = 0; i < 8; i++) {
			if (best >= 0 && i == best) { *o++ = ':'; if (i == 0) *o++ = ':'; i += bestlen - 1; continue; }
			o += sprintf(o, "%x", g[i]);
			if (i != 7) *o++ = ':';
		}
		*o = '\0';
		if ((socklen_t)(o - buf + 1) > size) { errno = ENOSPC; return 0; }
		{ char *d = dst; const char *s2 = buf; while ((*d++ = *s2++)); }
		return dst;
	}
	errno = EAFNOSUPPORT;
	return 0;
}

/* ---- resolver + dgram: honest failures until the /net socket layer ---- */

const char *hstrerror(int e) {
	switch (e) {
	case HOST_NOT_FOUND: return "host not found";
	case TRY_AGAIN:      return "try again";
	case NO_RECOVERY:    return "no recovery";
	case NO_DATA:        return "no data";
	}
	return "resolver error";
}

struct hostent *gethostbyname(const char *n) { (void)n; h_errno = NO_RECOVERY; return 0; }
struct hostent *gethostbyaddr(const void *a, socklen_t l, int t) { (void)a; (void)l; (void)t; h_errno = NO_RECOVERY; return 0; }
struct servent *getservbyname(const char *n, const char *p) { (void)n; (void)p; return 0; }
struct servent *getservbyport(int p, const char *pr) { (void)p; (void)pr; return 0; }

/* sendto/recvfrom are real in net9.c */
