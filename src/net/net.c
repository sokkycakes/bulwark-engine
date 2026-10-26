/* Bulwark engine - packet transport (UDP and in-process loopback).
 * ISC licence, see LICENSE. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/common.h"
#include "net.h"

/* --- loopback: a ring of packets per direction --- */
#define LOOP_PACKETS 256
typedef struct {
	uint8_t data[NET_MAX_RECV];
	size_t len;
} looppacket_t;
typedef struct {
	looppacket_t *packets;
	unsigned get, send;
} loopback_t;
static loopback_t loops[2]; /* [NS_CLIENT] is what the client receives */

static int sockets[2] = {-1, -1};
static bool sock_v4[2]; /* the socket is IPv4 only (no IPv6 on this system) */
static bool last_open_v4;
static int server_port;

/* --- simulated lag/loss --- */
typedef struct delayed_s {
	struct delayed_s *next;
	double when;
	netsrc_t src;
	netadr_t to;
	size_t len;
	uint8_t data[];
} delayed_t;
static delayed_t *delayed_head;
static float fake_lag, fake_loss;

void NET_Init(void)
{
	for (int i = 0; i < 2; i++)
		if (!loops[i].packets)
			loops[i].packets = Mem_Alloc(sizeof(looppacket_t) * LOOP_PACKETS);
}

static void close_socket(int which)
{
	if (sockets[which] >= 0)
		close(sockets[which]);
	sockets[which] = -1;
}

void NET_Shutdown(void)
{
	close_socket(0);
	close_socket(1);
}

/* an IPv4-only socket, for systems without IPv6 */
static int open_udp4(int port)
{
	struct sockaddr_in sa;
	int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
	if (s < 0) {
		Con_Printf("^3no UDP socket: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
		Con_Printf("^3UDP bind to port %d failed: %s\n", port, strerror(errno));
		close(s);
		return -1;
	}
	last_open_v4 = true;
	return s;
}

static int open_udp(int port)
{
	struct sockaddr_in6 sa;
	int s = socket(AF_INET6, SOCK_DGRAM, 0), off = 0, on = 1;
	last_open_v4 = false;
	if (s < 0)
		return open_udp4(port);
	/* dual-stack: accept IPv4 peers as v4-mapped addresses */
	setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = in6addr_any;
	sa.sin6_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
		Con_Printf("^3UDP bind to port %d failed: %s\n", port, strerror(errno));
		close(s);
		return -1;
	}
	return s;
}

bool NET_OpenServer(int port)
{
	struct sockaddr_in6 sa;
	socklen_t len = sizeof(sa);
	close_socket(NS_SERVER);
	sockets[NS_SERVER] = open_udp(port);
	if (sockets[NS_SERVER] < 0)
		return false;
	sock_v4[NS_SERVER] = last_open_v4;
	getsockname(sockets[NS_SERVER], (struct sockaddr *)&sa, &len);
	/* the port sits at the same offset in sockaddr_in and sockaddr_in6 */
	server_port = ntohs(sa.sin6_port);
	Con_Printf("UDP server listening on port %d\n", server_port);
	return true;
}

void NET_CloseServer(void) { close_socket(NS_SERVER); }
int NET_ServerPort(void) { return sockets[NS_SERVER] >= 0 ? server_port : 0; }

bool NET_OpenClient(void)
{
	if (sockets[NS_CLIENT] >= 0)
		return true;
	sockets[NS_CLIENT] = open_udp(0);
	sock_v4[NS_CLIENT] = last_open_v4;
	return sockets[NS_CLIENT] >= 0;
}

static void adr_to_sockaddr(const netadr_t *a, struct sockaddr_in6 *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin6_family = AF_INET6;
	sa->sin6_port = htons(a->port);
	if (a->type == NA_IP4) {
		sa->sin6_addr.s6_addr[10] = 0xff;
		sa->sin6_addr.s6_addr[11] = 0xff;
		memcpy(&sa->sin6_addr.s6_addr[12], a->ip, 4);
	} else
		memcpy(&sa->sin6_addr, a->ip, 16);
}

static void sockaddr_to_adr(const struct sockaddr_in6 *sa, netadr_t *a)
{
	static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
	memset(a, 0, sizeof(*a));
	a->port = ntohs(sa->sin6_port);
	if (!memcmp(sa->sin6_addr.s6_addr, mapped, 12)) {
		a->type = NA_IP4;
		memcpy(a->ip, &sa->sin6_addr.s6_addr[12], 4);
	} else {
		a->type = NA_IP6;
		memcpy(a->ip, &sa->sin6_addr, 16);
	}
}

bool NET_StringToAdr(const char *s, netadr_t *a, int defport)
{
	char host[256], *colon;
	const char *port = NULL;
	struct addrinfo hints, *res;
	memset(a, 0, sizeof(*a));
	if (!Str_ICmp(s, "localhost") || !Str_ICmp(s, "loopback") || !Str_ICmp(s, "local")) {
		a->type = NA_LOOPBACK;
		return true;
	}
	Str_Copy(host, s, sizeof(host));
	if (host[0] == '[') { /* [v6]:port */
		char *end = strchr(host, ']');
		if (!end)
			return false;
		*end = 0;
		if (end[1] == ':')
			port = end + 2;
		memmove(host, host + 1, strlen(host));
	} else if ((colon = strchr(host, ':')) && !strchr(colon + 1, ':')) {
		*colon = 0;
		port = colon + 1;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &res) || !res)
		return false;
	if (res->ai_family == AF_INET) {
		a->type = NA_IP4;
		memcpy(a->ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
	} else if (res->ai_family == AF_INET6) {
		a->type = NA_IP6;
		memcpy(a->ip, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, 16);
	}
	freeaddrinfo(res);
	a->port = (uint16_t)(port ? atoi(port) : defport);
	return a->type != NA_NONE;
}

const char *NET_AdrToString(const netadr_t *a)
{
	static char buf[4][80];
	static int which;
	char *out = buf[which++ & 3], ip[64];
	switch (a->type) {
	case NA_LOOPBACK:
		return "loopback";
	case NA_IP4:
		inet_ntop(AF_INET, a->ip, ip, sizeof(ip));
		snprintf(out, sizeof(buf[0]), "%s:%d", ip, a->port);
		return out;
	case NA_IP6:
		inet_ntop(AF_INET6, a->ip, ip, sizeof(ip));
		snprintf(out, sizeof(buf[0]), "[%s]:%d", ip, a->port);
		return out;
	default:
		return "none";
	}
}

bool NET_CompareBaseAdr(const netadr_t *a, const netadr_t *b)
{
	if (a->type != b->type)
		return false;
	if (a->type == NA_LOOPBACK)
		return true;
	return !memcmp(a->ip, b->ip, a->type == NA_IP4 ? 4 : 16);
}

bool NET_CompareAdr(const netadr_t *a, const netadr_t *b)
{
	return NET_CompareBaseAdr(a, b) && (a->type == NA_LOOPBACK || a->port == b->port);
}

static void send_now(netsrc_t src, const void *data, size_t len, const netadr_t *to)
{
	if (to->type == NA_LOOPBACK) {
		loopback_t *l = &loops[src == NS_CLIENT ? NS_SERVER : NS_CLIENT];
		looppacket_t *p;
		if (!l->packets || len > NET_MAX_RECV)
			return;
		if (l->send - l->get >= LOOP_PACKETS)
			l->get = l->send - LOOP_PACKETS + 1; /* drop the oldest */
		p = &l->packets[l->send++ % LOOP_PACKETS];
		memcpy(p->data, data, len);
		p->len = len;
		return;
	}
	if (sockets[src] < 0 && !(src == NS_CLIENT && NET_OpenClient()))
		return;
	if (sock_v4[src]) {
		struct sockaddr_in sa4;
		if (to->type != NA_IP4)
			return;
		memset(&sa4, 0, sizeof(sa4));
		sa4.sin_family = AF_INET;
		sa4.sin_port = htons(to->port);
		memcpy(&sa4.sin_addr, to->ip, 4);
		sendto(sockets[src], data, len, 0, (struct sockaddr *)&sa4, sizeof(sa4));
	} else {
		struct sockaddr_in6 sa;
		adr_to_sockaddr(to, &sa);
		sendto(sockets[src], data, len, 0, (struct sockaddr *)&sa, sizeof(sa));
	}
}

void NET_SetFakeConditions(float lag_ms, float loss_percent)
{
	fake_lag = lag_ms;
	fake_loss = loss_percent;
}

void NET_SendPacket(netsrc_t src, const void *data, size_t len, const netadr_t *to)
{
	if (fake_loss > 0 && (rand() % 10000) < fake_loss * 100)
		return;
	if (fake_lag > 0) {
		delayed_t *d = Mem_Alloc(sizeof(*d) + len), **link = &delayed_head;
		d->when = host_time + fake_lag / 1000.0;
		d->src = src;
		d->to = *to;
		d->len = len;
		memcpy(d->data, data, len);
		while (*link)
			link = &(*link)->next;
		*link = d;
		return;
	}
	send_now(src, data, len, to);
}

void NET_RunDelayed(void)
{
	double now = host_time;
	while (delayed_head && delayed_head->when <= now) {
		delayed_t *d = delayed_head;
		delayed_head = d->next;
		send_now(d->src, d->data, d->len, &d->to);
		Mem_Free(d);
	}
}

size_t NET_GetPacket(netsrc_t src, void *buf, size_t bufsize, netadr_t *from)
{
	loopback_t *l = &loops[src];
	if (l->packets && l->get != l->send) {
		looppacket_t *p = &l->packets[l->get++ % LOOP_PACKETS];
		size_t n = p->len < bufsize ? p->len : bufsize;
		memcpy(buf, p->data, n);
		memset(from, 0, sizeof(*from));
		from->type = NA_LOOPBACK;
		return n;
	}
	if (sockets[src] >= 0) {
		struct sockaddr_in6 sa;
		socklen_t salen = sizeof(sa);
		ssize_t n = recvfrom(sockets[src], buf, bufsize, 0, (struct sockaddr *)&sa, &salen);
		if (n > 0) {
			if (((struct sockaddr *)&sa)->sa_family == AF_INET) {
				const struct sockaddr_in *sa4 = (const struct sockaddr_in *)&sa;
				memset(from, 0, sizeof(*from));
				from->type = NA_IP4;
				from->port = ntohs(sa4->sin_port);
				memcpy(from->ip, &sa4->sin_addr, 4);
			} else {
				sockaddr_to_adr(&sa, from);
			}
			return (size_t)n;
		}
	}
	return 0;
}

void NET_OutOfBandPrint(netsrc_t src, const netadr_t *to, const char *fmt, ...)
{
	char buf[NET_MAX_PACKET];
	va_list ap;
	int n;
	memset(buf, 0xff, 4);
	va_start(ap, fmt);
	n = vsnprintf(buf + 4, sizeof(buf) - 4, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 5)
		n = (int)sizeof(buf) - 5;
	NET_SendPacket(src, buf, (size_t)n + 4, to);
}
