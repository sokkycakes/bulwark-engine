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
/* ports added with sv_addport / cl_addport */
#define MAX_EXTRA_PORTS 8
static struct {
	int fd, port;
	bool v4;
} extras[2][MAX_EXTRA_PORTS];
static int numextras[2];
static void stream_close_all(netsrc_t src);
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
	/* no SO_REUSEADDR: for UDP it lets a second server share the port and
	 * take its packets */
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		Con_Printf("^3no UDP socket: %s\n", strerror(errno));
		return -1;
	}
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
	int s = socket(AF_INET6, SOCK_DGRAM, 0), off = 0;
	last_open_v4 = false;
	if (s < 0)
		return open_udp4(port);
	/* dual-stack: accept IPv4 peers as v4-mapped addresses */
	setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
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
	/* another server has the port: the next free one */
	for (int i = 1; sockets[NS_SERVER] < 0 && port && i < 10; i++)
		sockets[NS_SERVER] = open_udp(port + i);
	if (sockets[NS_SERVER] < 0)
		return false;
	sock_v4[NS_SERVER] = last_open_v4;
	getsockname(sockets[NS_SERVER], (struct sockaddr *)&sa, &len);
	/* the port sits at the same offset in sockaddr_in and sockaddr_in6 */
	server_port = ntohs(sa.sin6_port);
	Con_Printf("UDP server listening on port %d\n", server_port);
	return true;
}

void NET_CloseServer(void)
{
	close_socket(NS_SERVER);
	for (int i = 0; i < numextras[NS_SERVER]; i++)
		close(extras[NS_SERVER][i].fd);
	numextras[NS_SERVER] = 0;
	NET_OpenServerTCP(0);
	stream_close_all(NS_SERVER);
}
int NET_ServerPort(void) { return sockets[NS_SERVER] >= 0 ? server_port : 0; }

bool NET_OpenClient(void)
{
	if (sockets[NS_CLIENT] >= 0)
		return true;
	sockets[NS_CLIENT] = open_udp(0);
	sock_v4[NS_CLIENT] = last_open_v4;
	return sockets[NS_CLIENT] >= 0;
}

bool NET_AddPort(netsrc_t src, int port)
{
	if (numextras[src] >= MAX_EXTRA_PORTS) {
		Con_Printf("too many ports\n");
		return false;
	}
	int fd = open_udp(port);
	if (fd < 0)
		return false;
	struct sockaddr_in6 sa;
	socklen_t len = sizeof(sa);
	getsockname(fd, (struct sockaddr *)&sa, &len);
	extras[src][numextras[src]].fd = fd;
	extras[src][numextras[src]].v4 = last_open_v4;
	extras[src][numextras[src]].port = ntohs(sa.sin6_port);
	numextras[src]++;
	return true;
}

void NET_EnableBroadcast(void)
{
	int on = 1;
	if (NET_OpenClient())
		setsockopt(sockets[NS_CLIENT], SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
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
	bool tcp = false;
	memset(a, 0, sizeof(*a));
	if (!Str_NICmp(s, "tcp://", 6))
		s += 6, tcp = true;
	else if (!Str_NICmp(s, "udp://", 6))
		s += 6;
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
	a->tcp = tcp;
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
		snprintf(out, sizeof(buf[0]), "%s%s:%d", a->tcp ? "tcp://" : "", ip, a->port);
		return out;
	case NA_IP6:
		inet_ntop(AF_INET6, a->ip, ip, sizeof(ip));
		snprintf(out, sizeof(buf[0]), "%s[%s]:%d", a->tcp ? "tcp://" : "", ip, a->port);
		return out;
	default:
		return "none";
	}
}

bool NET_CompareBaseAdr(const netadr_t *a, const netadr_t *b)
{
	if (a->type != b->type || a->tcp != b->tcp)
		return false;
	if (a->type == NA_LOOPBACK)
		return true;
	return !memcmp(a->ip, b->ip, a->type == NA_IP4 ? 4 : 16);
}

bool NET_CompareAdr(const netadr_t *a, const netadr_t *b)
{
	return NET_CompareBaseAdr(a, b) && (a->type == NA_LOOPBACK || a->port == b->port);
}

static void stream_send(netsrc_t src, const void *data, size_t len, const netadr_t *to);
static size_t stream_get(netsrc_t src, void *buf, size_t bufsize, netadr_t *from);

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
	if (to->tcp) {
		stream_send(src, data, len, to);
		return;
	}
	if (sockets[src] < 0 && !(src == NS_CLIENT && NET_OpenClient()))
		return;
	int fd = sockets[src];
	bool v4 = sock_v4[src];
	if (to->sock && to->sock <= numextras[src])
		fd = extras[src][to->sock - 1].fd, v4 = extras[src][to->sock - 1].v4;
	if (v4) {
		struct sockaddr_in sa4;
		if (to->type != NA_IP4)
			return;
		memset(&sa4, 0, sizeof(sa4));
		sa4.sin_family = AF_INET;
		sa4.sin_port = htons(to->port);
		memcpy(&sa4.sin_addr, to->ip, 4);
		sendto(fd, data, len, 0, (struct sockaddr *)&sa4, sizeof(sa4));
	} else {
		struct sockaddr_in6 sa;
		adr_to_sockaddr(to, &sa);
		sendto(fd, data, len, 0, (struct sockaddr *)&sa, sizeof(sa));
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
	for (int i = 0; i <= numextras[src]; i++) {
		int fd = i ? extras[src][i - 1].fd : sockets[src];
		if (fd < 0)
			continue;
		struct sockaddr_in6 sa;
		socklen_t salen = sizeof(sa);
		ssize_t n = recvfrom(fd, buf, bufsize, 0, (struct sockaddr *)&sa, &salen);
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
			from->sock = (uint8_t)i;
			return (size_t)n;
		}
	}
	return stream_get(src, buf, bufsize, from);
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

/* --- TCP streams --- */

/* IPv4 sockets for IPv4 peers, so this works without IPv6 */
static int tcp_socket(netadrtype_t type)
{
	int s = socket(type == NA_IP6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
	if (s >= 0)
		fcntl(s, F_SETFL, O_NONBLOCK);
	return s;
}

int NET_TCPListen(int port)
{
	struct sockaddr_in sa;
	int on = 1, s = tcp_socket(NA_IP4);
	if (s < 0)
		return -1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(s, 8) < 0) {
		Con_Printf("^3TCP listen on port %d failed: %s\n", port, strerror(errno));
		close(s);
		return -1;
	}
	return s;
}

int NET_TCPAccept(int listener, netadr_t *from)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int s = listener >= 0 ? accept(listener, (struct sockaddr *)&sa, &len) : -1;
	if (s < 0)
		return -1;
	fcntl(s, F_SETFL, O_NONBLOCK);
	if (from) {
		memset(from, 0, sizeof(*from));
		from->type = NA_IP4;
		memcpy(from->ip, &sa.sin_addr, 4);
		from->port = ntohs(sa.sin_port);
	}
	return s;
}

int NET_TCPConnect(const netadr_t *to)
{
	int s;
	if (to->type != NA_IP4 && to->type != NA_IP6)
		return -1;
	if ((s = tcp_socket(to->type)) < 0)
		return -1;
	if (to->type == NA_IP4) {
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(to->port);
		memcpy(&sa.sin_addr, to->ip, 4);
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
			close(s);
			return -1;
		}
	} else {
		struct sockaddr_in6 sa;
		adr_to_sockaddr(to, &sa);
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
			close(s);
			return -1;
		}
	}
	return s;
}

/* bytes sent (maybe fewer than asked, 0 when it would block), -1 when the stream is gone */
int NET_TCPSend(int s, const void *data, size_t len)
{
	ssize_t n;
	if (s < 0)
		return -1;
	if (!len)
		return 0;
	n = send(s, data, len, MSG_NOSIGNAL);
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == ENOTCONN ? 0 : -1;
	return (int)n;
}

/* bytes read, 0 when nothing is waiting, -1 when the peer closed or failed */
int NET_TCPRecv(int s, void *buf, size_t size)
{
	ssize_t n;
	if (s < 0)
		return -1;
	n = recv(s, buf, size, 0);
	if (n == 0)
		return -1;
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == ENOTCONN ? 0 : -1;
	return (int)n;
}

void NET_TCPClose(int s)
{
	if (s >= 0)
		close(s);
}

/* --- packets over TCP ("tcp://" addresses) ---
 * Each side starts with a hello line, then every packet is a two byte big
 * endian length and the bytes. Streams are opened by the client's first
 * packet and accepted on the server's TCP port. */

#define STREAM_HELLO "BWSTREAM 1\n"
#define STREAM_HELLO_LEN 11
#define MAX_STREAMS 64

typedef struct {
	int fd;
	netsrc_t src;
	netadr_t adr;
	bool hello_in;
	uint8_t *in;
	size_t inlen;
	strbuf_t out;
	size_t outpos;
} stream_t;

static stream_t *streams[MAX_STREAMS];
static int tcp_listener = -1, tcp_port;

bool NET_OpenServerTCP(int port)
{
	NET_TCPClose(tcp_listener);
	tcp_listener = -1;
	tcp_port = 0;
	if (port <= 0)
		return true;
	tcp_listener = NET_TCPListen(port);
	if (tcp_listener < 0)
		return false;
	tcp_port = port;
	Con_Printf("TCP server listening on port %d\n", port);
	return true;
}

static void stream_free(int i)
{
	stream_t *st = streams[i];
	NET_TCPClose(st->fd);
	Mem_Free(st->in);
	SB_Free(&st->out);
	Mem_Free(st);
	streams[i] = NULL;
}

static void stream_close_all(netsrc_t src)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i] && streams[i]->src == src)
			stream_free(i);
}

static stream_t *stream_new(int fd, netsrc_t src, const netadr_t *adr)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (!streams[i]) {
			stream_t *st = streams[i] = Mem_Alloc(sizeof(*st));
			st->fd = fd;
			st->src = src;
			st->adr = *adr;
			st->adr.tcp = true;
			st->adr.sock = 0;
			st->in = Mem_Alloc(NET_MAX_RECV + 2);
			SB_Append(&st->out, STREAM_HELLO, STREAM_HELLO_LEN);
			return st;
		}
	NET_TCPClose(fd);
	return NULL;
}

/* false when the stream has gone */
static bool stream_flush(stream_t *st)
{
	while (st->outpos < st->out.len) {
		int n = NET_TCPSend(st->fd, st->out.data + st->outpos, st->out.len - st->outpos);
		if (n < 0)
			return false;
		if (!n)
			break;
		st->outpos += (size_t)n;
	}
	if (st->outpos == st->out.len)
		st->out.len = st->outpos = 0;
	return true;
}

static void stream_send(netsrc_t src, const void *data, size_t len, const netadr_t *to)
{
	stream_t *st = NULL;
	int i;
	if (len > 0xffff)
		return;
	for (i = 0; i < MAX_STREAMS; i++)
		if (streams[i] && streams[i]->src == src && NET_CompareAdr(&streams[i]->adr, to)) {
			st = streams[i];
			break;
		}
	if (!st) {
		if (src != NS_CLIENT)
			return; /* servers only answer streams clients opened */
		int fd = NET_TCPConnect(to);
		if (fd < 0 || !(st = stream_new(fd, src, to)))
			return;
		for (i = 0; streams[i] != st; i++)
			;
	}
	/* a stream that can't keep up drops packets, like UDP would */
	if (st->out.len - st->outpos > 256 * 1024)
		return;
	uint8_t head[2] = {(uint8_t)(len >> 8), (uint8_t)len};
	SB_Append(&st->out, (const char *)head, 2);
	SB_Append(&st->out, data, len);
	if (!stream_flush(st))
		stream_free(i);
}

static size_t stream_get(netsrc_t src, void *buf, size_t bufsize, netadr_t *from)
{
	if (src == NS_SERVER && tcp_listener >= 0) {
		netadr_t adr;
		int fd = NET_TCPAccept(tcp_listener, &adr);
		if (fd >= 0)
			stream_new(fd, src, &adr);
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		stream_t *st = streams[i];
		if (!st || st->src != src)
			continue;
		if (!stream_flush(st)) {
			stream_free(i);
			continue;
		}
		size_t want = NET_MAX_RECV + 2 - st->inlen;
		int n = want ? NET_TCPRecv(st->fd, st->in + st->inlen, want) : 0;
		if (n < 0) {
			stream_free(i);
			continue;
		}
		st->inlen += (size_t)n;
		if (!st->hello_in) {
			if (st->inlen < STREAM_HELLO_LEN)
				continue;
			if (memcmp(st->in, STREAM_HELLO, STREAM_HELLO_LEN)) {
				stream_free(i); /* not one of ours */
				continue;
			}
			st->hello_in = true;
			memmove(st->in, st->in + STREAM_HELLO_LEN, st->inlen - STREAM_HELLO_LEN);
			st->inlen -= STREAM_HELLO_LEN;
		}
		if (st->inlen < 2)
			continue;
		size_t plen = (size_t)st->in[0] << 8 | st->in[1];
		if (st->inlen < 2 + plen)
			continue;
		size_t copy = plen < bufsize ? plen : bufsize;
		memcpy(buf, st->in + 2, copy);
		memmove(st->in, st->in + 2 + plen, st->inlen - 2 - plen);
		st->inlen -= 2 + plen;
		*from = st->adr;
		return copy;
	}
	return 0;
}

void NET_ListPorts(netsrc_t src, void (*fn)(const char *desc, void *ctx), void *ctx)
{
	char line[128];
	if (sockets[src] >= 0) {
		struct sockaddr_in6 sa;
		socklen_t len = sizeof(sa);
		getsockname(sockets[src], (struct sockaddr *)&sa, &len);
		snprintf(line, sizeof(line), "udp%s port %d", sock_v4[src] ? "4" : "", ntohs(sa.sin6_port));
		fn(line, ctx);
	}
	for (int i = 0; i < numextras[src]; i++) {
		snprintf(line, sizeof(line), "udp%s port %d (added)", extras[src][i].v4 ? "4" : "", extras[src][i].port);
		fn(line, ctx);
	}
	if (src == NS_SERVER && tcp_listener >= 0) {
		snprintf(line, sizeof(line), "tcp4 port %d", tcp_port);
		fn(line, ctx);
	}
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i] && streams[i]->src == src) {
			snprintf(line, sizeof(line), "tcp stream %s", NET_AdrToString(&streams[i]->adr));
			fn(line, ctx);
		}
}
