/* Bulwark engine - Bulwark Live client (docs/spec/live.md).
 *
 * One UDP socket talks to the bwlive server and, once a hole is punched, to
 * each peer directly; until then (or if punching never works) packets go
 * through the server's relay. Game traffic rides on it as "live:<id>"
 * addresses, so connect live:<id> reaches a listen server behind a NAT.
 * Original code, ISC licence. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/cmd.h"
#include "../common/common.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "net_compat.h"
#include "live.h"

/* message types */
enum {
	T_HELLO = 1, T_BYE, T_PARTY_CREATE, T_PARTY_JOIN, T_PARTY_LEAVE, T_PARTY_SET, T_INVITE,
	T_QUEUE, T_QUEUE_CANCEL, T_RELAY, T_STATUS, T_LIST, T_MATCH_LEFT, T_PONG,
	T_WELCOME = 64, T_ERROR, T_PARTY, T_PARTY_GONE, T_INVITED, T_INTRODUCE, T_FORWARD,
	T_QUEUE_STATUS, T_MATCH, T_PLAYERS, T_PING,
	T_PUNCH = 96, T_PUNCH_ACK, T_DATA
};

#define MAX_DGRAM 1472
#define MIN_HELLO 64
#define MAX_PEERS 32
#define MAX_MEMBERS 16
#define MAX_GUESTS 3
#define MAX_SEATS 16
#define MAX_SETTINGS 32
#define MAX_INVITES 8
#define QUEUE_LEN 128

#define PUNCH_EVERY 0.3
#define PUNCH_FOR 6.0
#define PUNCH_RETRY 30.0
#define DIRECT_IDLE 12.0 /* a direct path this quiet is given up on */
#define KEEPALIVE 4.0

typedef struct {
	char name[64];
	struct sockaddr_in pub;
	struct sockaddr_in lan[4];
	int nlan;
	char guests[MAX_GUESTS][64];
	int nguests;
	uint32_t id;
} member_t;

typedef struct {
	bool used;
	uint32_t id;
	char name[64];
	struct sockaddr_in pub, lan[4];
	int nlan;
	uint64_t nonce;
	bool direct;
	struct sockaddr_in path;
	double last_rx, last_tx, punch_until, next_punch, retry_at;
} peer_t;

typedef struct {
	uint32_t from;
	uint8_t srcport;
	uint16_t len;
	uint8_t data[MAX_DGRAM];
} packet_t;

typedef struct {
	packet_t p[QUEUE_LEN];
	unsigned get, put;
} pqueue_t;

static struct {
	int sock;
	struct sockaddr_in server;
	bool have_server;
	uint64_t token;
	uint32_t id;
	struct sockaddr_in pub;
	bool connected;
	double last_hello, last_welcome, hello_sent;
	int rtt_ms;
	char guests[MAX_GUESTS][64];
	int nguests;

	bool in_party;
	char code[8];
	uint32_t leader;
	int max, flags;
	char info[256];
	member_t members[MAX_MEMBERS];
	int nmembers;

	bool queued;
	char playlist[64];
	int stage, waited, searching;

	uint64_t match_id;
	char match_playlist[64];
	uint32_t match_host;
	char match_mode[40], match_map[72];
	struct {
		char key[56], value[72];
	} settings[MAX_SETTINGS];
	int nsettings;
	struct {
		uint32_t id;
		int slot, team;
		char name[64];
	} seats[MAX_SEATS];
	int nseats;

	struct {
		uint32_t from;
		char name[64], code[8];
	} invites[MAX_INVITES];
	int ninvites;

	peer_t peers[MAX_PEERS];
	pqueue_t game[2], lobby;
} L = {.sock = -1};

static cvar_t *live_punch, *live_party, *live_server, *live_game, *live_autoconnect, *live_hostcmd, *live_state, *live_onconnect;

static double now(void) { return Sys_Seconds(); }

/* --- writing ---------------------------------------------------------------- */

typedef struct {
	uint8_t b[MAX_DGRAM];
	size_t n;
	bool over;
} wbuf_t;

static void w_start(wbuf_t *w, int type)
{
	memcpy(w->b, "BWLV", 4);
	w->b[4] = 1;
	w->b[5] = (uint8_t)type;
	w->n = 6;
	w->over = false;
}
static void w_bytes(wbuf_t *w, const void *p, size_t n)
{
	if (w->n + n > sizeof(w->b)) {
		w->over = true;
		return;
	}
	memcpy(w->b + w->n, p, n);
	w->n += n;
}
static void w_u8(wbuf_t *w, unsigned v)
{
	uint8_t b = (uint8_t)v;
	w_bytes(w, &b, 1);
}
static void w_u32(wbuf_t *w, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	w_bytes(w, b, 4);
}
static void w_u64(wbuf_t *w, uint64_t v)
{
	w_u32(w, (uint32_t)(v >> 32));
	w_u32(w, (uint32_t)v);
}
static void w_str(wbuf_t *w, const char *s)
{
	size_t n = strlen(s);
	if (n > 255)
		n = 255;
	w_u8(w, (unsigned)n);
	w_bytes(w, s, n);
}
static void w_addr(wbuf_t *w, const struct sockaddr_in *a)
{
	w_u8(w, 4);
	w_bytes(w, &a->sin_addr, 4);
	w_bytes(w, &a->sin_port, 2); /* already big-endian */
}

/* --- reading ---------------------------------------------------------------- */

typedef struct {
	const uint8_t *b;
	size_t n, at;
	bool bad;
} rbuf_t;

static const uint8_t *r_take(rbuf_t *r, size_t n)
{
	if (r->bad || r->at + n > r->n) {
		r->bad = true;
		return NULL;
	}
	r->at += n;
	return r->b + r->at - n;
}
static unsigned r_u8(rbuf_t *r)
{
	const uint8_t *p = r_take(r, 1);
	return p ? p[0] : 0;
}
static unsigned r_u16(rbuf_t *r)
{
	const uint8_t *p = r_take(r, 2);
	return p ? (unsigned)(p[0] << 8 | p[1]) : 0;
}
static uint32_t r_u32(rbuf_t *r)
{
	const uint8_t *p = r_take(r, 4);
	return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0;
}
static uint64_t r_u64(rbuf_t *r)
{
	uint64_t hi = r_u32(r);
	return hi << 32 | r_u32(r);
}
static void r_str(rbuf_t *r, char *out, size_t size)
{
	unsigned n = r_u8(r);
	const uint8_t *p = r_take(r, n);
	size_t k = p ? (n < size - 1 ? n : size - 1) : 0;
	if (p)
		memcpy(out, p, k);
	out[k] = 0;
}
/* an addr; IPv6 ones are read past and left zero (this client is IPv4) */
static void r_addr(rbuf_t *r, struct sockaddr_in *a)
{
	memset(a, 0, sizeof(*a));
	unsigned fam = r_u8(r);
	const uint8_t *ip = r_take(r, fam == 6 ? 16 : 4);
	const uint8_t *port = r_take(r, 2);
	if (fam != 4 && fam != 6)
		r->bad = true;
	if (!ip || !port || fam != 4)
		return;
	a->sin_family = AF_INET;
	memcpy(&a->sin_addr, ip, 4);
	memcpy(&a->sin_port, port, 2);
}
static int r_addrs(rbuf_t *r, struct sockaddr_in *out, int max)
{
	int n = (int)r_u8(r), k = 0;
	for (int i = 0; i < n; i++) {
		struct sockaddr_in a;
		r_addr(r, &a);
		if (a.sin_family == AF_INET && k < max)
			out[k++] = a;
	}
	return k;
}

/* --- helpers ---------------------------------------------------------------- */

static const char *addr_str(const struct sockaddr_in *a)
{
	static char buf[4][32];
	static int which;
	char *out = buf[which++ & 3], ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
	snprintf(out, sizeof(buf[0]), "%s:%d", ip, ntohs(a->sin_port));
	return out;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_raw(const struct sockaddr_in *to, const void *data, size_t len)
{
	if (L.sock >= 0 && to->sin_family == AF_INET)
		sendto(L.sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void send_server(wbuf_t *w)
{
	if (!w->over && L.have_server)
		send_raw(&L.server, w->b, w->n);
}

static peer_t *find_peer(uint32_t id, bool create)
{
	peer_t *free_slot = NULL, *oldest = NULL;
	for (int i = 0; i < MAX_PEERS; i++) {
		peer_t *p = &L.peers[i];
		if (p->used && p->id == id)
			return p;
		if (!p->used && !free_slot)
			free_slot = p;
		if (p->used && (!oldest || p->last_rx < oldest->last_rx))
			oldest = p;
	}
	if (!create || !id)
		return NULL;
	peer_t *p = free_slot ? free_slot : oldest;
	memset(p, 0, sizeof(*p));
	p->used = true;
	p->id = id;
	return p;
}

static void push(pqueue_t *q, uint32_t from, uint8_t srcport, const uint8_t *data, size_t len)
{
	if (len > sizeof(q->p[0].data))
		return;
	if (q->put - q->get >= QUEUE_LEN)
		q->get++; /* drop the oldest */
	packet_t *p = &q->p[q->put++ % QUEUE_LEN];
	p->from = from;
	p->srcport = srcport;
	p->len = (uint16_t)len;
	memcpy(p->data, data, len);
}

static size_t pop(pqueue_t *q, void *buf, size_t size, uint32_t *from, uint8_t *srcport)
{
	if (q->get == q->put)
		return 0;
	packet_t *p = &q->p[q->get++ % QUEUE_LEN];
	size_t n = p->len < size ? p->len : size;
	memcpy(buf, p->data, n);
	*from = p->from;
	*srcport = p->srcport;
	return n;
}

static void json_str(strbuf_t *sb, const char *s)
{
	SB_AppendStr(sb, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			SB_Printf(sb, "\\%c", c);
		else if (c < 0x20)
			SB_Printf(sb, "\\u%04x", c);
		else
			SB_Append(sb, (const char *)&c, 1);
	}
	SB_AppendStr(sb, "\"");
}

/* live_state: what the menus and QC read */
static void update_state(void)
{
	strbuf_t sb = {0};
	SB_Printf(&sb, "{\"connected\":%s,\"id\":%u,\"public\":", L.connected ? "true" : "false", L.id);
	json_str(&sb, L.connected ? addr_str(&L.pub) : "");
	SB_Printf(&sb, ",\"rtt\":%d,\"party\":", L.rtt_ms);
	if (L.in_party) {
		SB_AppendStr(&sb, "{\"code\":");
		json_str(&sb, L.code);
		SB_Printf(&sb, ",\"leader\":%u,\"max\":%d,\"flags\":%d,\"info\":", L.leader, L.max, L.flags);
		json_str(&sb, L.info);
		SB_AppendStr(&sb, ",\"members\":[");
		for (int i = 0; i < L.nmembers; i++) {
			member_t *m = &L.members[i];
			peer_t *p = find_peer(m->id, false);
			SB_Printf(&sb, "%s{\"id\":%u,\"name\":", i ? "," : "", m->id);
			json_str(&sb, m->name);
			SB_Printf(&sb, ",\"direct\":%s,\"guests\":[", p && p->direct ? "true" : "false");
			for (int g = 0; g < m->nguests; g++) {
				if (g)
					SB_AppendStr(&sb, ",");
				json_str(&sb, m->guests[g]);
			}
			SB_AppendStr(&sb, "]}");
		}
		SB_AppendStr(&sb, "],\"mode\":");
		json_str(&sb, L.match_mode);
		SB_AppendStr(&sb, ",\"map\":");
		json_str(&sb, L.match_map);
		SB_AppendStr(&sb, ",\"settings\":{");
		for (int i = 0; i < L.nsettings; i++) {
			if (i)
				SB_AppendStr(&sb, ",");
			json_str(&sb, L.settings[i].key);
			SB_AppendStr(&sb, ":");
			json_str(&sb, L.settings[i].value);
		}
		SB_AppendStr(&sb, "}}");
	} else {
		SB_AppendStr(&sb, "null");
	}
	SB_AppendStr(&sb, ",\"queue\":");
	if (L.queued) {
		SB_AppendStr(&sb, "{\"playlist\":");
		json_str(&sb, L.playlist);
		SB_Printf(&sb, ",\"stage\":%d,\"waited\":%d,\"searching\":%d}", L.stage, L.waited, L.searching);
	} else {
		SB_AppendStr(&sb, "null");
	}
	SB_AppendStr(&sb, ",\"match\":");
	if (L.match_id) {
		SB_Printf(&sb, "{\"id\":\"%llu\",\"playlist\":", (unsigned long long)L.match_id);
		json_str(&sb, L.match_playlist);
		SB_Printf(&sb, ",\"host\":%u,\"seats\":[", L.match_host);
		for (int i = 0; i < L.nseats; i++) {
			SB_Printf(&sb, "%s{\"id\":%u,\"slot\":%d,\"team\":%d,\"name\":", i ? "," : "", L.seats[i].id,
			          L.seats[i].slot, L.seats[i].team);
			json_str(&sb, L.seats[i].name);
			SB_AppendStr(&sb, "}");
		}
		SB_AppendStr(&sb, "]}");
	} else {
		SB_AppendStr(&sb, "null");
	}
	SB_AppendStr(&sb, ",\"invites\":[");
	for (int i = 0; i < L.ninvites; i++) {
		SB_Printf(&sb, "%s{\"from\":%u,\"name\":", i ? "," : "", L.invites[i].from);
		json_str(&sb, L.invites[i].name);
		SB_AppendStr(&sb, ",\"code\":");
		json_str(&sb, L.invites[i].code);
		SB_AppendStr(&sb, "}");
	}
	SB_AppendStr(&sb, "]}");
	Cvar_SetDirect(live_state, sb.data ? sb.data : "");
	SB_Free(&sb);
}

/* --- identity --------------------------------------------------------------- */

static uint64_t random64(void)
{
	uint64_t v = 0;
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(&v, sizeof(v), 1, f) != 1)
			v = 0;
		fclose(f);
	}
	if (!v)
		v = (uint64_t)(Sys_Seconds() * 1e6) ^ (uint64_t)getpid() << 40 ^ (uint64_t)(uintptr_t)&v;
	return v;
}

/* the token survives restarts in live_identity.txt, so a quick restart
 * keeps the player's party and id */
static uint64_t load_token(void)
{
	char path[1024];
	unsigned long long v = 0;
	if (!FS_WritePath("live_identity.txt", path, sizeof(path)))
		return random64();
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%llx", &v) != 1)
			v = 0;
		fclose(f);
	}
	if (!v) {
		v = random64();
		FS_CreatePath(path);
		if ((f = fopen(path, "w"))) {
			fprintf(f, "%016llx\n", v);
			fclose(f);
		}
	}
	return v;
}

/* --- socket ----------------------------------------------------------------- */

static bool open_socket(void)
{
	if (L.sock >= 0)
		return true;
	struct sockaddr_in sa;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		Con_Printf("^3live: no UDP socket: %s\n", strerror(errno));
		return false;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)Cvar_Value("live_port"));
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
		Con_Printf("^3live: bind failed: %s\n", strerror(errno));
		close(s);
		return false;
	}
	L.sock = s;
	return true;
}

/* our LAN endpoint: the local address the OS would use to reach the server */
static bool lan_endpoint(struct sockaddr_in *out)
{
	struct sockaddr_in me;
	socklen_t len = sizeof(me);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return false;
	bool ok = connect(s, (const struct sockaddr *)&L.server, sizeof(L.server)) == 0 &&
	          getsockname(s, (struct sockaddr *)&me, &len) == 0;
	close(s);
	if (!ok || me.sin_addr.s_addr == htonl(INADDR_ANY))
		return false;
	struct sockaddr_in bound;
	len = sizeof(bound);
	if (getsockname(L.sock, (struct sockaddr *)&bound, &len) != 0)
		return false;
	*out = me;
	out->sin_port = bound.sin_port;
	return true;
}

static void send_hello(void)
{
	wbuf_t w;
	struct sockaddr_in lan;
	cvar_t *name = Cvar_Find("name");
	w_start(&w, T_HELLO);
	w_u64(&w, L.token);
	w_str(&w, live_game->string);
	w_str(&w, name && name->string[0] ? name->string : "player");
	if (lan_endpoint(&lan)) {
		w_u8(&w, 1);
		w_addr(&w, &lan);
	} else {
		w_u8(&w, 0);
	}
	w_u8(&w, (unsigned)L.nguests);
	for (int i = 0; i < L.nguests; i++)
		w_str(&w, L.guests[i]);
	while (w.n < MIN_HELLO)
		w_u8(&w, 0); /* padding: the answer must not be bigger than the question */
	send_server(&w);
	L.last_hello = L.hello_sent = now();
}

/* --- punching --------------------------------------------------------------- */

static void send_punch(peer_t *p, int type, const struct sockaddr_in *to)
{
	wbuf_t w;
	w_start(&w, type);
	w_u32(&w, L.id);
	w_u32(&w, p->id);
	w_u64(&w, p->nonce);
	send_raw(to, w.b, w.n);
	p->last_tx = now();
}

static void start_punch(peer_t *p)
{
	if (!live_punch->ivalue)
		return;
	double t = now();
	p->punch_until = t + PUNCH_FOR;
	p->next_punch = t;
	p->retry_at = t + PUNCH_RETRY;
}

static void punch_frame(double t)
{
	for (int i = 0; i < MAX_PEERS; i++) {
		peer_t *p = &L.peers[i];
		if (!p->used || !p->nonce)
			continue;
		if (p->direct && t - p->last_rx > DIRECT_IDLE) {
			Con_DPrintf("live: direct path to %u went quiet, using the relay\n", p->id);
			p->direct = false;
			update_state();
		}
		if (!p->direct && t < p->punch_until && t >= p->next_punch) {
			p->next_punch = t + PUNCH_EVERY;
			if (p->pub.sin_family == AF_INET)
				send_punch(p, T_PUNCH, &p->pub);
			for (int k = 0; k < p->nlan; k++)
				send_punch(p, T_PUNCH, &p->lan[k]);
		}
		if (!p->direct && t >= p->retry_at && p->punch_until < t)
			start_punch(p);
		/* keep the NAT mapping open on a direct path */
		if (p->direct && t - p->last_tx > KEEPALIVE)
			send_punch(p, T_PUNCH, &p->path);
	}
}

/* --- sending to a peer ------------------------------------------------------ */

static bool send_peer(uint32_t id, uint8_t srcport, uint8_t dstport, const void *data, size_t len)
{
	wbuf_t w;
	if (!L.connected || !id)
		return false;
	peer_t *p = find_peer(id, true);
	if (p->direct) {
		w_start(&w, T_DATA);
		w_u32(&w, L.id);
	} else {
		w_start(&w, T_RELAY);
		w_u32(&w, id);
	}
	w_u8(&w, srcport);
	w_u8(&w, dstport);
	w_bytes(&w, data, len);
	if (w.over)
		return false;
	if (p->direct) {
		send_raw(&p->path, w.b, w.n);
		p->last_tx = now();
	} else {
		send_server(&w);
	}
	return true;
}

static void deliver(uint32_t from, const uint8_t *payload, size_t len)
{
	if (len < 2)
		return;
	uint8_t src = payload[0], dst = payload[1];
	if (dst == LIVE_VPORT_SERVER)
		push(&L.game[NS_SERVER], from, src, payload + 2, len - 2);
	else if (dst == LIVE_VPORT_CLIENT)
		push(&L.game[NS_CLIENT], from, src, payload + 2, len - 2);
	else if (dst == LIVE_VPORT_LOBBY)
		push(&L.lobby, from, src, payload + 2, len - 2);
}

/* --- from the server -------------------------------------------------------- */

/* The same rules bwlive-core applies to playlists, checked again here so a
 * bad server can't slip a command into the host's console. */
static bool ident_ok(const char *s, const char *extra)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_' && !strchr(extra, *s))
			return false;
	return true;
}

static bool setting_allowed(const char *key, const char *value)
{
	if (!ident_ok(key, ""))
		return false;
	for (const char *v = value; *v; v++)
		if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7e || *v == '"' || *v == ';' || *v == '\\')
			return false;
	/* never the console's own secrets or Live's own settings */
	char low[64];
	size_t n = 0;
	for (; key[n] && n + 1 < sizeof(low); n++)
		low[n] = (char)tolower((unsigned char)key[n]);
	low[n] = 0;
	if (!strncmp(low, "live_", 5) || !strncmp(low, "rcon", 4) || strstr(low, "password"))
		return false;
	return true;
}

static void apply_settings(void)
{
	for (int i = 0; i < L.nsettings; i++) {
		const char *k = L.settings[i].key, *v = L.settings[i].value;
		if (!setting_allowed(k, v)) {
			Con_Printf("live: ignored setting %s\n", k);
			continue;
		}
		Con_Printf("live: %s \"%s\"\n", k, v);
		Cvar_Set(k, v);
	}
}

static void on_match(void)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "%u", L.match_host);
	Cvar_Set("live_match_host", buf);
	snprintf(buf, sizeof(buf), "%llu", (unsigned long long)L.match_id);
	Cvar_Set("live_match_id", buf);
	Cvar_Set("live_match_playlist", L.match_playlist);
	Cvar_Set("live_match_mode", L.match_mode);
	Cvar_Set("live_match_map", L.match_map);
	Con_Printf("live: match on %s%s%s%s%s, %d players, host %u%s\n", L.match_playlist,
	           L.match_mode[0] ? ", " : "", L.match_mode, L.match_map[0] ? " on " : "", L.match_map, L.nseats,
	           L.match_host, L.match_host == L.id ? " (you)" : "");
	if (L.match_host == L.id) {
		/* the mode's settings go in before the map loads, so the game's
		 * rules see them from the first frame */
		apply_settings();
		if (live_hostcmd->string[0]) {
			Cbuf_AddText(live_hostcmd->string);
			Cbuf_AddText("\n");
		} else if (L.match_map[0]) {
			snprintf(buf, sizeof(buf), "map %s\n", L.match_map);
			Cbuf_AddText(buf);
		}
	} else if (live_autoconnect->ivalue) {
		/* the client resends its challenge every second until the host's
		 * server is up */
		snprintf(buf, sizeof(buf), "connect live:%u\n", L.match_host);
		Cbuf_AddText(buf);
	}
}

static void from_server(int type, rbuf_t *r)
{
	switch (type) {
	case T_WELCOME: {
		uint32_t id = r_u32(r);
		struct sockaddr_in pub;
		r_addr(r, &pub);
		r_u16(r);
		if (r->bad)
			return;
		bool first = !L.connected || id != L.id;
		if (first)
			Con_Printf("live: connected as %u, public address %s\n", id, addr_str(&pub));
		L.rtt_ms = (int)((now() - L.hello_sent) * 1000);
		L.id = id;
		L.pub = pub;
		L.connected = true;
		L.last_welcome = now();
		update_state();
		if (first && live_onconnect->string[0]) {
			Cbuf_AddText(live_onconnect->string);
			Cbuf_AddText("\n");
		}
		break;
	}
	case T_PING: {
		wbuf_t w;
		uint32_t nonce = r_u32(r);
		if (r->bad)
			return;
		w_start(&w, T_PONG);
		w_u32(&w, nonce);
		send_server(&w);
		break;
	}
	case T_ERROR: {
		char text[256];
		unsigned code = r_u8(r);
		r_str(r, text, sizeof(text));
		Con_Printf("^3live: %s (error %u)\n", text, code);
		break;
	}
	case T_PARTY: {
		char code[16];
		r_str(r, code, sizeof(code));
		L.leader = r_u32(r);
		L.max = (int)r_u8(r);
		L.flags = (int)r_u8(r);
		r_str(r, L.info, sizeof(L.info));
		int n = (int)r_u8(r);
		L.nmembers = 0;
		for (int i = 0; i < n && !r->bad; i++) {
			member_t m;
			memset(&m, 0, sizeof(m));
			m.id = r_u32(r);
			r_str(r, m.name, sizeof(m.name));
			r_addr(r, &m.pub);
			m.nlan = r_addrs(r, m.lan, 4);
			int g = (int)r_u8(r);
			for (int k = 0; k < g; k++) {
				char tmp[64];
				r_str(r, tmp, sizeof(tmp));
				if (m.nguests < MAX_GUESTS)
					Str_Copy(m.guests[m.nguests++], tmp, sizeof(m.guests[0]));
			}
			if (L.nmembers < MAX_MEMBERS)
				L.members[L.nmembers++] = m;
		}
		if (r->bad)
			return;
		if (!L.in_party || strcmp(code, L.code))
			Con_Printf("live: in party %s (%d of %d)\n", code, L.nmembers, L.max);
		Str_Copy(L.code, code, sizeof(L.code));
		L.in_party = true;
		Cvar_SetDirect(live_party, L.code);
		update_state();
		break;
	}
	case T_PARTY_GONE: {
		static const char *why[] = {"left", "kicked", "timed out", "server restart"};
		unsigned reason = r_u8(r);
		if (L.in_party)
			Con_Printf("live: out of party %s (%s)\n", L.code, reason < 4 ? why[reason] : "?");
		L.in_party = false;
		L.queued = false;
		L.code[0] = 0;
		L.nmembers = 0;
		Cvar_SetDirect(live_party, "");
		update_state();
		break;
	}
	case T_INVITED: {
		uint32_t from = r_u32(r);
		char name[64], code[16];
		r_str(r, name, sizeof(name));
		r_str(r, code, sizeof(code));
		if (r->bad)
			return;
		if (L.ninvites == MAX_INVITES)
			memmove(&L.invites[0], &L.invites[1], sizeof(L.invites[0]) * (MAX_INVITES - 1)), L.ninvites--;
		L.invites[L.ninvites].from = from;
		Str_Copy(L.invites[L.ninvites].name, name, sizeof(L.invites[0].name));
		Str_Copy(L.invites[L.ninvites].code, code, sizeof(L.invites[0].code));
		L.ninvites++;
		Con_Printf("live: %s invited you to party %s (live_join %s)\n", name, code, code);
		update_state();
		break;
	}
	case T_INTRODUCE: {
		uint32_t id = r_u32(r);
		char name[64];
		struct sockaddr_in pub, lan[4];
		r_str(r, name, sizeof(name));
		r_addr(r, &pub);
		int nlan = r_addrs(r, lan, 4);
		uint64_t nonce = r_u64(r);
		if (r->bad || !id || id == L.id)
			return;
		peer_t *p = find_peer(id, true);
		Str_Copy(p->name, name, sizeof(p->name));
		p->pub = pub;
		memcpy(p->lan, lan, sizeof(lan));
		p->nlan = nlan;
		p->nonce = nonce;
		if (!p->direct)
			start_punch(p);
		Con_DPrintf("live: introduced to %s (%u) at %s\n", name, id, addr_str(&pub));
		break;
	}
	case T_FORWARD: {
		uint32_t from = r_u32(r);
		if (r->bad)
			return;
		peer_t *p = find_peer(from, true);
		p->last_rx = p->direct ? p->last_rx : now();
		deliver(from, r->b + r->at, r->n - r->at);
		break;
	}
	case T_QUEUE_STATUS: {
		char pl[64];
		r_str(r, pl, sizeof(pl));
		unsigned stage = r_u8(r);
		unsigned waited = r_u16(r), searching = r_u16(r);
		if (r->bad)
			return;
		bool was = L.queued;
		L.queued = stage != 255;
		Str_Copy(L.playlist, pl, sizeof(L.playlist));
		L.stage = (int)stage;
		L.waited = (int)waited;
		L.searching = (int)searching;
		if (L.queued && !was)
			Con_Printf("live: searching %s\n", pl);
		else if (!L.queued && was)
			Con_Printf("live: search stopped\n");
		update_state();
		break;
	}
	case T_MATCH: {
		uint64_t id = r_u64(r);
		char pl[64];
		r_str(r, pl, sizeof(pl));
		uint32_t host = r_u32(r);
		int n = (int)r_u8(r);
		L.nseats = 0;
		for (int i = 0; i < n && !r->bad; i++) {
			uint32_t sid = r_u32(r);
			int slot = (int)r_u8(r), team = (int)r_u8(r);
			char name[64];
			r_str(r, name, sizeof(name));
			if (L.nseats < MAX_SEATS) {
				L.seats[L.nseats].id = sid;
				L.seats[L.nseats].slot = slot;
				L.seats[L.nseats].team = team;
				Str_Copy(L.seats[L.nseats].name, name, sizeof(L.seats[0].name));
				L.nseats++;
			}
		}
		char mode[sizeof(L.match_mode)], map[sizeof(L.match_map)];
		r_str(r, mode, sizeof(mode));
		r_str(r, map, sizeof(map));
		int ns = (int)r_u8(r);
		int kept = 0;
		for (int i = 0; i < ns && !r->bad; i++) {
			char k[sizeof(L.settings[0].key)], v[sizeof(L.settings[0].value)];
			r_str(r, k, sizeof(k));
			r_str(r, v, sizeof(v));
			if (kept < MAX_SETTINGS && id != L.match_id) {
				Str_Copy(L.settings[kept].key, k, sizeof(k));
				Str_Copy(L.settings[kept].value, v, sizeof(v));
				kept++;
			}
		}
		if (r->bad || id == L.match_id)
			return;
		if (map[0] && (!ident_ok(map, "-./") || strstr(map, "..") || map[0] == '/' || map[0] == '.'))
			map[0] = 0;
		L.nsettings = kept;
		Str_Copy(L.match_mode, mode, sizeof(L.match_mode));
		Str_Copy(L.match_map, map, sizeof(L.match_map));
		L.match_id = id;
		Str_Copy(L.match_playlist, pl, sizeof(L.match_playlist));
		L.match_host = host;
		L.queued = false;
		update_state();
		on_match();
		break;
	}
	case T_PLAYERS: {
		int n = (int)r_u8(r);
		Con_Printf("%d other player%s online:\n", n, n == 1 ? "" : "s");
		for (int i = 0; i < n && !r->bad; i++) {
			char name[64], status[80], code[16];
			uint32_t id = r_u32(r);
			r_str(r, name, sizeof(name));
			r_str(r, status, sizeof(status));
			r_str(r, code, sizeof(code));
			Con_Printf("  %10u  %-24s %s%s%s\n", id, name, status, code[0] ? "  party " : "", code);
		}
		break;
	}
	}
}

/* --- from peers ------------------------------------------------------------- */

static bool known_addr(const peer_t *p, const struct sockaddr_in *a)
{
	if (p->direct && same_addr(&p->path, a))
		return true;
	if (!p->nonce)
		return false; /* never introduced: relay only */
	if (same_addr(&p->pub, a))
		return true;
	for (int i = 0; i < p->nlan; i++)
		if (same_addr(&p->lan[i], a))
			return true;
	return false;
}

static void from_peer(int type, rbuf_t *r, const struct sockaddr_in *from)
{
	if (type == T_PUNCH || type == T_PUNCH_ACK) {
		uint32_t src = r_u32(r), dst = r_u32(r);
		uint64_t nonce = r_u64(r);
		peer_t *p = find_peer(src, false);
		if (r->bad || dst != L.id || !p || !p->nonce || nonce != p->nonce)
			return;
		if (type == T_PUNCH)
			send_punch(p, T_PUNCH_ACK, from);
		if (!p->direct || !same_addr(&p->path, from)) {
			if (!p->direct)
				Con_DPrintf("live: direct path to %s (%u) via %s\n", p->name, p->id, addr_str(from));
			p->direct = true;
			p->path = *from;
			update_state();
		}
		p->last_rx = now();
		return;
	}
	if (type == T_DATA) {
		uint32_t src = r_u32(r);
		peer_t *p = find_peer(src, false);
		/* only from an address we know that peer by: the punched path, or
		 * one of the endpoints the server introduced (the two sides can pick
		 * different ones, say the LAN address one way and loopback the other) */
		if (r->bad || !p || !known_addr(p, from))
			return;
		p->last_rx = now();
		deliver(src, r->b + r->at, r->n - r->at);
	}
}

/* --- frame ------------------------------------------------------------------ */

void Live_Frame(void)
{
	if (L.sock < 0)
		return;
	uint8_t buf[MAX_DGRAM + 64];
	for (int guard = 0; guard < 512; guard++) {
		struct sockaddr_in from;
		socklen_t len = sizeof(from);
		ssize_t n = recvfrom(L.sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
		if (n <= 0)
			break;
		if (n < 6 || memcmp(buf, "BWLV", 4) || buf[4] != 1 || from.sin_family != AF_INET)
			continue;
		rbuf_t r = {buf + 6, (size_t)n - 6, 0, false};
		if (L.have_server && same_addr(&from, &L.server))
			from_server(buf[5], &r);
		else if (L.connected)
			from_peer(buf[5], &r, &from);
	}
	double t = now();
	if (L.have_server && t - L.last_hello >= 5.0)
		send_hello();
	if (L.connected && t - L.last_welcome > 20.0) {
		Con_Printf("^3live: lost contact with the server; still trying\n");
		L.connected = false;
		update_state();
	}
	if (L.connected)
		punch_frame(t);
}

/* --- game and lobby streams ------------------------------------------------- */

void Live_SendGame(netsrc_t src, const void *data, size_t len, const netadr_t *to)
{
	uint32_t id = (uint32_t)to->ip[0] << 24 | (uint32_t)to->ip[1] << 16 | (uint32_t)to->ip[2] << 8 | to->ip[3];
	uint8_t me = src == NS_SERVER ? LIVE_VPORT_SERVER : LIVE_VPORT_CLIENT;
	uint8_t dst = to->port ? (uint8_t)to->port : (src == NS_SERVER ? LIVE_VPORT_CLIENT : LIVE_VPORT_SERVER);
	send_peer(id, me, dst, data, len);
}

size_t Live_GetGame(netsrc_t src, void *buf, size_t bufsize, netadr_t *from)
{
	uint32_t id;
	uint8_t port;
	size_t n = pop(&L.game[src], buf, bufsize, &id, &port);
	if (!n)
		return 0;
	memset(from, 0, sizeof(*from));
	from->type = NA_LIVE;
	from->ip[0] = (uint8_t)(id >> 24);
	from->ip[1] = (uint8_t)(id >> 16);
	from->ip[2] = (uint8_t)(id >> 8);
	from->ip[3] = (uint8_t)id;
	from->port = port;
	return n;
}

bool Live_SendLobby(uint32_t peer, const void *data, size_t len)
{
	return send_peer(peer, LIVE_VPORT_LOBBY, LIVE_VPORT_LOBBY, data, len);
}

size_t Live_GetLobby(void *buf, size_t bufsize, uint32_t *from)
{
	uint8_t port;
	return pop(&L.lobby, buf, bufsize, from, &port);
}

bool Live_Connected(void) { return L.connected; }
uint32_t Live_MyId(void) { return L.connected ? L.id : 0; }
const char *Live_PartyCode(void) { return L.in_party ? L.code : ""; }
uint32_t Live_PartyLeader(void) { return L.in_party ? L.leader : 0; }

/* --- commands --------------------------------------------------------------- */

static bool need(void)
{
	if (L.connected)
		return true;
	Con_Printf("not connected to Bulwark Live (live_connect)\n");
	return false;
}

static void simple(int type)
{
	wbuf_t w;
	w_start(&w, type);
	send_server(&w);
}

static void Connect_f(int argc, const char **argv)
{
	const char *server = argc > 1 ? argv[1] : live_server->string;
	netadr_t a;
	if (!server[0]) {
		Con_Printf("usage: live_connect <host[:port]> (or set live_server)\n");
		return;
	}
	if (!NET_StringToAdr(server, &a, 27950) || a.type != NA_IP4) {
		Con_Printf("live: can't resolve %s to an IPv4 address\n", server);
		return;
	}
	if (!open_socket())
		return;
	memset(&L.server, 0, sizeof(L.server));
	L.server.sin_family = AF_INET;
	memcpy(&L.server.sin_addr, a.ip, 4);
	L.server.sin_port = htons(a.port);
	L.have_server = true;
	if (!L.token)
		L.token = load_token();
	L.last_welcome = now();
	if (argc > 1)
		Cvar_Set("live_server", server);
	Con_Printf("live: contacting %s\n", server);
	send_hello();
}

static void Disconnect_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (L.have_server)
		simple(T_BYE);
	if (L.sock >= 0)
		close(L.sock);
	uint64_t token = L.token;
	memset(&L, 0, sizeof(L));
	L.sock = -1;
	L.token = token;
	Cvar_SetDirect(live_party, "");
	update_state();
}

static void Party_f(int argc, const char **argv)
{
	wbuf_t w;
	if (!need())
		return;
	w_start(&w, T_PARTY_CREATE);
	w_u8(&w, (unsigned)(argc > 1 ? atoi(argv[1]) : 4));
	w_u8(&w, (unsigned)(argc > 2 ? atoi(argv[2]) : 1));
	w_str(&w, argc > 3 ? argv[3] : "");
	send_server(&w);
}

static void Join_f(int argc, const char **argv)
{
	wbuf_t w;
	if (argc < 2) {
		Con_Printf("usage: live_join <room code>\n");
		return;
	}
	if (!need())
		return;
	w_start(&w, T_PARTY_JOIN);
	w_str(&w, argv[1]);
	send_server(&w);
}

static void Leave_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (need())
		simple(T_PARTY_LEAVE);
}

static void Open_f(int argc, const char **argv)
{
	wbuf_t w;
	if (argc < 2) {
		Con_Printf("usage: live_open <0|1> [info]  (1: anyone with the code may join)\n");
		return;
	}
	if (!need())
		return;
	w_start(&w, T_PARTY_SET);
	w_u8(&w, (unsigned)atoi(argv[1]));
	w_str(&w, argc > 2 ? argv[2] : L.info);
	send_server(&w);
}

static void Invite_f(int argc, const char **argv)
{
	wbuf_t w;
	if (argc < 2) {
		Con_Printf("usage: live_invite <player id> (see live_list)\n");
		return;
	}
	if (!need())
		return;
	w_start(&w, T_INVITE);
	w_u32(&w, (uint32_t)strtoul(argv[1], NULL, 10));
	send_server(&w);
}

static void List_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (need())
		simple(T_LIST);
}

static void Queue_f(int argc, const char **argv)
{
	wbuf_t w;
	if (argc < 2) {
		Con_Printf("usage: live_queue <playlist> [seniority]\n");
		return;
	}
	if (!need())
		return;
	w_start(&w, T_QUEUE);
	w_str(&w, argv[1]);
	w_u32(&w, (uint32_t)(argc > 2 ? strtoul(argv[2], NULL, 10) : 1));
	send_server(&w);
}

static void Cancel_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (need())
		simple(T_QUEUE_CANCEL);
}

static void SetStatus_f(int argc, const char **argv)
{
	wbuf_t w;
	if (!need())
		return;
	w_start(&w, T_STATUS);
	w_str(&w, argc > 1 ? argv[1] : "");
	send_server(&w);
}

/* the console's split-screen guests; the next hello tells the server */
static void Guests_f(int argc, const char **argv)
{
	if (argc == 1) {
		Con_Printf("%d guest%s", L.nguests, L.nguests == 1 ? "" : "s");
		for (int i = 0; i < L.nguests; i++)
			Con_Printf("%s %s", i ? "," : ":", L.guests[i]);
		Con_Printf("\nusage: live_guests <name>... | live_guests -  (up to %d; - for none)\n", MAX_GUESTS);
		return;
	}
	L.nguests = 0;
	for (int i = 1; i < argc && L.nguests < MAX_GUESTS; i++) {
		if (!strcmp(argv[i], "-"))
			continue;
		Str_Copy(L.guests[L.nguests++], argv[i], sizeof(L.guests[0]));
	}
	if (L.have_server)
		send_hello();
}

static void Status_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	update_state();
	if (!L.have_server) {
		Con_Printf("not connected to Bulwark Live\n");
		return;
	}
	Con_Printf("live: %s, id %u, public %s, %d ms\n", L.connected ? "connected" : "connecting", L.id,
	           addr_str(&L.pub), L.rtt_ms);
	if (L.in_party) {
		Con_Printf("party %s, %d of %d, %s\n", L.code, L.nmembers, L.max, L.flags & 1 ? "open" : "invite only");
		for (int i = 0; i < L.nmembers; i++) {
			member_t *m = &L.members[i];
			peer_t *p = find_peer(m->id, false);
			Con_Printf("  %10u %-24s %s%s", m->id, m->name, m->id == L.leader ? "leader " : "",
			           m->id == L.id ? "(you)" : p && p->direct ? "direct" : "relayed");
			for (int g = 0; g < m->nguests; g++)
				Con_Printf(" +%s", m->guests[g]);
			Con_Printf("\n");
		}
	}
	if (L.queued)
		Con_Printf("searching %s: stage %d, %d s, %d searching\n", L.playlist, L.stage, L.waited, L.searching);
}

static void Peers_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	double t = now();
	for (int i = 0; i < MAX_PEERS; i++) {
		peer_t *p = &L.peers[i];
		if (!p->used)
			continue;
		Con_Printf("  %10u %-24s %s %s\n", p->id, p->name, p->direct ? "direct" : "relay",
		           p->direct ? addr_str(&p->path) : (t < p->punch_until ? "(punching)" : ""));
	}
}

void Live_Init(void)
{
	live_server = Cvar_Get("live_server", "", CVAR_ARCHIVE, "Bulwark Live server, host[:port] (port 27950)");
	Cvar_Get("live_port", "0", CVAR_ARCHIVE, "Local UDP port for Bulwark Live, 0 for any");
	live_game = Cvar_Get("live_game", "bulwark", 0, "Game name sent to Bulwark Live; players only meet the same game");
	live_autoconnect = Cvar_Get("live_autoconnect", "1", CVAR_ARCHIVE, "Join a match's host when Bulwark Live makes one");
	live_hostcmd = Cvar_Get("live_hostcmd", "", 0, "Commands run when Bulwark Live picks this game to host a match");
	live_onconnect = Cvar_Get("live_onconnect", "", 0, "Commands run each time Bulwark Live (re)connects, e.g. \"live_join ABCDEF\"");
	live_state = Cvar_Get("live_state", "", CVAR_READONLY | CVAR_NOSAVE, "Bulwark Live state as JSON");
	live_punch = Cvar_Get("live_punch", "1", CVAR_ARCHIVE, "Try direct paths to other players; 0 sends everything through the relay");
	live_party = Cvar_Get("live_party", "", CVAR_READONLY | CVAR_NOSAVE, "The Bulwark Live party's room code");
	Cvar_Get("live_match_host", "", CVAR_NOSAVE, "Player id hosting the last Bulwark Live match");
	Cvar_Get("live_match_id", "", CVAR_NOSAVE, "The last Bulwark Live match");
	Cvar_Get("live_match_playlist", "", CVAR_NOSAVE, "The last Bulwark Live match's playlist");
	Cvar_Get("live_match_mode", "", CVAR_NOSAVE, "The last Bulwark Live match's game mode, from its playlist");
	Cvar_Get("live_match_map", "", CVAR_NOSAVE, "The last Bulwark Live match's map; the host loads it when live_hostcmd is empty");
	Cmd_AddD("live_connect", Connect_f, "Connect to a Bulwark Live server");
	Cmd_AddD("live_disconnect", Disconnect_f, "Leave Bulwark Live");
	Cmd_AddD("live_party", Party_f, "Create a party: live_party [max] [open 0/1] [info]");
	Cmd_AddD("live_join", Join_f, "Join a party by room code");
	Cmd_AddD("live_leave", Leave_f, "Leave the party");
	Cmd_AddD("live_open", Open_f, "Leader: live_open <0|1> [info] sets whether the code alone lets people join");
	Cmd_AddD("live_invite", Invite_f, "Invite a player (by id from live_list) to the party");
	Cmd_AddD("live_list", List_f, "List players of this game on Bulwark Live");
	Cmd_AddD("live_queue", Queue_f, "Leader: search a playlist, live_queue <playlist> [seniority]");
	Cmd_AddD("live_cancel", Cancel_f, "Stop searching");
	Cmd_AddD("live_setstatus", SetStatus_f, "Set the status line friends see");
	Cmd_AddD("live_guests", Guests_f, "Name the split-screen guests on this console");
	Cmd_AddD("live_status", Status_f, "Show Bulwark Live state");
	Cmd_AddD("live_peers", Peers_f, "Show paths to other players (direct or relayed)");
	update_state();
}

void Live_Shutdown(void)
{
	if (L.have_server)
		simple(T_BYE);
	if (L.sock >= 0)
		close(L.sock);
	L.sock = -1;
	L.have_server = L.connected = false;
}
