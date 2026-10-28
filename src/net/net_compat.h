/* Bulwark engine - socket API gaps on consoles. ISC licence, see LICENSE.
 *
 * The 3DS (libctru) and DS (dswifi) socket layers are IPv4 only: they
 * name AF_INET6 but have no sockaddr_in6, in6addr_any or IPV6_V6ONLY, and
 * dswifi has no inet_ntop/inet_pton either. The transport code already
 * falls back to an IPv4 socket when an AF_INET6 one can't be opened, so
 * these definitions only have to make it compile; at run time
 * socket(AF_INET6) fails and the v4 paths are taken. Include after the
 * system socket headers. */
#ifndef BW_NET_COMPAT_H
#define BW_NET_COMPAT_H

#if defined(__3DS__) || defined(__NDS__)
#define BW_NO_IPV6 1
#endif

#ifdef BW_NO_IPV6
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 26
#endif
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct in6_addr {
	uint8_t s6_addr[16];
};
/* the port sits at the same offset as in sockaddr_in, which the code that
 * reads ports back from getsockname relies on */
struct sockaddr_in6 {
	uint16_t sin6_family;
	uint16_t sin6_port;
	uint32_t sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t sin6_scope_id;
};
static const struct in6_addr bw_in6addr_any;
#define in6addr_any bw_in6addr_any

#ifdef __NDS__
/* dswifi has neither; IPv4 is all it can carry anyway */
static inline const char *bw_inet_ntop(int af, const void *src, char *dst, int size)
{
	const uint8_t *b = src;
	if (af != AF_INET) {
		snprintf(dst, (size_t)size, "::");
		return dst;
	}
	snprintf(dst, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	return dst;
}
static inline int bw_inet_pton(int af, const char *src, void *dst)
{
	unsigned a, b, c, d;
	uint8_t *o = dst;
	if (af != AF_INET || sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 ||
	    d > 255)
		return 0;
	o[0] = (uint8_t)a, o[1] = (uint8_t)b, o[2] = (uint8_t)c, o[3] = (uint8_t)d;
	return 1;
}
#define inet_ntop(af, src, dst, size) bw_inet_ntop(af, src, dst, (int)(size))
#define inet_pton bw_inet_pton
#endif
#endif

#endif
