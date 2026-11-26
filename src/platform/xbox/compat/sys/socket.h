/* Bulwark engine - BSD sockets on the Xbox: lwIP's socket API (nxdk's net
 * library), under the POSIX names (socket, bind, close, fcntl, ...).
 * The network starts on first use (posix_xbox.c, bwx_net_up).
 * ISC licence, see LICENSE. */
#ifndef BW_XBOX_SYS_SOCKET_H
#define BW_XBOX_SYS_SOCKET_H

#include <sys/types.h>
#include <lwip/opt.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>

/* brings up the network interface (DHCP) the first time a socket is made;
 * false when there is no link */
int bwx_net_up(void);
int bwx_socket(int domain, int type, int proto);
#undef socket
#define socket(d, t, p) bwx_socket(d, t, p)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* the engine calls fcntl(s, F_GETFL) with two arguments, lwIP's macro
 * wants three */
#include <stdarg.h>
#undef fcntl
static inline int bwx_fcntl(int s, int cmd, ...)
{
	va_list ap;
	int v;
	va_start(ap, cmd);
	v = cmd == F_SETFL ? va_arg(ap, int) : 0;
	va_end(ap);
	return lwip_fcntl(s, cmd, v);
}
#define fcntl bwx_fcntl

/* lwIP's port pulls in windef.h, whose empty far and near collide with
 * ordinary names */
#undef far
#undef near

#endif
