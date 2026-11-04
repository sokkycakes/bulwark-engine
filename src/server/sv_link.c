/* Bulwark engine - link players: handhelds (the Nintendo DS first) that join
 * as companion players over a small UDP protocol. ISC licence, see LICENSE.
 *
 * docs/spec/link.md has the protocol. A link player holds a normal client
 * slot; the engine treats it like a bot for networking (no snapshots, the
 * last command repeats every frame) while the game sees a real player. Its
 * input arrives here, and what it sees goes back from here: a private
 * screen the game draws with the link_* builtins, and a view of where it is.
 */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../net/net_compat.h"

#define F sv.vm->fofs

#define LINK_MAGIC "BWL1"
#define MAX_LINKS 16
#define MAX_SCREEN 1200
#define MAX_MARKERS 64
#define MAX_EDICTS_LINK 2048
#define FL_MONSTER 32
#define SCREEN_RESEND 0.25
#define VIEW_INTERVAL 0.05
#define LOAD_GRACE 60.0 /* seconds a device may take to load a map it was sent to */
#define ENTS_MAXMODELS 1024 /* brush models the presence bits cover */
#define ENTS_MAXTHINGS 48
#define ENTS_RANGE 2048.0f

enum { L_DISCOVER = 1, L_JOIN, L_INPUT, L_LEAVE };
enum { L_INFO = 0x81, L_WELCOME, L_REFUSE, L_SCREEN, L_VIEW, L_KICK, L_ENTS };
enum { SC_CLEAR = 1, SC_FILL, SC_TEXT };

typedef struct {
	int16_t org[3];
	uint8_t kind;
	uint16_t rgb;
} marker_t;

typedef struct {
	bool used;
	struct sockaddr_in from;
	uint32_t session;
	client_t *client;
	double lastrecv;
	uint16_t lastseq;
	uint16_t keys;
	uint8_t touch, tx, ty;
	int spawncount;

	uint8_t draw[MAX_SCREEN]; /* being drawn by the game */
	int drawlen;
	uint8_t screen[MAX_SCREEN]; /* last presented */
	int screenlen;
	uint16_t version, acked;
	double nextscreen;
	bool gamedrew;   /* the game has presented a screen; no default one */
	double nextdefault;

	marker_t markers[MAX_MARKERS];
	int nummarkers;
	double nextview;
	uint16_t viewseq, entsseq;
	int fps, minfps, peakpolys, loadms; /* what the device last said about itself; -1 unknown */
	double loading_until; /* a new map was announced: the device may be quiet while it loads it */
} link_t;

static link_t links[MAX_LINKS];
static int sock = -1;
static int boundport;
static cvar_t *sv_link_port, *sv_link_max, *sv_link_timeout;

/* --- packets ---------------------------------------------------------------------------- */

typedef struct {
	uint8_t data[1400];
	int len;
} pkt_t;

static void p_begin(pkt_t *p, int type)
{
	memcpy(p->data, LINK_MAGIC, 4);
	p->data[4] = (uint8_t)type;
	p->len = 5;
}
static void p_u8(pkt_t *p, unsigned v)
{
	if (p->len < (int)sizeof(p->data))
		p->data[p->len++] = (uint8_t)v;
}
static void p_u16(pkt_t *p, unsigned v) { p_u8(p, v & 0xff), p_u8(p, (v >> 8) & 0xff); }
static void p_u32(pkt_t *p, uint32_t v) { p_u16(p, v & 0xffff), p_u16(p, v >> 16); }
static void p_str(pkt_t *p, const char *s, int size)
{
	for (int i = 0; i < size; i++)
		p_u8(p, *s ? (uint8_t)*s++ : 0);
}
static void p_bytes(pkt_t *p, const void *d, int n)
{
	if (p->len + n <= (int)sizeof(p->data)) {
		memcpy(p->data + p->len, d, (size_t)n);
		p->len += n;
	}
}

static void send_to(const struct sockaddr_in *to, const pkt_t *p)
{
	if (sock >= 0)
		sendto(sock, (const char *)p->data, (size_t)p->len, MSG_NOSIGNAL, (const struct sockaddr *)to, sizeof(*to));
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static uint16_t rgb15(const float *c)
{
	int r = (int)(BW_CLAMP(c[0], 0, 1) * 31 + 0.5f), g = (int)(BW_CLAMP(c[1], 0, 1) * 31 + 0.5f);
	int b = (int)(BW_CLAMP(c[2], 0, 1) * 31 + 0.5f);
	return (uint16_t)(r | g << 5 | b << 10);
}

/* --- the socket ------------------------------------------------------------------------- */

static void close_socket(void)
{
	if (sock >= 0)
		close(sock);
	sock = -1;
	boundport = 0;
}

static void open_socket(void)
{
	struct sockaddr_in a;
	int port = sv_link_port->ivalue, one = 1;
	if (port == boundport && (sock >= 0 || port <= 0))
		return;
	close_socket();
	boundport = port;
	if (port <= 0)
		return;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
		Con_Printf("link: can't use UDP port %d\n", port);
		close(sock);
		sock = -1;
		return;
	}
	Con_DPrintf("link: listening on UDP port %d\n", port);
}

/* --- link players ----------------------------------------------------------------------- */

static link_t *find_session(uint32_t session, const struct sockaddr_in *from)
{
	for (int i = 0; i < MAX_LINKS; i++)
		if (links[i].used && links[i].session == session && links[i].from.sin_addr.s_addr == from->sin_addr.s_addr)
			return &links[i];
	return NULL;
}

static link_t *link_for_edict(int e)
{
	client_t *c = SV_ClientForEdict(e);
	return c && c->link > 0 && c->link <= MAX_LINKS && links[c->link - 1].used ? &links[c->link - 1] : NULL;
}

static int count_links(void)
{
	int n = 0;
	for (int i = 0; i < MAX_LINKS; i++)
		n += links[i].used;
	return n;
}

static void send_welcome(link_t *l)
{
	pkt_t p;
	p_begin(&p, L_WELCOME);
	p_u32(&p, l->session);
	p_u8(&p, (unsigned)(l->client - svs.clients));
	p_u16(&p, (unsigned)svs.spawncount);
	p_str(&p, sv.mapname, 64);
	send_to(&l->from, &p);
	if (l->spawncount != svs.spawncount)
		l->loading_until = Sys_Seconds() + LOAD_GRACE;
	l->spawncount = svs.spawncount;
}

static void refuse(const struct sockaddr_in *to, const char *reason)
{
	pkt_t p;
	p_begin(&p, L_REFUSE);
	p_str(&p, reason, 64);
	send_to(to, &p);
}

static void handle_join(const struct sockaddr_in *from, const uint8_t *d, int len)
{
	uint32_t session;
	char name[17];
	link_t *l = NULL;
	client_t *c;
	if (len < 4 + 2 + 16)
		return;
	session = rd32(d);
	if ((l = find_session(session, from))) {
		send_welcome(l); /* the welcome was lost */
		return;
	}
	if (count_links() >= BW_CLAMP(sv_link_max->ivalue, 0, MAX_LINKS)) {
		refuse(from, "no free link slots");
		return;
	}
	for (int i = 0; i < MAX_LINKS && !l; i++)
		if (!links[i].used)
			l = &links[i];
	memcpy(name, d + 6, 16);
	name[16] = 0;
	for (char *s = name; *s; s++)
		if ((unsigned char)*s < 32 || *s == '"' || *s == '\\')
			*s = '_';
	if (!name[0])
		strcpy(name, "DS");
	memset(l, 0, sizeof(*l));
	l->used = true;
	l->from = *from;
	l->session = session;
	l->lastrecv = Sys_Seconds();
	c = SV_SpawnLinkClient(name, (int)(l - links) + 1);
	if (!c) {
		l->used = false;
		refuse(from, "the server is full");
		return;
	}
	l->client = c;
	l->version = 1; /* nothing acknowledged yet */
	l->fps = l->minfps = l->peakpolys = l->loadms = -1;
	send_welcome(l);
	Con_Printf("link: %s joined from %s\n", name, inet_ntoa(from->sin_addr));
}

static void handle_input(link_t *l, const uint8_t *d, int len)
{
	client_t *c = l->client;
	usercmd_t *u;
	int16_t pitch, yaw;
	uint16_t seq;
	if (len < 4 + 2 + 2 + 3 + 3 + 4 + 2 || !c || c->state != CS_SPAWNED)
		return;
	seq = rd16(d + 4);
	if ((int16_t)(seq - l->lastseq) <= 0 && l->lastseq)
		return; /* old or repeated */
	l->lastseq = seq;
	l->keys = rd16(d + 6);
	l->touch = d[8], l->tx = d[9], l->ty = d[10];
	pitch = (int16_t)rd16(d + 14);
	yaw = (int16_t)rd16(d + 16);
	l->acked = rd16(d + 18);
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 4) /* the device's own frame rate, last second */
		l->fps = d[20], l->minfps = d[21], l->peakpolys = rd16(d + 22);
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 6)
		l->loadms = rd16(d + 24) * 10;

	u = &c->lastucmd;
	u->angles[0] = -pitch * (360.0f / 65536.0f); /* the device's pitch is up-positive */
	u->angles[1] = yaw * (360.0f / 65536.0f);
	u->angles[2] = 0;
	u->movevalues[0] = (int8_t)d[11] * (400.0f / 127.0f);
	u->movevalues[1] = (int8_t)d[12] * (400.0f / 127.0f);
	u->movevalues[2] = (int8_t)d[13] * (400.0f / 127.0f);
	/* DS keys (libnds): A 1, B 2, R 0x100, X 0x400, Y 0x800 */
	u->buttons = (l->keys & 0x001 ? 1 : 0) | (l->keys & 0x002 ? 2 : 0) | (l->keys & 0x400 ? 16 : 0) |
	             (l->keys & 0x800 ? 8 : 0) | (l->keys & 0x100 ? 32 : 0);
	/* the game reads the view from v_angle as well */
	if (F.v_angle >= 0) {
		float *va = &QC_EdictFields(sv.vm, c->edict)[F.v_angle].f;
		va[0] = u->angles[0], va[1] = u->angles[1], va[2] = 0;
	}
}

static void handle_discover(const struct sockaddr_in *from)
{
	pkt_t p;
	int players = 0;
	for (int i = 0; i < svs.maxclients; i++)
		players += svs.clients[i].state != CS_FREE;
	p_begin(&p, L_INFO);
	p_str(&p, Cvar_String("hostname"), 32);
	p_str(&p, sv.mapname, 32);
	p_u8(&p, (unsigned)players);
	p_u8(&p, (unsigned)svs.maxclients);
	p_u8(&p, (unsigned)BW_MAX(0, BW_CLAMP(sv_link_max->ivalue, 0, MAX_LINKS) - count_links()));
	send_to(from, &p);
}

void SV_LinkReadPackets(void)
{
	uint8_t buf[1500];
	open_socket();
	if (sock < 0)
		return;
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
		link_t *l;
		if (n < 0)
			break;
		if (n < 5 || memcmp(buf, LINK_MAGIC, 4))
			continue;
		switch (buf[4]) {
		case L_DISCOVER: handle_discover(&from); break;
		case L_JOIN: handle_join(&from, buf + 5, n - 5); break;
		case L_INPUT:
		case L_LEAVE:
			if (n < 9 || !(l = find_session(rd32(buf + 5), &from)))
				break;
			l->lastrecv = Sys_Seconds();
			l->from.sin_port = from.sin_port; /* NAT may move it */
			if (buf[4] == L_INPUT)
				handle_input(l, buf + 5, n - 5);
			else if (l->client)
				SV_DropClient(l->client, "left");
			break;
		}
	}
}

/* --- screens and views ------------------------------------------------------------------ */

static void draw_u8(link_t *l, unsigned v)
{
	if (l->drawlen < MAX_SCREEN)
		l->draw[l->drawlen++] = (uint8_t)v;
}

static void draw_clear(link_t *l, uint16_t rgb)
{
	l->drawlen = 0;
	draw_u8(l, SC_CLEAR), draw_u8(l, rgb & 0xff), draw_u8(l, rgb >> 8);
}

static void draw_fill(link_t *l, int x, int y, int w, int h, uint16_t rgb)
{
	if (l->drawlen + 7 > MAX_SCREEN)
		return;
	draw_u8(l, SC_FILL), draw_u8(l, (unsigned)BW_CLAMP(x, 0, 255)), draw_u8(l, (unsigned)BW_CLAMP(y, 0, 191));
	draw_u8(l, (unsigned)BW_CLAMP(w, 0, 256) & 0xff), draw_u8(l, (unsigned)BW_CLAMP(h, 0, 192));
	draw_u8(l, rgb & 0xff), draw_u8(l, rgb >> 8);
}

static void draw_text(link_t *l, int x, int y, const char *s, uint16_t rgb)
{
	int n = (int)strnlen(s, 32);
	if (l->drawlen + 6 + n > MAX_SCREEN)
		return;
	draw_u8(l, SC_TEXT), draw_u8(l, (unsigned)BW_CLAMP(x, 0, 255)), draw_u8(l, (unsigned)BW_CLAMP(y, 0, 191));
	draw_u8(l, rgb & 0xff), draw_u8(l, rgb >> 8), draw_u8(l, (unsigned)n);
	for (int i = 0; i < n; i++)
		draw_u8(l, (unsigned char)s[i]);
}

static void present(link_t *l)
{
	if (l->drawlen == l->screenlen && !memcmp(l->draw, l->screen, (size_t)l->drawlen))
		return;
	memcpy(l->screen, l->draw, (size_t)l->drawlen);
	l->screenlen = l->drawlen;
	l->version++;
	if (!l->version)
		l->version = 1;
	l->nextscreen = 0;
}

/* what a link player sees until the game draws its own screen */
static void default_screen(link_t *l)
{
	char line[40];
	int y = 4, e = l->client->edict;
	draw_clear(l, 0x1084);
	draw_fill(l, 0, 0, 0, 14, 0x4d26);
	draw_text(l, 4, 3, l->client->name, 0x7fff);
	snprintf(line, sizeof(line), "map %.32s", sv.mapname);
	draw_text(l, 4, y += 16, line, 0x6318);
	snprintf(line, sizeof(line), "health %d", (int)QC_EFloat(sv.vm, e, F.health));
	draw_text(l, 4, y += 12, line, 0x03ff);
	draw_text(l, 4, y += 18, "players", 0x6318);
	for (int i = 0; i < svs.maxclients && y < 176; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		snprintf(line, sizeof(line), "%.24s%s", c->name, c->link ? " (DS)" : c->bot ? " (bot)" : "");
		draw_text(l, 12, y += 10, line, c == l->client ? 0x7fff : 0x5294);
	}
	present(l);
}

static void send_screen(link_t *l)
{
	pkt_t p;
	p_begin(&p, L_SCREEN);
	p_u32(&p, l->session);
	p_u16(&p, l->version);
	p_bytes(&p, l->screen, l->screenlen);
	send_to(&l->from, &p);
}

static void send_view(link_t *l)
{
	pkt_t p;
	int e = l->client->edict, countpos;
	const float *org = F.origin >= 0 ? &QC_EdictFields(sv.vm, e)[F.origin].f : NULL;
	const float *ofs = F.view_ofs >= 0 ? &QC_EdictFields(sv.vm, e)[F.view_ofs].f : NULL;
	float *ang = l->client->lastucmd.angles;
	int count = 0, flags = 0;
	if (!org)
		return;
	/* the game turned the player (a teleporter, a spawn): the device adopts the angles */
	if (F.fixangle >= 0 && F.angles >= 0 && QC_EFloat(sv.vm, e, F.fixangle) != 0) {
		const float *a = &QC_EdictFields(sv.vm, e)[F.angles].f;
		ang[0] = 0, ang[1] = a[1], ang[2] = 0;
		if (F.v_angle >= 0) {
			float *va = &QC_EdictFields(sv.vm, e)[F.v_angle].f;
			va[0] = 0, va[1] = a[1], va[2] = 0;
		}
		QC_EdictFields(sv.vm, e)[F.fixangle].f = 0;
		flags |= 1;
	}
	p_begin(&p, L_VIEW);
	p_u32(&p, l->session);
	p_u16(&p, ++l->viewseq);
	for (int c = 0; c < 3; c++)
		p_u32(&p, (uint32_t)(int32_t)((org[c] + (ofs ? ofs[c] : 0)) * 8));
	p_u16(&p, (uint16_t)(int16_t)(-ang[0] * 65536 / 360));
	p_u16(&p, (uint16_t)(int16_t)(ang[1] * 65536 / 360));
	p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(QC_EFloat(sv.vm, e, F.health), -32768, 32767));
	p_u8(&p, (uint8_t)flags);
	countpos = p.len;
	p_u8(&p, 0);
	/* the other players, then what the game marked */
	for (int i = 0; i < svs.maxclients && count < 255 && p.len + 9 <= (int)sizeof(p.data); i++) {
		client_t *c = &svs.clients[i];
		const float *o;
		if (c->state != CS_SPAWNED || c == l->client)
			continue;
		o = &QC_EdictFields(sv.vm, c->edict)[F.origin].f;
		for (int k = 0; k < 3; k++)
			p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(o[k], -32768, 32767));
		p_u8(&p, 0);
		p_u16(&p, c->link ? 0x03e0 : 0x7fff);
		count++;
	}
	for (int i = 0; i < l->nummarkers && count < 255 && p.len + 9 <= (int)sizeof(p.data); i++) {
		const marker_t *m = &l->markers[i];
		for (int k = 0; k < 3; k++)
			p_u16(&p, (uint16_t)m->org[k]);
		p_u8(&p, m->kind);
		p_u16(&p, m->rgb);
		count++;
	}
	l->nummarkers = 0;
	p.data[countpos] = (uint8_t)count;
	send_to(&l->from, &p);
}

/* the entities a link player draws itself (docs/spec/link.md, ENTS): which
 * of the map's brush models are there now and where the moved ones are
 * (doors, lifts, trains), then the nearest other things as boxes. Only what
 * the game networks counts: triggers and other unsent entities are left out. */
static int ent_submodel(int e)
{
	int mi = (int)QC_EFloat(sv.vm, e, F.modelindex);
	const char *name = mi > 0 && mi < SV_MAX_MODELS ? sv.models[mi] : NULL;
	return name && name[0] == '*' ? atoi(name + 1) : -1;
}

static bool ent_shown(int e)
{
	if (sv.vm->edicts[e].free || QC_EFloat(sv.vm, e, F.modelindex) <= 0)
		return false;
	if (F.sendentity >= 0 && !QC_EInt(sv.vm, e, F.sendentity))
		return false;
	return QC_EFloat(sv.vm, e, F.solid) != SOLID_TRIGGER;
}

typedef struct {
	int e;
	float d2;
} thing_t;

static int cmp_thing(const void *a, const void *b)
{
	float x = ((const thing_t *)a)->d2, y = ((const thing_t *)b)->d2;
	return x < y ? -1 : x > y;
}

static void send_ents(link_t *l)
{
	static uint8_t present[ENTS_MAXMODELS / 8];
	static thing_t things[MAX_EDICTS_LINK];
	pkt_t p;
	int self = l->client->edict, nummodels = 0, nthings = 0, countpos, count = 0;
	const float *me = &QC_EdictFields(sv.vm, self)[F.origin].f;
	if (F.origin < 0 || F.modelindex < 0 || !sv.world)
		return;
	memset(present, 0, sizeof(present));
	for (int i = 1; i < sv.nummodels && i < SV_MAX_MODELS; i++)
		if (sv.models[i] && sv.models[i][0] == '*' && atoi(sv.models[i] + 1) + 1 > nummodels)
			nummodels = atoi(sv.models[i] + 1) + 1;
	nummodels = BW_MIN(nummodels, ENTS_MAXMODELS);
	p_begin(&p, L_ENTS);
	p_u32(&p, l->session);
	p_u16(&p, ++l->entsseq);
	p_u16(&p, (unsigned)nummodels);
	for (int e = 1; e < sv.vm->num_edicts; e++) {
		int sm;
		if (e == self || !ent_shown(e))
			continue;
		sm = ent_submodel(e);
		if (sm > 0 && sm < nummodels)
			present[sm >> 3] |= (uint8_t)(1 << (sm & 7));
		else if (sm < 0 && nthings < MAX_EDICTS_LINK && !((int)QC_EFloat(sv.vm, e, F.flags) & FL_CLIENT)) {
			const float *o = &QC_EdictFields(sv.vm, e)[F.origin].f;
			float d2 = 0;
			for (int c = 0; c < 3; c++)
				d2 += (o[c] - me[c]) * (o[c] - me[c]);
			if (d2 < ENTS_RANGE * ENTS_RANGE)
				things[nthings].e = e, things[nthings++].d2 = d2;
		}
	}
	p_bytes(&p, present, (nummodels + 7) / 8);
	/* the moved brush models: where their origin is now, and their angles */
	countpos = p.len;
	p_u8(&p, 0);
	for (int e = 1; e < sv.vm->num_edicts && count < 255 && p.len + 14 <= (int)sizeof(p.data) - ENTS_MAXTHINGS * 10; e++) {
		int sm;
		const float *o, *a;
		if (e == self || !ent_shown(e) || (sm = ent_submodel(e)) <= 0 || sm >= nummodels)
			continue;
		o = &QC_EdictFields(sv.vm, e)[F.origin].f;
		a = F.angles >= 0 ? &QC_EdictFields(sv.vm, e)[F.angles].f : NULL;
		if (!o[0] && !o[1] && !o[2] && (!a || (!a[0] && !a[1] && !a[2])))
			continue;
		p_u16(&p, (unsigned)sm);
		for (int c = 0; c < 3; c++)
			p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(o[c] * 4, -32768, 32767));
		for (int c = 0; c < 3; c++)
			p_u16(&p, (uint16_t)(int16_t)((a ? a[c] : 0) * 65536 / 360));
		count++;
	}
	p.data[countpos] = (uint8_t)count;
	/* the nearest other things, as boxes */
	qsort(things, (size_t)nthings, sizeof(*things), cmp_thing);
	countpos = p.len;
	p_u8(&p, 0);
	count = 0;
	for (int i = 0; i < nthings && count < ENTS_MAXTHINGS && p.len + 10 <= (int)sizeof(p.data); i++) {
		int e = things[i].e, flags = (int)QC_EFloat(sv.vm, e, F.flags);
		const float *o = &QC_EdictFields(sv.vm, e)[F.origin].f;
		const float *mn = F.mins >= 0 ? &QC_EdictFields(sv.vm, e)[F.mins].f : NULL;
		const float *mx = F.maxs >= 0 ? &QC_EdictFields(sv.vm, e)[F.maxs].f : NULL;
		float yaw = F.angles >= 0 ? QC_EdictFields(sv.vm, e)[F.angles + 1].f : 0;
		float half = 8, bottom = 0, top = 16;
		int kind = (flags & FL_MONSTER) ? 1 : QC_EFloat(sv.vm, e, F.solid) == SOLID_NOT ? 3 : 2;
		if (mn && mx && mx[0] > mn[0]) {
			half = BW_MAX(BW_MAX(-mn[0], mx[0]), BW_MAX(-mn[1], mx[1]));
			bottom = mn[2], top = mx[2];
		}
		if (QC_EFloat(sv.vm, e, F.health) <= 0 && (flags & FL_MONSTER))
			kind = 4; /* dead */
		for (int c = 0; c < 3; c++)
			p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(o[c] + (c == 2 ? bottom : 0), -32768, 32767));
		p_u8(&p, (unsigned)((int)(yaw * 256 / 360) & 255));
		p_u8(&p, (unsigned)BW_CLAMP(half, 1, 255));
		p_u8(&p, (unsigned)BW_CLAMP(top - bottom, 1, 255));
		p_u8(&p, (unsigned)kind);
		count++;
	}
	p.data[countpos] = (uint8_t)count;
	send_to(&l->from, &p);
}

void SV_LinkSend(void)
{
	double now = Sys_Seconds();
	if (sock < 0)
		return;
	for (int i = 0; i < MAX_LINKS; i++) {
		link_t *l = &links[i];
		if (!l->used || !l->client)
			continue;
		if (now - l->lastrecv > BW_MAX(sv_link_timeout->value, 2) && now > l->loading_until) {
			SV_DropClient(l->client, "the link timed out");
			continue;
		}
		if (l->client->state != CS_SPAWNED)
			continue;
		if (l->spawncount != svs.spawncount)
			send_welcome(l);
		if (!l->gamedrew && now >= l->nextdefault) {
			default_screen(l);
			l->nextdefault = now + 1;
		}
		if (l->acked != l->version && now >= l->nextscreen) {
			send_screen(l);
			l->nextscreen = now + SCREEN_RESEND;
		}
		if (now >= l->nextview) {
			send_view(l);
			send_ents(l);
			l->nextview = now + VIEW_INTERVAL;
		}
	}
}

/* the client's slot is going away (SV_DropClient) */
void SV_LinkClientDropped(client_t *c, const char *reason)
{
	link_t *l;
	pkt_t p;
	if (c->link <= 0 || c->link > MAX_LINKS)
		return;
	l = &links[c->link - 1];
	if (l->used) {
		p_begin(&p, L_KICK);
		p_u32(&p, l->session);
		p_str(&p, reason ? reason : "", 64);
		send_to(&l->from, &p);
		Con_Printf("link: %s left\n", c->name);
	}
	memset(l, 0, sizeof(*l));
	c->link = 0;
}

/* --- builtins --------------------------------------------------------------------------- */

static void bi_link_isdevice(qcvm_t *vm) { QC_ReturnFloat(vm, link_for_edict(QC_PENT(vm, 0)) ? 1 : 0); }

static void bi_link_keys(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	QC_ReturnFloat(vm, l ? l->keys : 0);
}

static void bi_link_touch(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	float v[3] = {0, 0, 0};
	if (l)
		v[0] = l->tx, v[1] = l->ty, v[2] = l->touch & 1;
	QC_ReturnVector(vm, v);
}

static void bi_link_clear(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	if (l)
		draw_clear(l, rgb15(QC_PVEC(vm, 1)));
}

static void bi_link_fill(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	if (l)
		draw_fill(l, (int)QC_PFLOAT(vm, 1), (int)QC_PFLOAT(vm, 2), (int)QC_PFLOAT(vm, 3), (int)QC_PFLOAT(vm, 4),
		          rgb15(QC_PVEC(vm, 5)));
}

static void bi_link_text(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	if (l)
		draw_text(l, (int)QC_PFLOAT(vm, 1), (int)QC_PFLOAT(vm, 2), QC_PSTR(vm, 3), rgb15(QC_PVEC(vm, 4)));
}

static void bi_link_present(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	if (l) {
		l->gamedrew = true;
		present(l);
	}
}

static void bi_link_marker(qcvm_t *vm)
{
	link_t *l = link_for_edict(QC_PENT(vm, 0));
	const float *o = QC_PVEC(vm, 1);
	marker_t *m;
	if (!l || l->nummarkers >= MAX_MARKERS)
		return;
	m = &l->markers[l->nummarkers++];
	for (int k = 0; k < 3; k++)
		m->org[k] = (int16_t)BW_CLAMP(o[k], -32768, 32767);
	m->kind = (uint8_t)BW_CLAMP(QC_PFLOAT(vm, 2), 1, 255);
	m->rgb = rgb15(QC_PVEC(vm, 3));
}

const qc_builtin_t sv_link_builtins[] = {
	{"link_isdevice", bi_link_isdevice}, {"link_keys", bi_link_keys}, {"link_touch", bi_link_touch},
	{"link_clear", bi_link_clear}, {"link_fill", bi_link_fill}, {"link_text", bi_link_text},
	{"link_present", bi_link_present}, {"link_marker", bi_link_marker}, {NULL, NULL}};

/* link_teleport x y z [yaw]: puts every link player there (sv_cheats 1), for
 * testing a level's triggers and changes from the server's console or rcon */
static void SV_LinkTeleport_f(int argc, const char **argv)
{
	if (!sv.active || argc < 4 || F.origin < 0)
		return;
	if (!Cvar_Value("sv_cheats")) {
		Con_Printf("link_teleport needs sv_cheats 1\n");
		return;
	}
	for (int i = 0; i < MAX_LINKS; i++) {
		link_t *l = &links[i];
		int e;
		if (!l->used || !l->client)
			continue;
		e = l->client->edict;
		for (int c = 0; c < 3; c++)
			QC_EdictFields(sv.vm, e)[F.origin + c].f = (float)atof(argv[1 + c]);
		if (F.velocity >= 0)
			memset(&QC_EdictFields(sv.vm, e)[F.velocity], 0, sizeof(qcval_t) * 3);
		if (argc > 4 && F.angles >= 0 && F.fixangle >= 0) {
			QC_EdictFields(sv.vm, e)[F.angles + 1].f = (float)atof(argv[4]);
			QC_EdictFields(sv.vm, e)[F.fixangle].f = 1;
		}
		SV_LinkEdict(e, true);
		Con_Printf("link: moved %s\n", l->client->name);
	}
}

/* link_stats: each link player's map, where it is, and how fast the device runs */
static void SV_LinkStats_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	for (int i = 0; i < MAX_LINKS; i++) {
		const link_t *l = &links[i];
		const float *o;
		if (!l->used || !l->client || !sv.active || F.origin < 0)
			continue;
		o = &QC_EdictFields(sv.vm, l->client->edict)[F.origin].f;
		Con_Printf("link %s map %s at %.0f %.0f %.0f eye %+.0f mins %.0f fps %d min %d polys %d load %d ms\n",
		           l->client->name, sv.mapname, o[0], o[1], o[2], QC_EField(sv.vm, l->client->edict, F.view_ofs) ?
		           QC_EField(sv.vm, l->client->edict, F.view_ofs)[2].f : 0, QC_EField(sv.vm, l->client->edict, F.mins) ?
		           QC_EField(sv.vm, l->client->edict, F.mins)[2].f : 0, l->fps, l->minfps, l->peakpolys, l->loadms);
	}
}

void SV_LinkInit(void)
{
	Cmd_AddD("link_stats", SV_LinkStats_f, "Show the link players and how fast their devices run");
	Cmd_AddD("link_teleport", SV_LinkTeleport_f, "Put the link players at x y z [yaw] (sv_cheats 1)");
	sv_link_port = Cvar_Get("sv_link_port", "27901", 0, "UDP port for link players (Nintendo DS), 0 for none");
	sv_link_max = Cvar_Get("sv_link_max", "4", 0, "Player slots link players may take");
	sv_link_timeout = Cvar_Get("sv_link_timeout", "10", 0, "Seconds without input before a link player is dropped");
}
