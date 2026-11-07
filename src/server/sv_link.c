/* Bulwark engine - link players: handhelds (the Nintendo DS first) that join
 * as companion players over a small UDP protocol. ISC licence, see LICENSE.
 *
 * docs/spec/link.md has the protocol. A link player holds a normal client
 * slot; the engine treats it like a bot for networking (no snapshots) while
 * the game sees a real player. An old device sends its state and the last
 * command repeats every frame; a link v2 device sends numbered commands,
 * which run once each, as a PC client's do, and gets back the player's state
 * after the last one so it can predict its own movement. Its input arrives
 * here, and what it sees goes back from here: a private screen the game
 * draws with the link_* builtins, and a view of where it is.
 */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/studio.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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
#define MAX_FX 16 /* impacts in one FX packet */
enum { FX_OWN = 1, FX_SHOT = 2 }; /* our own shot; shot by someone (tracer from 'from') */
#define VIEW_INTERVAL 0.05
#define LOAD_GRACE 60.0 /* seconds a device may take to load a map it was sent to */
#define ENTS_MAXMODELS 1024 /* brush models the presence bits cover */
#define ENTS_MAXTHINGS 48
#define THING_SIZE 18 /* bytes a thing in ENTS */
#define ENTS_MAXVEL 8  /* things whose velocity ENTS carries: the players */
/* what follows the things: u32 time, u8 count, count x {u8 thing, s16 velocity[3]} */
#define ENTS_TRAILER (4 + 1 + ENTS_MAXVEL * 7)
#define LINK_PLAYERMODEL 1000 /* model indexes from here are the players' own models */
#define ENTS_RANGE 2048.0f

enum { L_DISCOVER = 1, L_JOIN, L_INPUT, L_LEAVE };
enum { L_INFO = 0x81, L_WELCOME, L_REFUSE, L_SCREEN, L_VIEW, L_KICK, L_ENTS, L_MODELS, L_STATS, L_FX,
       L_PSTATE, L_PMOVEVARS, L_BRUSHINFO, L_WEAPONINFO, L_HUDINFO, L_EVENT };
enum { SC_CLEAR = 1, SC_FILL, SC_TEXT };

typedef struct {
	int16_t org[3];
	uint8_t kind;
	uint16_t rgb;
} marker_t;

/* link v2 (docs/spec/link.md, "Prediction") */
#define LINK_VER 2
#define PMV_COUNT 37       /* ncPMoveVars' values (Nuclide pmove.h), one set */
#define MAX_LBRUSH 512     /* brush entities in one BRUSHINFO version */
#define BRUSH_PAGE 70      /* entries in one BRUSHINFO packet */
#define MAX_MOVERS 8
#define MAX_BOXES 16
#define MAX_LOGBOXES 64
#define MOVER_RANGE 256.0f
#define BOX_RANGE 512.0f
#define PM_PERIOD 2.0      /* PMOVEVARS and BRUSHINFO again this often */
#define PM_RESEND 0.2      /* and this soon while the device has not acknowledged them */
#define EV_RING 64         /* EVENT records kept until the device has them */
#define EV_REC 224         /* the most bytes in one (type, length, payload) */
#define EV_RAW 4096        /* prints and game messages waiting to be read in SV_LinkSend */
#define EV_RESEND 0.2      /* EVENT again this soon while the device has not acknowledged it */
#define CMD_WINDOW 1024    /* the most newer than the last run an INPUT may be (17 s of commands) */
#define PSTATE_IDLE 0.1    /* PSTATE with a VIEW when no command ran for this long (movers, boxes) */
#define BRUSH_CHECK 0.25   /* how often the brush list is compared */
#define MAX_HUDBODY 1300   /* HUDINFO after the version */
#define HUD_OBITS 4        /* obituaries HUDINFO carries */
#define HUD_RESEND 0.25    /* HUDINFO again this soon while the device has not acknowledged it */
#define HUD_NPCRANGE 2000.0f /* friendly NPCs HUDINFO lists: Nameplates' cg_targetIDRange */
#define HUD_MAXNPC 8
#define HUD_NAME 31        /* an NPC's netname in HUDINFO is cut to this */

typedef struct {
	uint16_t e, model; /* edict; the * number of its brush model */
	uint8_t solid;     /* 4 or 22 */
	int8_t skin;       /* a Quake contents value the whole model takes, 0 for none */
	float org[3];
} lbrush_t;

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
	int modelcursor; /* where the next MODELS packet starts in the model list */
	int fps, minfps, peakpolys, loadms; /* what the device last said about itself; -1 unknown */
	double loading_until; /* a new map was announced: the device may be quiet while it loads it */
	double welcomed, nextwelcome; /* when this map was announced; when to say it again */
	int devspawn;                 /* the map (spawn count) the device says it has, -1 if it doesn't say */
	int buttons;                  /* the game's buttons as the device mapped them, -1 if it doesn't */
	int impseq;                   /* the count of the device's last one-shot impulse */
	int vmseq;                    /* the view model's sequence when last looked at */
	float vmtime;                 /* and how long it had played */
	uint8_t vmplays, vmsent;      /* sequences started (the same one again counts), and as last sent */
	struct {
		uint8_t flags;      /* FX_ flags */
		float from[3], to[3];
	} fx[MAX_FX];
	int numfx;                    /* impacts waiting for the next FX packet */

	/* link v2: the device's own commands, and what it needs to predict them */
	int joinver;                  /* linkver from JOIN, 0 from an old device */
	bool v2;                      /* an INPUT carried commands */
	bool spawned;                 /* the client was in the game at the last send */
	uint32_t lastrun;             /* the last command run or dropped */
	uint16_t gaps;                /* commands that never arrived */
	uint8_t clipped;              /* commands the movement budget cut short */
	uint16_t pseq;
	uint32_t pstate_ack;          /* lastrun as the last PSTATE carried it */
	double lastpstate;            /* when it went */
	uint8_t fixseq;               /* +1 each time the game turned the player */
	int16_t fixpitch, fixyaw;
	bool warned;                  /* missing fields reported */
	int v2map;                    /* the spawncount the rest belongs to, -1 none */
	uint16_t varsver, varssent, varsack;
	bool varsread, varsonce;      /* read for this map; sent at least once */
	float vars[2 * PMV_COUNT];    /* g_pmoveVars, then the player's m_pmoveVars */
	double nextvars, lastvars;
	uint16_t brushver, brushsent, brushack;
	bool brushread;
	lbrush_t brush[MAX_LBRUSH];
	int numbrush;
	double nextbrush, lastbrush, brushcheck;
	/* sv_link_log: what the log last printed */
	bool logon, logmap, logvarsok, logbrushok;
	float logvars[2 * PMV_COUNT];
	lbrush_t logbrush[MAX_LBRUSH];
	int numlogbrush;
	/* weapons: WEAPONINFO for the weapon in hand */
	uint16_t wiedict, wiver, wivers, wiack; /* its edict and version (0 none); the last version given; acked */
	uint8_t wibody[1200];                   /* the packet after the version */
	int wilen;
	int8_t wiammo[2];                       /* its fireInfos' ammo types */
	double wicheck, wisent;
	uint16_t wilogver;                      /* the version sv_link_log printed */
	bool wicap;                             /* its INPUT carried 'W': it takes WEAPONINFO */
	uint8_t wisends;                        /* sends of this version with an INPUT since the one before */
	bool wiheard;                           /* an INPUT with 'W' came since the last send (a device
	                                         * loading its map sends none: those don't count) */
	int32_t wiclass;                        /* the weapon's classname (a QC string) */
	/* HUDINFO: what the co-op HUD shows, for a device that draws it itself */
	bool hudcap;                            /* its INPUT carried 'H': it takes HUDINFO */
	uint16_t hudver, hudack, hudsentver;    /* the version now (0 none yet); the device's; the last sent */
	uint8_t hudbody[MAX_HUDBODY];           /* the packet after the version */
	int hudlen;
	double hudsent;
	uint8_t obitseq;                        /* obituaries seen, mod 256 */
	uint8_t obitkept;                       /* of them in obit[], at most HUD_OBITS (obitseq wraps) */
	struct {
		char who[32], by[32], weapon[32], how[16];
		bool named; /* weapon is the decl's inv_name now */
	} obit[HUD_OBITS];                      /* the newest, obit[(obitseq - 1) % HUD_OBITS] */
	int32_t hudwclass;                      /* the weapon class its crosshair was looked up for */
	int hudmap;                             /* the spawncount hudwclass and the names belong to */
	uint8_t xw, xh;                         /* that crosshair: size (0 none), colour, pixels */
	uint16_t xrgb;
	uint8_t xmask[128];
	char hudstatic[64], hudstaticname[64], huddynamic[64], huddynamicname[64]; /* *herohud's weapons, named */
	/* events: what the PC's client shows from prints and the game's CSQC events (EVENT) */
	uint8_t evraw[EV_RAW];                  /* what came for the client this frame, not read yet */
	int evrawlen;
	uint8_t evring[EV_RING][EV_REC];        /* records the device has not acknowledged, by seq % EV_RING */
	uint8_t evlen[EV_RING];
	uint16_t evnext, evack;                 /* the next record's seq (the first is 1); the last the device has, in order */
	uint16_t evsentnext;                    /* evnext at the last send */
	bool evcap;                             /* its INPUT carried 'E': it takes EVENT */
	double evsent;
	uint32_t evdropped, evcount;            /* records lost to a full ring; records made */
	char evfeed[96];                        /* serverinfo coop_feed as last seen */
	uint32_t evtext[5];                     /* each game_text channel's last record (a hash), 0 none */
	bool evfeedinit;
} link_t;

static link_t links[MAX_LINKS];
static int sock = -1;
static int boundport;
static cvar_t *sv_link_port, *sv_link_max, *sv_link_timeout, *sv_link_log, *sv_link_lag;

/* --- packets ---------------------------------------------------------------------------- */

typedef struct {
	uint8_t data[1400];
	int len;
} pkt_t;
/* HUDINFO: magic, type, session, version, then the body */
_Static_assert(MAX_HUDBODY + 11 <= sizeof(((pkt_t *)0)->data), "HUDINFO's body must fit a packet");

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

/* sv_link_lag: packets held for a while each way, to measure a device at a
 * given round trip without a network emulator */
#define LAG_QUEUE 256
typedef struct {
	double due;
	struct sockaddr_in addr;
	int len;
	uint8_t data[1500];
} lagpkt_t;
typedef struct {
	lagpkt_t *q;
	int head, num;
} lagqueue_t;
static lagqueue_t lagin, lagout;

static double link_lag(void) { return sv_link_lag && sv_link_lag->value > 0 ? sv_link_lag->value / 1000.0 : 0; }

static bool lag_push(lagqueue_t *lq, const struct sockaddr_in *a, const uint8_t *d, int len)
{
	lagpkt_t *k;
	if (!lq->q)
		lq->q = Mem_Alloc(sizeof(lagpkt_t) * LAG_QUEUE);
	if (lq->num >= LAG_QUEUE || len > (int)sizeof(k->data))
		return false;
	k = &lq->q[(lq->head + lq->num++) % LAG_QUEUE];
	k->due = Sys_Seconds() + link_lag();
	k->addr = *a;
	k->len = len;
	memcpy(k->data, d, (size_t)len);
	return true;
}

/* the oldest packet once it is due (all of them when force), else NULL */
static const lagpkt_t *lag_pop(lagqueue_t *lq, bool force)
{
	const lagpkt_t *k;
	if (!lq->num || (!force && lq->q[lq->head].due > Sys_Seconds()))
		return NULL;
	k = &lq->q[lq->head];
	lq->head = (lq->head + 1) % LAG_QUEUE;
	lq->num--;
	return k;
}

static void send_raw(const struct sockaddr_in *to, const uint8_t *d, int len)
{
	if (sock >= 0)
		sendto(sock, (const char *)d, (size_t)len, MSG_NOSIGNAL, (const struct sockaddr *)to, sizeof(*to));
}

static void flush_lagout(void)
{
	const lagpkt_t *k;
	bool off = link_lag() <= 0;
	while ((k = lag_pop(&lagout, off)))
		send_raw(&k->addr, k->data, k->len);
}

static void send_to(const struct sockaddr_in *to, const pkt_t *p)
{
	if (sock < 0)
		return;
	if (link_lag() > 0 && lag_push(&lagout, to, p->data, p->len))
		return;
	flush_lagout();
	send_raw(to, p->data, p->len);
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

/* the link a packet is from: by session and address, and by port as well when
 * two DSes behind one router picked the same session; otherwise the port may
 * have moved (NAT) */
static link_t *find_session(uint32_t session, const struct sockaddr_in *from, bool exact)
{
	link_t *only = NULL;
	int count = 0;
	for (int i = 0; i < MAX_LINKS; i++)
		if (links[i].used && links[i].session == session && links[i].from.sin_addr.s_addr == from->sin_addr.s_addr) {
			if (links[i].from.sin_port == from->sin_port)
				return &links[i];
			only = &links[i], count++;
		}
	return count == 1 && !exact ? only : NULL;
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
	p_u8(&p, LINK_VER);
	send_to(&l->from, &p);
	if (l->spawncount != svs.spawncount)
		l->loading_until = Sys_Seconds() + LOAD_GRACE, l->welcomed = Sys_Seconds();
	l->spawncount = svs.spawncount;
	l->nextwelcome = Sys_Seconds() + 2;
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
	if ((l = find_session(session, from, true))) {
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
	l->devspawn = -1;
	l->buttons = -1;
	l->impseq = -1;
	l->joinver = len >= 4 + 2 + 16 + 1 ? d[22] : 0;
	l->v2map = -1;
	l->evnext = 1; /* EVENT records from 1; 0 acknowledged: none */
	send_welcome(l);
	Con_Printf("link: %s joined from %s\n", name, inet_ntoa(from->sin_addr));
}

static void input_v2(link_t *l, const uint8_t *d, int len);

/* INPUT: the device's state, then either its commands (link v2: an extension
 * after the v1 fields, run here once each) or, from an old device, one state
 * of the sticks and buttons that repeats every frame until the next packet */
static void handle_input(link_t *l, const uint8_t *d, int len)
{
	client_t *c = l->client;
	usercmd_t *u;
	int16_t pitch, yaw;
	uint16_t seq;
	if (len < 4 + 2 + 2 + 3 + 3 + 4 + 2 || !c)
		return;
	seq = rd16(d + 4);
	if ((int16_t)(seq - l->lastseq) <= 0 && l->lastseq)
		return; /* old or repeated */
	l->lastseq = seq;
	l->keys = rd16(d + 6);
	l->touch = d[8], l->tx = d[9], l->ty = d[10];
	l->acked = rd16(d + 18);
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 4) /* the device's own frame rate, last second */
		l->fps = d[20], l->minfps = d[21], l->peakpolys = rd16(d + 22);
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 6)
		l->loadms = rd16(d + 24) * 10;
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 8)
		l->devspawn = (uint16_t)rd16(d + 26);
	/* after the v1 part, the commands, 'W' and 'H' (link v2), or neither: 'E', the
	 * last EVENT record the device has in order; a device that sends it takes EVENT */
	{
		int t = 32;
		if (len >= 44 && d[32] == 'C' && d[38] >= 1 && d[38] <= 8 && d[39] == 14 && len >= 44 + 14 * d[38]) {
			t = 44 + 14 * d[38];
			if (len >= t + 5 && d[t] == 'W')
				t += 5;
			if (len >= t + 3 && d[t] == 'H')
				t += 3;
		}
		if (len >= t + 3 && d[t] == 'E') {
			uint16_t ack = (uint16_t)rd16(d + t + 1);
			l->evcap = true;
			/* only forward, and never past what was made */
			if ((uint16_t)(ack - l->evack) <= (uint16_t)(l->evnext - 1 - l->evack))
				l->evack = ack;
		}
	}
	/* the v2 extension: 'C', version, u32 newest, u8 count, u8 cmdsize, u16
	 * varsack, u16 brushack, then count commands, oldest first */
	if (len >= 44 && d[32] == 'C' && d[33] == LINK_VER && d[38] >= 1 && d[38] <= 8 && d[39] == 14 &&
	    len >= 44 + 14 * d[38] && rd32(d + 34) >= d[38]) {
		input_v2(l, d, len);
		return;
	}
	if (c->state != CS_SPAWNED)
		return;
	if (len >= 4 + 2 + 2 + 3 + 3 + 4 + 2 + 12) {
		/* the device's own button mapping, and a one-shot impulse it repeats
		 * in every packet: taken once, when its count changes */
		l->buttons = (uint16_t)rd16(d + 28);
		if (l->impseq >= 0 && d[31] != l->impseq && d[30])
			c->forced_impulse = d[30];
		l->impseq = d[31];
	}

	pitch = (int16_t)rd16(d + 14);
	yaw = (int16_t)rd16(d + 16);
	u = &c->lastucmd;
	u->angles[0] = -pitch * (360.0f / 65536.0f); /* the device's pitch is up-positive */
	u->angles[1] = yaw * (360.0f / 65536.0f);
	u->angles[2] = 0;
	u->movevalues[0] = (int8_t)d[11] * (400.0f / 127.0f);
	u->movevalues[1] = (int8_t)d[12] * (400.0f / 127.0f);
	u->movevalues[2] = (int8_t)d[13] * (400.0f / 127.0f);
	/* a device that maps its own buttons sends them; otherwise the DS keys
	 * (libnds: A 1, B 2, R 0x100, X 0x400, Y 0x800) become attack, jump, use
	 * (0x10), reload (8) and attack2 (0x20), as Nuclide numbers them
	 * (src/shared/input/input.h). DS builds before link v2 (JOIN without a
	 * linkver) sent use as 0x20 and attack2 as 0x10: those two are swapped back */
	if (l->buttons >= 0 && l->joinver < 2)
		u->buttons = ((unsigned)l->buttons & ~0x30u) | ((unsigned)l->buttons & 0x10 ? 0x20 : 0) |
		             ((unsigned)l->buttons & 0x20 ? 0x10 : 0);
	else if (l->buttons >= 0)
		u->buttons = (unsigned)l->buttons;
	else
		u->buttons = (l->keys & 0x001 ? 1 : 0) | (l->keys & 0x002 ? 2 : 0) | (l->keys & 0x400 ? 0x10 : 0) |
		             (l->keys & 0x800 ? 8 : 0) | (l->keys & 0x100 ? 0x20 : 0);
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

static void handle_packet(const struct sockaddr_in *from, const uint8_t *buf, int n)
{
	link_t *l;
	if (n < 5 || memcmp(buf, LINK_MAGIC, 4))
		return;
	switch (buf[4]) {
	case L_DISCOVER: handle_discover(from); break;
	case L_JOIN: handle_join(from, buf + 5, n - 5); break;
	case L_INPUT:
	case L_LEAVE:
		if (n < 9 || !(l = find_session(rd32(buf + 5), from, false)))
			break;
		l->lastrecv = Sys_Seconds();
		l->from.sin_port = from->sin_port; /* NAT may move it */
		if (buf[4] == L_INPUT)
			handle_input(l, buf + 5, n - 5);
		else if (l->client)
			SV_DropClient(l->client, "left");
		break;
	}
}

void SV_LinkReadPackets(void)
{
	uint8_t buf[1500];
	const lagpkt_t *k;
	bool lagged;
	open_socket();
	if (sock < 0)
		return;
	lagged = link_lag() > 0;
	flush_lagout();
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break;
		if (lagged && lag_push(&lagin, &from, buf, n))
			continue;
		while ((k = lag_pop(&lagin, true))) /* keep the order */
			handle_packet(&k->addr, k->data, k->len);
		handle_packet(&from, buf, n);
	}
	while ((k = lag_pop(&lagin, !lagged)))
		handle_packet(&k->addr, k->data, k->len);
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

static float efloat(int e, const char *name);
static int active_weapon(int e);
static void raise_fix(link_t *l, float p, float y);

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
		raise_fix(l, 0, a[1]);
	}
	/* 2: on the ground (FL_ONGROUND), 4: crouching (Nuclide's VFL_CROUCHING) */
	flags |= ((int)efloat(e, "flags") & 512 ? 2 : 0) | ((int)efloat(e, "m_nuclideFlags") & 4 ? 4 : 0);
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
	/* after the markers: the player's velocity (units a second), for the
	 * device's bob and roll, which the PC client works out from it */
	{
		int vofs = QC_FieldOfs(sv.vm, "velocity");
		for (int k = 0; k < 3; k++)
			p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(vofs >= 0 ? QC_EdictFields(sv.vm, e)[vofs + k].f : 0, -32768, 32767));
	}
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

/* the model index of a studio (.mdl) model, else 0 */
static int ent_studio(int e)
{
	int mi = (int)QC_EFloat(sv.vm, e, F.modelindex);
	const char *name = mi > 0 && mi < SV_MAX_MODELS ? sv.models[mi] : NULL;
	size_t n = name ? strlen(name) : 0;
	if (n > 4 && !Str_ICmp(name + n - 4, ".mdl"))
		return mi;
	/* a player drawn by its own client: the model its userinfo names */
	return e >= 1 && e <= svs.maxclients ? LINK_PLAYERMODEL + e - 1 : 0;
}

/* frame1time as sent (u16, 1/100 s): Nuclide only sets it back when the sequence
 * changes (SurfacePropEntity.qc), so a monster idling for long would pass 655 s and
 * stop there. The PC poses a looping sequence at frame floor(t * fps) % numframes
 * (skel.c Skel_SampleRelative, fps 10 when the model has none): taken modulo the most whole
 * laps of numframes / fps under 600 s it poses the same, and comes round to 0 once in
 * that while; a sequence that doesn't loop holds its last frame long before */
static float ent_seqtime(int e, float t)
{
	int mi = (int)QC_EFloat(sv.vm, e, F.modelindex), seq = F.frame >= 0 ? (int)QC_EFloat(sv.vm, e, F.frame) : 0;
	const char *name = mi > 0 && mi < SV_MAX_MODELS ? sv.models[mi] : NULL;
	const studio_model_t *m;
	float dur, period;
	if (t < 600 || !ent_studio(e) || !name || !(m = Studio_Cached(name)) || seq < 0 || seq >= m->numseqs ||
	    !(m->seqs[seq].flags & STUDIO_LOOPING) ||
	    (dur = (float)BW_MAX(1, m->seqs[seq].numframes) / (m->seqs[seq].fps > 0 ? m->seqs[seq].fps : 10)) > 600)
		return t;
	period = dur * floorf(600 / dur);
	return fmodf(t, period);
}

/* the model a player's userinfo picks, as a path */
static void player_model(const client_t *c, char *out, size_t size)
{
	char m[64] = "";
	SV_InfoGet(c->userinfo, "model", m, sizeof(m));
	for (char *q = m; *q; q++)
		if (*q == '/' || *q == '\\' || *q == '.')
			*q = 0;
	if (m[0])
		snprintf(out, size, "models/player/%s/%s.mdl", m, m);
	else
		snprintf(out, size, "models/player.mdl");
}

/* another player in the game, as a thing: only once it is in the map */
static bool client_spawned(int e)
{
	return e >= 1 && e <= svs.maxclients && svs.clients[e - 1].state == CS_SPAWNED;
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

static void send_stats(link_t *l);
static void watch_viewmodel(link_t *l);
static void send_fx(link_t *l);
static void ev_read(link_t *l);
static void ev_coopfeed(link_t *l);
static void send_events(link_t *l, double now);

/* the names of the studio models, a run at a time (docs/spec/link.md,
 * MODELS): u32 session, u16 spawncount, u8 count, count x {u16 index, u8
 * length, name} */
static void send_models(link_t *l)
{
	pkt_t p;
	int countpos, count = 0, i = l->modelcursor;
	p_begin(&p, L_MODELS);
	p_u32(&p, l->session);
	p_u16(&p, (unsigned)svs.spawncount);
	countpos = p.len;
	p_u8(&p, 0);
	/* the players' models first, every time: they are few and can change */
	for (int c = 0; c < svs.maxclients && c < 32; c++) {
		char name[96];
		size_t n;
		if (svs.clients[c].state != CS_SPAWNED)
			continue;
		player_model(&svs.clients[c], name, sizeof(name));
		n = strlen(name);
		p_u16(&p, (unsigned)(LINK_PLAYERMODEL + c));
		p_u8(&p, (unsigned)n);
		p_bytes(&p, (const uint8_t *)name, (int)n);
		count++;
	}
	if (i <= 0 || i >= sv.nummodels)
		i = 1;
	for (; i < sv.nummodels && i < SV_MAX_MODELS && count < 255; i++) {
		size_t n = sv.models[i] ? strlen(sv.models[i]) : 0;
		if (n < 5 || n > 63 || Str_ICmp(sv.models[i] + n - 4, ".mdl"))
			continue;
		if (p.len + 3 + (int)n > (int)sizeof(p.data))
			break;
		p_u16(&p, (unsigned)i);
		p_u8(&p, (unsigned)n);
		p_bytes(&p, (const uint8_t *)sv.models[i], (int)n);
		count++;
	}
	l->modelcursor = i >= sv.nummodels ? 1 : i;
	p.data[countpos] = (uint8_t)count;
	send_to(&l->from, &p);
}

static void send_ents(link_t *l)
{
	static uint8_t present[ENTS_MAXMODELS / 8];
	static thing_t things[MAX_EDICTS_LINK];
	pkt_t p;
	int self = l->client->edict, nummodels = 0, nthings = 0, countpos, count = 0, nvel = 0;
	const float *me = &QC_EdictFields(sv.vm, self)[F.origin].f;
	int f1t = QC_FieldOfs(sv.vm, "frame1time"), vofs = QC_FieldOfs(sv.vm, "velocity");
	uint8_t velthing[ENTS_MAXVEL];
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
		else if (sm < 0 && nthings < MAX_EDICTS_LINK && !(e <= svs.maxclients && !client_spawned(e))) {
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
	for (int e = 1; e < sv.vm->num_edicts && count < 255 && p.len + 14 <= (int)sizeof(p.data) - 2 - ENTS_MAXTHINGS * THING_SIZE - ENTS_TRAILER; e++) {
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
	p_u8(&p, THING_SIZE);
	count = 0;
	for (int i = 0; i < nthings && count < ENTS_MAXTHINGS && p.len + THING_SIZE <= (int)sizeof(p.data) - ENTS_TRAILER; i++) {
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
		/* which studio model it is and the sequence it plays, for a device
		 * that has the model; the entity number keeps its animation going */
		p_u16(&p, (unsigned)e);
		p_u16(&p, (unsigned)ent_studio(e));
		p_u8(&p, F.frame >= 0 ? (unsigned)BW_CLAMP((int)QC_EFloat(sv.vm, e, F.frame), 0, 255) : 0);
		p_u8(&p, (unsigned)BW_CLAMP(-bottom, 0, 255)); /* the origin, above the bottom */
		/* the time into that sequence (the game's frame1time, which the PC
		 * draws the model at), 1/100 s; 0xffff when the game has none */
		p_u16(&p, f1t < 0 ? 0xffffu : (unsigned)BW_CLAMP((int)lrintf(ent_seqtime(e, QC_EdictFields(sv.vm, e)[f1t].f) * 100), 0, 0xfffe));
		if (e <= svs.maxclients && nvel < ENTS_MAXVEL && vofs >= 0)
			velthing[nvel++] = (uint8_t)count;
		count++;
	}
	p.data[countpos] = (uint8_t)count;
	/* the game's time for all of the above (the PC's snapshots carry it too),
	 * milliseconds; then the velocity of the players among the things, which
	 * the PC's client game moves on from between updates (Nuclide's
	 * cg_extrapolateClients) */
	p_u32(&p, (uint32_t)(int64_t)llround(sv.time * 1000));
	p_u8(&p, (unsigned)nvel);
	for (int i = 0; i < nvel; i++) {
		int e = things[velthing[i]].e;
		p_u8(&p, velthing[i]);
		for (int c = 0; c < 3; c++)
			p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(QC_EdictFields(sv.vm, e)[vofs + c].f, -32768, 32767));
	}
	send_to(&l->from, &p);
}

/* --- link v2: the device predicts its own movement --------------------------------------------
 * docs/spec/link.md, "Prediction". The device runs a C port of the game's
 * player movement (Nuclide's PMoveCustom and ncPlayer::Physics_Run). It needs
 * the player's state after the last command the server ran (PSTATE), the
 * movement values the game uses (PMOVEVARS), and which brush entities collide
 * (BRUSHINFO); PSTATE also carries the movers and boxes near the player. */

enum { PS_PREDICT = 1, PS_AUTOJUMP = 4, PS_LONGJUMP = 8, PS_HLRULES = 16 };
#define VFL_INVEHICLE (1 << 3)
#define VFL_FAKESPEC (1 << 6)

/* ncPMoveVars' values in the order of Nuclide's pmove.h */
static const char *const pmv_names[PMV_COUNT] = {
	"g_gravity", "pm_accelerate", "pm_airaccelerate", "pm_airstepsize", "pm_boxcenter", "pm_boxwidth",
	"pm_crouchheight", "pm_crouchspeed", "pm_crouchviewheight", "pm_edgefriction", "pm_friction", "pm_gravity",
	"pm_jumpheight", "pm_airjumps", "pm_backspeed", "pm_groundspeedcap", "pm_jumpspeedcap", "pm_maxviewpitch",
	"pm_minviewpitch", "pm_noclipaccelerate", "pm_noclipspeed", "pm_normalheight", "pm_normalviewheight",
	"pm_nospeedcap", "pm_proneheight", "pm_pronespeed", "pm_proneviewheight", "pm_runspeed", "pm_runfiring",
	"pm_stamina", "pm_staminarate", "pm_staminathreshold", "pm_stepsize", "pm_stopspeed", "pm_walkspeed",
	"pm_wateraccelerate", "pm_waterjumpheight"};

/* the game's fields and globals the movement reads, looked up once a map;
 * -1 when the game has none */
static struct {
	const qcvm_t *vm;
	int spawncount;
	int vflags, gflags, friction, maxspeed, basevel, hookvel, tsj, tst, stamina, speedtarget, viewzoom;
	int wsm, wsmfire, wsmzoom, pmvars, isplayer, itemlist, nextitem, declclass;
	int var[PMV_COUNT];
	int g_pmovevars, g_maploaded, g_menubg; /* globals */
	bool hlrules;
} pf;

/* a class member field (Nuclide's compiler names them Class::__m<name>), else the plain field */
static int cfield(const char *cls, const char *mname)
{
	char full[96];
	int ofs;
	snprintf(full, sizeof(full), "%s::__m%s", cls, mname);
	ofs = QC_FieldOfs(sv.vm, full);
	return ofs >= 0 ? ofs : QC_FieldOfs(sv.vm, mname);
}

static void pm_fields(void)
{
	if (pf.vm == sv.vm && pf.spawncount == svs.spawncount)
		return;
	pf.vm = sv.vm, pf.spawncount = svs.spawncount;
	pf.vflags = QC_FieldOfs(sv.vm, "m_nuclideFlags");
	pf.gflags = QC_FieldOfs(sv.vm, "gflags");
	pf.friction = QC_FieldOfs(sv.vm, "friction");
	pf.maxspeed = QC_FieldOfs(sv.vm, "maxspeed");
	pf.basevel = cfield("ncPlayer", "m_pmoveBaseVelocity");
	pf.hookvel = cfield("ncPlayer", "m_pmoveHookVelocity");
	pf.tsj = cfield("ncPlayer", "m_timeSinceJump");
	pf.tst = cfield("ncPlayer", "m_timeSinceTeleport");
	pf.stamina = cfield("ncActor", "m_currentStamina");
	pf.speedtarget = cfield("ncPlayer", "m_pmoveSpeedTarget");
	pf.viewzoom = cfield("ncPlayer", "viewzoom");
	pf.wsm = cfield("ncWeapon", "m_fiSpeedMod");
	pf.wsmfire = cfield("ncWeapon", "m_fiSpeedModFiring");
	pf.wsmzoom = cfield("ncWeapon", "m_fiSpeedModZoomed");
	pf.pmvars = cfield("ncPlayer", "m_pmoveVars");
	pf.isplayer = QC_FieldOfs(sv.vm, "_isPlayer");
	pf.itemlist = cfield("ncActor", "m_itemList");
	pf.nextitem = cfield("ncItem", "m_nextItem");
	pf.declclass = QC_FieldOfs(sv.vm, "declclass");
	for (int i = 0; i < PMV_COUNT; i++)
		pf.var[i] = cfield("ncPMoveVars", pmv_names[i]);
	pf.g_pmovevars = QC_GlobalOfs(sv.vm, "g_pmoveVars");
	pf.g_maploaded = QC_GlobalOfs(sv.vm, "g_mapEntitiesLoaded");
	pf.g_menubg = QC_GlobalOfs(sv.vm, "g_menuBackgroundMapMode");
	pf.hlrules = QC_FindFunction(sv.vm, "hlPlayer::Physics_Jump") > 0;
}

/* a new map: everything the device was told about the old one goes */
static void v2_sync(link_t *l)
{
	pm_fields();
	if (l->v2map == svs.spawncount)
		return;
	l->v2map = svs.spawncount;
	l->varsread = l->varsonce = l->brushread = false;
	l->varsack = l->brushack = 0;
	l->numbrush = 0;
	l->nextvars = l->nextbrush = l->brushcheck = 0;
	l->logmap = l->logvarsok = l->logbrushok = false;
	l->wiedict = l->wiver = l->wiack = l->wilogver = 0;
	l->wicheck = 0;
	l->wiclass = 0;
}

/* the game turned the player: the device takes these angles (QC's, pitch down-positive) */
static void raise_fix(link_t *l, float p, float y)
{
	l->fixseq++;
	l->fixpitch = (int16_t)(uint16_t)lrintf(-p * 65536 / 360);
	l->fixyaw = (int16_t)(uint16_t)lrintf(y * 65536 / 360);
	Con_DPrintf("link: the game turned %s to %g %g (fixseq %d)\n", l->client ? l->client->name : "?", p, y, l->fixseq);
}

static const float *fvec(int e, int ofs)
{
	static const float zero[3] = {0, 0, 0};
	return ofs >= 0 ? &QC_EdictFields(sv.vm, e)[ofs].f : zero;
}

static bool ent_live(int e) { return e > 0 && e < sv.vm->num_edicts && !sv.vm->edicts[e].free; }

/* the server's world has a brush model for e (sv_world.c's ent_brush_model) */
static bool ent_has_bsp(int e)
{
	int mi = (int)QC_EFloat(sv.vm, e, F.modelindex);
	return mi > 0 && mi < sv.nummodels && mi < SV_MAX_MODELS && sv.modelinfo[mi].bsp && sv.modelinfo[mi].bsp->type != BSP_NONE;
}

/* SV_Move's owner and dimension rules with the player as passent (sv_world.c) */
static bool ent_tested(int e, int self)
{
	int passowner = QC_EInt(sv.vm, self, F.owner);
	int passhit = F.dimension_hit >= 0 ? (int)QC_EFloat(sv.vm, self, F.dimension_hit) : 255;
	int owner = QC_EInt(sv.vm, e, F.owner);
	if (owner == self || (passowner && passowner == e))
		return false;
	if (F.dimension_solid >= 0 && !((int)QC_EFloat(sv.vm, e, F.dimension_solid) & passhit))
		return false;
	return true;
}

/* the contents that stop the player's own moves (sv_world.c's default_mask) */
static int move_mask(int self)
{
	int mask = QC_EInt(sv.vm, self, F.hitcontentsmaski);
	if (mask)
		return mask;
	mask = CONTENTS_SOLID | CONTENTS_BODY;
	if ((int)QC_EFloat(sv.vm, self, F.flags) & FL_CLIENT)
		mask |= CONTENTS_PLAYERCLIP;
	return mask;
}

/* --- the player's state --- */

typedef struct {
	float origin[3], velocity[3], basevel[3], hookvel[3], view_ofs[3], mins[3], maxs[3];
	uint32_t flags, vflags, gflags;
	int movetype, solid, waterlevel, watertype, groundent;
	float gravity, friction, maxspeed, tsj, tst, stamina, speedtarget, speedmod, wsm, wsmfire, wsmzoom, viewzoom;
	float health;
	unsigned rules; /* status bits 2-4, shifted down: 1 autojump, 2 long jump, 4 Half-Life's rules */
} lstate_t;

static bool has_longjump(int e)
{
	int it = pf.itemlist >= 0 ? QC_EInt(sv.vm, e, pf.itemlist) : 0;
	for (int n = 0; n < 1024 && ent_live(it) && pf.declclass >= 0; n++) {
		string_t dc = QC_EInt(sv.vm, it, pf.declclass);
		const char *name = dc ? QC_GetString(sv.vm, dc) : "";
		if (!name || !name[0])
			break;
		if (!strcmp(name, "item_longjump"))
			return true;
		it = pf.nextitem >= 0 ? QC_EInt(sv.vm, it, pf.nextitem) : 0;
	}
	return false;
}

static void read_state(const link_t *l, int e, lstate_t *s)
{
	const float *src[7] = {fvec(e, F.origin), fvec(e, F.velocity), fvec(e, pf.basevel), fvec(e, pf.hookvel),
	                       fvec(e, F.view_ofs), fvec(e, F.mins), fvec(e, F.maxs)};
	float *dst[7] = {s->origin, s->velocity, s->basevel, s->hookvel, s->view_ofs, s->mins, s->maxs};
	int w = active_weapon(e), g = F.groundentity >= 0 ? QC_EInt(sv.vm, e, F.groundentity) : 0;
	char aj[8] = "";
	for (int i = 0; i < 7; i++)
		memcpy(dst[i], src[i], sizeof(float) * 3);
	s->flags = (uint32_t)(int)QC_EFloat(sv.vm, e, F.flags);
	s->vflags = (uint32_t)(int)QC_EFloat(sv.vm, e, pf.vflags);
	s->gflags = (uint32_t)(int)QC_EFloat(sv.vm, e, pf.gflags);
	s->movetype = (int)QC_EFloat(sv.vm, e, F.movetype);
	s->solid = (int)QC_EFloat(sv.vm, e, F.solid);
	s->waterlevel = (int)QC_EFloat(sv.vm, e, F.waterlevel);
	s->watertype = (int)QC_EFloat(sv.vm, e, F.watertype);
	s->groundent = ent_live(g) ? g : 0;
	s->gravity = QC_EFloat(sv.vm, e, F.gravity);
	s->friction = QC_EFloat(sv.vm, e, pf.friction);
	s->maxspeed = QC_EFloat(sv.vm, e, pf.maxspeed);
	s->tsj = QC_EFloat(sv.vm, e, pf.tsj);
	s->tst = QC_EFloat(sv.vm, e, pf.tst);
	s->stamina = QC_EFloat(sv.vm, e, pf.stamina);
	s->speedtarget = QC_EFloat(sv.vm, e, pf.speedtarget);
	s->viewzoom = QC_EFloat(sv.vm, e, pf.viewzoom);
	if (w) {
		s->wsm = QC_EFloat(sv.vm, w, pf.wsm);
		s->wsmfire = QC_EFloat(sv.vm, w, pf.wsmfire);
		s->wsmzoom = QC_EFloat(sv.vm, w, pf.wsmzoom);
	} else
		s->wsm = 1, s->wsmfire = 0, s->wsmzoom = 0;
	/* ncWeapon::PlayerSpeedMod (Weapon.qc), for the log and the device to check */
	if (s->wsmzoom > 0.0f && s->viewzoom < 1.0f)
		s->speedmod = s->wsmzoom;
	else if (s->wsmfire > 0.0f && (s->vflags & (1u << 13)))
		s->speedmod = s->wsmfire;
	else
		s->speedmod = s->wsm;
	s->health = QC_EFloat(sv.vm, e, F.health);
	SV_InfoGet(l->client->userinfo, "autojump", aj, sizeof(aj));
	s->rules = (!strcmp(aj, "1") ? 1u : 0) | (has_longjump(e) ? 2u : 0) | (pf.hlrules ? 4u : 0);
}

/* PSTATE's PREDICT bit: the device's port would run what the server runs */
static bool can_predict(link_t *l, int e, const lstate_t *s)
{
	static const char *const need[] = {"origin", "velocity", "mins", "maxs", "flags", "m_nuclideFlags", "movetype", "view_ofs"};
	int fields[] = {F.origin, F.velocity, F.mins, F.maxs, F.flags, pf.vflags, F.movetype, F.view_ofs};
	bool ok = true;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(fields); i++)
		if (fields[i] < 0) {
			ok = false;
			if (!l->warned)
				Con_Printf("link: the game has no .%s field: %s does not predict\n", need[i], l->client->name);
		}
	if (!l->warned) {
		const char *opt[] = {"m_pmoveBaseVelocity", "m_pmoveHookVelocity", "m_timeSinceJump", "m_timeSinceTeleport",
		                     "m_currentStamina", "m_pmoveSpeedTarget", "viewzoom", "m_fiSpeedMod", "m_pmoveVars",
		                     "g_pmoveVars"};
		int ofs[] = {pf.basevel, pf.hookvel, pf.tsj, pf.tst, pf.stamina, pf.speedtarget, pf.viewzoom, pf.wsm,
		             pf.pmvars, pf.g_pmovevars};
		for (int i = 0; i < (int)BW_ARRAY_COUNT(ofs); i++)
			if (ofs[i] < 0)
				Con_Printf("link: the game has no %s (read as 0)\n", opt[i]);
		l->warned = true;
	}
	if (!ok || !l->v2 || !l->varsonce || s->health <= 0 || s->movetype == 0)
		return false;
	if (s->vflags & (VFL_FAKESPEC | VFL_INVEHICLE))
		return false;
	if (QC_EInt(sv.vm, e, F.hitcontentsmaski))
		return false;
	if (pf.g_pmovevars < 0 || !QC_GInt(sv.vm, pf.g_pmovevars) || pf.pmvars < 0 || !QC_EInt(sv.vm, e, pf.pmvars))
		return false;
	/* SV_RunClientCommand's early returns (Nuclide entry.qc) */
	if (pf.isplayer >= 0 && QC_EFloat(sv.vm, e, pf.isplayer) != 1)
		return false;
	if (pf.g_maploaded >= 0 && !QC_GFloat(sv.vm, pf.g_maploaded))
		return false;
	if (pf.g_menubg >= 0 && QC_GFloat(sv.vm, pf.g_menubg))
		return false;
	return true;
}

/* --- PMOVEVARS --- */

/* g_pmoveVars' values, then the player's m_pmoveVars'; an entity of 0 reads
 * the world's fields, as QC does. Without g_pmoveVars, the cvars */
static void read_vars(int e, float *out)
{
	int g = pf.g_pmovevars >= 0 ? QC_GInt(sv.vm, pf.g_pmovevars) : 0;
	int p = pf.pmvars >= 0 ? QC_EInt(sv.vm, e, pf.pmvars) : 0;
	if (!ent_live(g))
		g = 0;
	if (!ent_live(p))
		p = 0;
	for (int i = 0; i < PMV_COUNT; i++) {
		if (pf.g_pmovevars < 0)
			out[i] = out[PMV_COUNT + i] = Cvar_Value(pmv_names[i]);
		else
			out[i] = QC_EFloat(sv.vm, g, pf.var[i]), out[PMV_COUNT + i] = QC_EFloat(sv.vm, p, pf.var[i]);
	}
}

static void update_vars(link_t *l, int e)
{
	float v[2 * PMV_COUNT];
	read_vars(e, v);
	if (l->varsread && !memcmp(v, l->vars, sizeof(v)))
		return;
	memcpy(l->vars, v, sizeof(v));
	l->varsread = true;
	if (!++l->varsver)
		l->varsver = 1;
}

static void p_f32(pkt_t *p, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	p_u32(p, u);
}

static void send_pmovevars(link_t *l, double now)
{
	pkt_t p;
	p_begin(&p, L_PMOVEVARS);
	p_u32(&p, l->session);
	p_u16(&p, (unsigned)svs.spawncount);
	p_u16(&p, l->varsver);
	p_u8(&p, PMV_COUNT);
	p_u8(&p, 0);
	for (int i = 0; i < 2 * PMV_COUNT; i++)
		p_f32(&p, l->vars[i]);
	send_to(&l->from, &p);
	l->varssent = l->varsver;
	l->varsonce = true;
	l->lastvars = now;
	l->nextvars = now + PM_PERIOD;
}

/* --- BRUSHINFO --- */

/* every brush entity SV_Move would test for the player, by edict */
static int build_brushes(int self, lbrush_t *out)
{
	int n = 0;
	for (int e = 1; e < sv.vm->num_edicts && n < MAX_LBRUSH; e++) {
		int solid, sm, skin;
		if (e == self || sv.vm->edicts[e].free)
			continue;
		solid = (int)QC_EFloat(sv.vm, e, F.solid);
		if ((solid != SOLID_BSP && solid != SOLID_BSPTRIGGER) || (sm = ent_submodel(e)) < 1 || !ent_has_bsp(e) ||
		    !ent_tested(e, self))
			continue;
		skin = F.skin >= 0 ? (int)QC_EFloat(sv.vm, e, F.skin) : 0;
		out[n].e = (uint16_t)e;
		out[n].model = (uint16_t)sm;
		out[n].solid = (uint8_t)solid;
		out[n].skin = (int8_t)(skin < 0 && BSP_QuakeContentsToBits(skin) ? skin : 0);
		memcpy(out[n].org, fvec(e, F.origin), sizeof(float) * 3);
		n++;
	}
	return n;
}

static bool at_rest(int e)
{
	const float *v = fvec(e, F.velocity);
	return !v[0] && !v[1] && !v[2];
}

/* a list differs from what entities say now: an entry added, gone or
 * changed, or a brush at rest somewhere else */
static bool brushes_differ(const lbrush_t *a, int na, const lbrush_t *b, int nb)
{
	if (na != nb)
		return true;
	for (int i = 0; i < na; i++) {
		if (a[i].e != b[i].e || a[i].model != b[i].model || a[i].solid != b[i].solid || a[i].skin != b[i].skin)
			return true;
		if (at_rest(b[i].e) && memcmp(a[i].org, b[i].org, sizeof(a[i].org)))
			return true;
	}
	return false;
}

static void update_brushes(link_t *l, int e)
{
	static lbrush_t now[MAX_LBRUSH];
	int n = build_brushes(e, now);
	if (l->brushread && !brushes_differ(l->brush, l->numbrush, now, n))
		return;
	memcpy(l->brush, now, sizeof(lbrush_t) * (size_t)n);
	l->numbrush = n;
	l->brushread = true;
	if (!++l->brushver)
		l->brushver = 1;
}

static void send_brushinfo(link_t *l, double now)
{
	int first = 0;
	do {
		pkt_t p;
		int count = BW_MIN(l->numbrush - first, BRUSH_PAGE);
		p_begin(&p, L_BRUSHINFO);
		p_u32(&p, l->session);
		p_u16(&p, (unsigned)svs.spawncount);
		p_u16(&p, l->brushver);
		p_u16(&p, (unsigned)l->numbrush);
		p_u16(&p, (unsigned)first);
		p_u8(&p, (unsigned)count);
		for (int i = first; i < first + count; i++) {
			const lbrush_t *b = &l->brush[i];
			p_u16(&p, b->e);
			p_u16(&p, b->model);
			p_u8(&p, b->solid);
			p_u8(&p, (uint8_t)b->skin);
			for (int k = 0; k < 3; k++)
				p_f32(&p, b->org[k]);
		}
		send_to(&l->from, &p);
		first += count;
	} while (first < l->numbrush);
	l->brushsent = l->brushver;
	l->lastbrush = now;
	l->nextbrush = now + PM_PERIOD;
}

/* --- weapons (link v2): WEAPONINFO, and PSTATE's weapon block ---------------------------
 * What the device needs to run the game's weapon code itself, as the PC client
 * does (Nuclide's ncWeapon, src/shared/game/Weapon.qc, predicted from
 * ncPlayer::Physics_InputPostMove): the weapon's decl values as Weapon.qc
 * reads them (ncIO::GetSubDefString and friends, called here), the view
 * model's sequences, and the player's and weapon's state after each command. */

#define WI_MAX_SEQ 64   /* view model sequences sent */
#define WI_MAX_EV 32    /* muzzle flash events sent, all sequences */
#define WI_RESEND 0.25  /* WEAPONINFO again this soon while the device has not acknowledged it */
#define WI_CHECK 1.0    /* and the decl read again this often */
#define WI_BACKOFF 8    /* after this many sends answered by INPUT without the ack, WI_SLOW apart (a device that can't parse it) */
#define WI_SLOW 2.0
#define WI_MAX_ACTS 32  /* an activity's list as sent: GetSubDefAct picks among all of it, so a
                         * device that can hold fewer leaves the weapon to the server */
#define WI_AMMO 4

/* Weapon.qc's activities, in WEAPONINFO's numbering (docs/spec/link.md) */
static const char *const wa_names[] = {"fire", "fireLast", "fireFailed", "fireStart", "fireStop", "reload",
                                       "reloadEmpty", "reloadStart", "reloadEnd", "draw", "drawEmpty", "idle",
                                       "idleEmpty", "cock", "loop", "delay", "release", "cooking", "detonate",
                                       "modeOn", "modeOff"};
#define WA_COUNT ((int)BW_ARRAY_COUNT(wa_names))

/* a field of the game's with its type: Nuclide declares some int (NETWORKED_INT) */
typedef struct {
	int ofs;
	bool isint;
} qfld_t;

static qfld_t qfld(const char *name)
{
	qcdef_t *d = QC_FindField(sv.vm, name);
	qfld_t f = {-1, false};
	if (d) {
		unsigned t = d->type & 0x7fff;
		f.ofs = (int)d->ofs, f.isint = t == ev_integer || t == ev_uint;
	}
	return f;
}

static float qnum(int e, qfld_t f)
{
	const qcval_t *v;
	if (f.ofs < 0 || e <= 0)
		return 0;
	v = &QC_EdictFields(sv.vm, e)[f.ofs];
	return f.isint ? (float)v->i : v->f;
}

static const char *qstr(int e, qfld_t f)
{
	string_t s = f.ofs >= 0 && e > 0 ? QC_EdictFields(sv.vm, e)[f.ofs].i : 0;
	const char *r = s ? QC_GetString(sv.vm, s) : NULL;
	return r ? r : "";
}

/* the fields and functions this reads, looked up once a map */
static struct {
	const qcvm_t *vm;
	int spawncount;
	qfld_t seq, animtime, atk, idle, rel, punch, punchvel, ammo[16];
	qfld_t clip, clipsize, state, fiv, burst, firing, willrelease, overheat, overheatlen, reloadstep, spin;
	qfld_t lastfi, primfi, secfi, viewmodel, primammo, secammo;
	int subdefstring, subdefact, unpack, ammonum, dictfloat; /* functions */
	int falldecl, infinite;                                  /* globals */
} wf;

static void w_fields(void)
{
	char name[64];
	if (wf.vm == sv.vm && wf.spawncount == svs.spawncount)
		return;
	wf.vm = sv.vm, wf.spawncount = svs.spawncount;
	wf.seq = qfld("ncPlayer::__mm_weaponAnimSequence");
	wf.animtime = qfld("ncPlayer::__mm_weaponAnimTime");
	wf.atk = qfld("ncPlayer::__mm_timeUntilNextAttack");
	wf.idle = qfld("ncPlayer::__mm_timeUntilNextIdle");
	wf.rel = qfld("ncPlayer::__mm_timeUntilReloaded");
	wf.punch = qfld("ncPlayer::__mm_punchAngle");
	wf.punchvel = qfld("ncPlayer::__mm_punchVelocity");
	for (int i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "ncActor::__mm_ammoTypes[%d]", i);
		wf.ammo[i] = qfld(name);
	}
	wf.clip = qfld("ncWeapon::__mm_weaponClip");
	wf.clipsize = qfld("ncWeapon::__mm_weaponClipSize");
	wf.state = qfld("ncWeapon::__mm_weaponState");
	wf.fiv = qfld("ncWeapon::__mm_weaponFireInfoValue");
	wf.burst = qfld("ncWeapon::__mm_burstCount");
	wf.firing = qfld("ncWeapon::__mm_weaponIsFiring");
	wf.willrelease = qfld("ncWeapon::__mm_fiWillRelease");
	wf.overheat = qfld("ncWeapon::__mm_weaponOverheating");
	wf.overheatlen = qfld("ncWeapon::__mm_fiOverheatLength");
	wf.reloadstep = qfld("ncWeapon::__mm_reloadStepLength");
	wf.spin = qfld("ncWeapon::__mm_weaponSpin");
	wf.lastfi = qfld("ncWeapon::__mm_weaponLastFireInfo");
	wf.primfi = qfld("ncWeapon::__mm_primaryFireInfo");
	wf.secfi = qfld("ncWeapon::__mm_secondaryFireInfo");
	wf.viewmodel = qfld("ncWeapon::__mm_viewModel");
	wf.primammo = qfld("ncWeapon::__mm_primaryAmmoType");
	wf.secammo = qfld("ncWeapon::__mm_secondaryAmmoType");
	wf.subdefstring = QC_FindFunction(sv.vm, "ncIO::GetSubDefString");
	wf.subdefact = QC_FindFunction(sv.vm, "ncIO::GetSubDefAct");
	wf.unpack = QC_FindFunction(sv.vm, "unpackStringCommand");
	wf.ammonum = QC_FindFunction(sv.vm, "ammoNumForName");
	wf.dictfloat = QC_FindFunction(sv.vm, "ncDict::GetFloat");
	wf.falldecl = QC_GlobalOfs(sv.vm, "g_fallDamageDecl");
	wf.infinite = QC_GlobalOfs(sv.vm, "autocvar_g_infiniteAmmo");
}

/* the game can predict a weapon on the device: it has what Weapon.qc keeps */
static bool w_usable(void)
{
	return wf.seq.ofs >= 0 && wf.animtime.ofs >= 0 && wf.atk.ofs >= 0 && wf.idle.ofs >= 0 && wf.rel.ofs >= 0 &&
	       wf.clip.ofs >= 0 && wf.state.ofs >= 0 && wf.lastfi.ofs >= 0 && wf.primfi.ofs >= 0 &&
	       wf.viewmodel.ofs >= 0 && wf.subdefstring > 0 && wf.subdefact > 0 && wf.unpack > 0 && wf.ammonum > 0;
}

/* a QC function run with self and up to two strings; its return value */
static qcval_t qc_call(int fn, int self, int argc, const char *a0, const char *a1)
{
	qcvm_t *vm = sv.vm;
	int32_t oself = QC_GInt(vm, vm->gofs.self);
	qcval_t r;
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGFloat(vm, vm->gofs.time, (float)sv.time);
	if (argc > 0)
		vm->globals[OFS_PARM(0)].i = QC_NewString(vm, a0);
	if (argc > 1)
		vm->globals[OFS_PARM(1)].i = QC_NewString(vm, a1);
	vm->argc = argc;
	r.i = 0;
	if (fn > 0 && QC_Execute(vm, fn))
		r = vm->globals[OFS_RETURN];
	QC_SetGInt(vm, vm->gofs.self, oself);
	return r;
}

/* ncIO::GetSubDefString(subDef, key) on the weapon: the fireInfo's value, else the weapon decl's */
static void w_subdef(int w, const char *fi, const char *key, char *out, size_t size)
{
	qcval_t r = qc_call(wf.subdefstring, w, 2, fi, key);
	const char *s = r.i ? QC_GetString(sv.vm, r.i) : NULL;
	snprintf(out, size, "%s", s ? s : "");
}

/* QC's stof, stoi and stov */
static float w_stof(const char *s) { return (float)strtod(s, NULL); }
static int w_stoi(const char *s) { return (int)strtol(s, NULL, 0); }
static void w_stov(const char *s, float *v)
{
	char *end;
	for (int k = 0; k < 3; k++, s = end) {
		while (*s == '\'' || *s == ' ')
			s++;
		v[k] = (float)strtod(s, &end);
	}
}

static float w_subf(int w, const char *fi, const char *key)
{
	char v[256];
	w_subdef(w, fi, key, v, sizeof(v));
	return w_stof(v);
}

/* an activity's sequences as GetSubDefAct picks among them (activities.qc): the
 * decl's comma list, each through unpackStringCommand, or the one value
 * GetSubDefAct finds elsewhere */
static int w_acts(int w, const char *fi, const char *act, int16_t *out, int max)
{
	char key[40], raw[256];
	int n = 0;
	snprintf(key, sizeof(key), "act_%s", act);
	w_subdef(w, fi, key, raw, sizeof(raw));
	if (!raw[0]) {
		float v = qc_call(wf.subdefact, w, 2, fi, act).f;
		if (v != -1)
			out[n++] = (int16_t)v;
		return n;
	}
	for (char *tok = raw, *next; tok && n < max; tok = next) {
		qcval_t r;
		const char *s;
		if ((next = strchr(tok, ',')))
			*next++ = 0;
		r = qc_call(wf.unpack, 0, 1, tok, NULL);
		s = r.i ? QC_GetString(sv.vm, r.i) : NULL;
		out[n++] = (int16_t)w_stof(s ? s : "");
	}
	return n;
}

/* one fireInfo, as ncWeapon::UpdateFireInfoCache (Weapon.qc:126-256) reads it */
static int w_fireinfo(pkt_t *p, int w, const char *fi)
{
	char v[256], onfire[256];
	float f[25], vec[3];
	unsigned flags = 0;
	int16_t acts[WI_MAX_ACTS];
	int nacts = 0, at;
	if (w_subf(w, fi, "ammoRequired") != 0)
		flags |= 1;
	if (w_subf(w, fi, "semiAuto") != 0)
		flags |= 2;
	if (w_subf(w, fi, "fireUnderwater") != 0)
		flags |= 4;
	if (w_subf(w, fi, "act_loop") != 0)
		flags |= 8;
	if (w_subf(w, fi, "cocks") != 0)
		flags |= 16;
	if (w_subf(w, fi, "drawAfterRelease") != 0)
		flags |= 32;
	if (w_subf(w, fi, "reloadTimed") != 0)
		flags |= 64;
	w_subdef(w, fi, "def_onFire", onfire, sizeof(onfire));
	if (onfire[0])
		flags |= 128;
	w_subdef(w, fi, "def_onRelease", v, sizeof(v));
	if (v[0])
		flags |= 256;
	w_subdef(w, fi, "detonateOnFire", v, sizeof(v));
	if (v[0])
		flags |= 512;
	if (w_subf(w, fi, "serverFire") != 0)
		flags |= 1024;
	p_u16(p, flags);
	w_subdef(w, fi, "ammoType", v, sizeof(v));
	at = wf.ammonum > 0 ? qc_call(wf.ammonum, 0, 1, v, NULL).i : -1;
	p_u8(p, (uint8_t)(int8_t)BW_CLAMP(at, -1, 15));
	w_subdef(w, fi, "ammoPerShot", v, sizeof(v));
	p_u16(p, (uint16_t)(int16_t)(v[0] ? w_stoi(v) : 1));
	w_subdef(w, fi, "depleteAmmo", v, sizeof(v));
	p_u16(p, (uint16_t)(int16_t)w_stoi(v));
	w_subdef(w, fi, "punchAngle", v, sizeof(v));
	w_stov(v, vec);
	f[0] = vec[0], f[1] = vec[1], f[2] = vec[2];
	w_subdef(w, fi, "punchSpring", v, sizeof(v));
	w_stov(v, vec);
	f[3] = vec[0], f[4] = vec[1], f[5] = vec[2];
	f[6] = w_subf(w, fi, "punchKick");
	f[7] = w_subf(w, fi, "fireRate");
	f[8] = w_subf(w, fi, "chargeTime");
	f[9] = w_subf(w, fi, "overheatPerShot");
	f[10] = w_subf(w, fi, "overheatLength");
	f[11] = w_subf(w, fi, "burstCount");
	f[12] = w_subf(w, fi, "burstTime");
	f[13] = w_subf(w, fi, "knockback");
	f[14] = w_subf(w, fi, "knockbackRelease");
	f[15] = w_subf(w, fi, "testDistance");
	f[16] = w_subf(w, fi, "spinUpTime");
	w_subdef(w, fi, "reloadTime", v, sizeof(v));
	f[17] = v[0] ? w_stof(v) : -1.0f; /* -1: from the animation */
	f[18] = w_subf(w, fi, "reloadStartTime");
	f[19] = w_subf(w, fi, "zoomFov") / 90.0f;
	f[20] = w_subf(w, fi, "failRate");
	/* a melee sweep's cone and reach: its def_onFire's sweepAngle and range (TestFireAbility) */
	f[21] = f[22] = 0;
	if (onfire[0]) {
		static const char *const keys[2] = {"sweepAngle", "range"};
		int fn = QC_FindFunction(sv.vm, "EntityDef_GetKeyValue");
		for (int k = 0; k < 2 && fn > 0; k++) {
			qcval_t r = qc_call(fn, 0, 2, onfire, keys[k]);
			const char *s = r.i ? QC_GetString(sv.vm, r.i) : NULL;
			f[21 + k] = w_stof(s ? s : "");
		}
	}
	f[23] = w_subf(w, fi, "regenRate");
	f[24] = w_subf(w, fi, "depleteRate");
	for (int i = 0; i < 25; i++)
		p_f32(p, f[i]);
	for (int a = 0; a < WA_COUNT; a++)
		nacts += w_acts(w, fi, wa_names[a], acts, WI_MAX_ACTS) > 0;
	p_u8(p, (unsigned)nacts);
	for (int a = 0; a < WA_COUNT; a++) {
		int n = w_acts(w, fi, wa_names[a], acts, WI_MAX_ACTS);
		if (!n)
			continue;
		p_u8(p, (unsigned)a);
		p_u8(p, (unsigned)n);
		for (int i = 0; i < n; i++)
			p_u16(p, (uint16_t)acts[i]);
	}
	return at;
}

/* ncDict::GetFloat on the game's falldamage decl (Physics_Fall's punch) */
static float w_fall(const char *key)
{
	int d = QC_GInt(sv.vm, wf.falldecl);
	return wf.falldecl >= 0 && d > 0 && wf.dictfloat > 0 ? qc_call(wf.dictfloat, d, 1, key, NULL).f : 0;
}

/* WEAPONINFO's body (after the version): what the device needs to run weapon w */
static void w_build(pkt_t *p, int w, int *fiammo)
{
	static const char *const wkeys[] = {"altAlternates", "removeOnEmpty", "powerAmmo", "altMode", "buggyIdleAnim"};
	const char *prim = qstr(w, wf.primfi), *sec = qstr(w, wf.secfi);
	const studio_model_t *m = QC_StudioModel(sv.vm, (int)qnum(w, wf.viewmodel));
	char v[256];
	unsigned flags = 0;
	int ng = 0;
	/* a view model that isn't a studio model (a Quake .mdl with a .framegroups file, a
	 * skeletal one): frameduration() times its frame groups, count / fps (sv_builtins.c) */
	const framegroup_t *g = m ? NULL : SV_ModelFramegroups((int)qnum(w, wf.viewmodel), &ng);
	int nseq = m ? BW_MIN(m->numseqs, WI_MAX_SEQ) : g ? BW_MIN(ng, WI_MAX_SEQ) : 0, nev = 0;
	/* the weapon decl's own keys (ReloadCachedAttributes): GetSubDefString with no
	 * fireInfo falls back to GetDefString */
	for (int i = 0; i < 5; i++) {
		w_subdef(w, "", wkeys[i], v, sizeof(v));
		if (w_stof(v) != 0)
			flags |= 1u << i;
	}
	if (sec[0])
		flags |= 32;
	p_u16(p, (unsigned)qnum(w, wf.viewmodel));
	p_u8(p, flags);
	p_u8(p, (uint8_t)(int8_t)BW_CLAMP((int)QC_GFloat(sv.vm, wf.infinite), -1, 9));
	p_u8(p, (uint8_t)(int8_t)BW_CLAMP((int)qnum(w, wf.primammo), -1, 15));
	p_u8(p, (uint8_t)(int8_t)BW_CLAMP((int)qnum(w, wf.secammo), -1, 15));
	w_subdef(w, "", "primed_fuse", v, sizeof(v));
	p_f32(p, w_stof(v));
	p_f32(p, w_fall("minFallDistance"));
	p_f32(p, w_fall("minLandDistance"));
	p_f32(p, w_fall("punchAngleScale"));
	p_f32(p, w_fall("punchSpringScale"));
	p_u8(p, sec[0] ? 2 : 1);
	fiammo[0] = w_fireinfo(p, w, prim);
	fiammo[1] = sec[0] ? w_fireinfo(p, w, sec) : -1;
	/* the view model's sequences: how long each lasts (frameduration, as Weapon.qc times
	 * them), its frames and rate, and its muzzle flash events (5001, 5011, 5021, 5031) */
	p_u8(p, (unsigned)nseq);
	for (int i = 0; i < nseq && g; i++) { /* no studio events: the device shows no flash of its own */
		p_f32(p, (float)g[i].count / g[i].fps);
		p_f32(p, g[i].fps);
		p_u16(p, (unsigned)BW_CLAMP(g[i].count, 0, 65535));
		p_u8(p, g[i].loop ? 1 : 0);
		p_u8(p, 0);
	}
	for (int i = 0; i < nseq && m; i++) {
		const studio_seq_t *s = &m->seqs[i];
		int idx[8], n = 0;
		for (int k = 0; k < s->numevents && n < 8 && nev < WI_MAX_EV; k++) {
			int ev = s->events[k].event;
			if (ev == 5001 || ev == 5011 || ev == 5021 || ev == 5031)
				idx[n++] = k, nev++;
		}
		p_f32(p, Studio_SeqDuration(m, i));
		p_f32(p, s->fps);
		p_u16(p, (unsigned)BW_CLAMP(s->numframes, 0, 65535));
		p_u8(p, (s->flags & STUDIO_LOOPING) ? 1 : 0);
		p_u8(p, (unsigned)n);
		for (int k = 0; k < n; k++) {
			const studio_event_t *e = &s->events[idx[k]];
			p_u16(p, (unsigned)BW_CLAMP(e->frame, 0, 65535));
			p_u16(p, (unsigned)e->event);
			p_u8(p, (unsigned)BW_CLAMP(w_stoi(e->options), 0, 255));
		}
	}
}

/* the weapon's info again when the weapon changed or WI_CHECK went by; a new version
 * when anything in it differs */
static void update_winfo(link_t *l, int e, double now)
{
	int w = active_weapon(e);
	pkt_t p;
	int fiammo[2];
	int32_t cls = w > 0 && F.classname >= 0 ? QC_EInt(sv.vm, w, F.classname) : 0;
	/* the same edict can be another weapon within WI_CHECK (an edict is reused 0.5 s
	 * after it is freed): its classname says so */
	if (w == l->wiedict && cls == l->wiclass && now < l->wicheck)
		return;
	l->wicheck = now + WI_CHECK;
	l->wiclass = cls;
	if (!w || !w_usable()) {
		l->wiedict = (uint16_t)w;
		l->wiver = 0;
		return;
	}
	p.len = 0;
	w_build(&p, w, fiammo);
	if (p.len > (int)sizeof(l->wibody)) { /* doesn't fit one packet: the device shows the server's */
		l->wiedict = (uint16_t)w;
		l->wiver = 0;
		return;
	}
	l->wiammo[0] = (int8_t)fiammo[0], l->wiammo[1] = (int8_t)fiammo[1];
	if (w == l->wiedict && l->wiver && p.len == l->wilen && !memcmp(p.data, l->wibody, (size_t)p.len))
		return;
	memcpy(l->wibody, p.data, (size_t)p.len);
	l->wilen = p.len;
	l->wiedict = (uint16_t)w;
	if (!++l->wivers)
		l->wivers = 1;
	l->wiver = l->wivers;
	l->wisent = 0;
	l->wisends = 0;
}

static void send_winfo(link_t *l, double now)
{
	pkt_t p;
	p_begin(&p, L_WEAPONINFO);
	p_u32(&p, l->session);
	p_u16(&p, (unsigned)svs.spawncount);
	p_u16(&p, l->wiedict);
	p_u16(&p, l->wiver);
	p_bytes(&p, l->wibody, l->wilen);
	send_to(&l->from, &p);
	l->wisent = now;
	if (l->wiheard && l->wisends < 255)
		l->wisends++;
	l->wiheard = false;
}

/* the weapon state Weapon.qc and Physics_InputPostMove keep: the player's and its weapon's */
typedef struct {
	int w, fi, state, flags, clip, clipsize, animseq, nammo;
	float burst, fiv, overheat, overheatlen, reloadstep, spin, atk, idle, rel, animtime, firescale;
	float punch[3], punchvel[3];
	int ammotype[WI_AMMO], ammo[WI_AMMO];
} lwstate_t;

static void read_wstate(const link_t *l, int e, lwstate_t *s)
{
	int w = active_weapon(e), types[4], nt = 0;
	char fs[32] = "";
	memset(s, 0, sizeof(*s));
	s->w = w;
	s->animseq = (int)qnum(e, wf.seq);
	s->animtime = qnum(e, wf.animtime);
	s->atk = qnum(e, wf.atk), s->idle = qnum(e, wf.idle), s->rel = qnum(e, wf.rel);
	if (wf.punch.ofs >= 0)
		memcpy(s->punch, fvec(e, wf.punch.ofs), sizeof(s->punch));
	if (wf.punchvel.ofs >= 0)
		memcpy(s->punchvel, fvec(e, wf.punchvel.ofs), sizeof(s->punchvel));
	SV_InfoGet(l->client->userinfo, "*fireScale", fs, sizeof(fs)); /* ncWeapon::_FireScale */
	s->firescale = fs[0] && w_stof(fs) > 0 ? w_stof(fs) : 1.0f;
	s->fi = 255;
	if (!w)
		return;
	{
		const char *last = qstr(w, wf.lastfi);
		s->fi = !strcmp(last, qstr(w, wf.primfi)) ? 0 : last[0] && !strcmp(last, qstr(w, wf.secfi)) ? 1 : 255;
	}
	s->state = (int)qnum(w, wf.state);
	s->flags = (qnum(w, wf.firing) != 0 ? 1 : 0) | (qnum(w, wf.willrelease) != 0 ? 2 : 0);
	s->clip = (int)qnum(w, wf.clip), s->clipsize = (int)qnum(w, wf.clipsize);
	s->burst = qnum(w, wf.burst), s->fiv = qnum(w, wf.fiv);
	s->overheat = qnum(w, wf.overheat), s->overheatlen = qnum(w, wf.overheatlen);
	s->reloadstep = qnum(w, wf.reloadstep), s->spin = qnum(w, wf.spin);
	/* the ammo the weapon uses: its two types, and its fireInfos' (WEAPONINFO) */
	types[nt++] = (int)qnum(w, wf.primammo);
	types[nt++] = (int)qnum(w, wf.secammo);
	if (w == l->wiedict && l->wiver)
		types[nt++] = l->wiammo[0], types[nt++] = l->wiammo[1];
	for (int i = 0; i < nt && s->nammo < WI_AMMO; i++) {
		bool dup = types[i] < 0 || types[i] > 15;
		for (int j = 0; j < s->nammo && !dup; j++)
			dup = s->ammotype[j] == types[i];
		if (!dup)
			s->ammotype[s->nammo] = types[i], s->ammo[s->nammo++] = (int)qnum(e, wf.ammo[types[i]]);
	}
}

/* PSTATE's weapon block (docs/spec/link.md) */
static void p_wstate(pkt_t *p, const link_t *l, const lwstate_t *s)
{
	p_u8(p, 'W');
	p_u8(p, 1);
	p_u16(p, (unsigned)s->w);
	p_u16(p, s->w && s->w == l->wiedict ? l->wiver : 0);
	p_u8(p, (unsigned)s->state);
	p_u8(p, (unsigned)s->fi);
	p_u8(p, (unsigned)s->flags);
	p_u8(p, (unsigned)s->nammo);
	p_u16(p, (uint16_t)(int16_t)BW_CLAMP(s->clip, -32768, 32767));
	p_u16(p, (uint16_t)(int16_t)BW_CLAMP(s->clipsize, -32768, 32767));
	p_f32(p, s->burst);
	p_f32(p, s->fiv);
	p_f32(p, s->overheat);
	p_f32(p, s->overheatlen);
	p_f32(p, s->reloadstep);
	p_f32(p, s->spin);
	p_f32(p, s->atk);
	p_f32(p, s->idle);
	p_f32(p, s->rel);
	p_u16(p, (uint16_t)(int16_t)BW_CLAMP(s->animseq, -32768, 32767));
	p_f32(p, s->animtime);
	for (int k = 0; k < 3; k++)
		p_f32(p, s->punch[k]);
	for (int k = 0; k < 3; k++)
		p_f32(p, s->punchvel[k]);
	p_f32(p, s->firescale);
	for (int i = 0; i < s->nammo; i++) {
		p_u8(p, (unsigned)s->ammotype[i]);
		p_u16(p, (uint16_t)(int16_t)BW_CLAMP(s->ammo[i], -32768, 32767));
	}
}

/* --- PSTATE --- */

typedef struct {
	int e;
	float d2;
	float mins[3], maxs[3];
	int solid;
} lbox_t;

static int cmp_lbox_d(const void *a, const void *b)
{
	const lbox_t *x = a, *y = b;
	return x->d2 < y->d2 ? -1 : x->d2 > y->d2 ? 1 : x->e - y->e;
}

static int cmp_lbox_e(const void *a, const void *b) { return ((const lbox_t *)a)->e - ((const lbox_t *)b)->e; }

/* how far apart two boxes are along each axis, 0 where they overlap */
static float box_gap2(const float *amin, const float *amax, const float *bmin, const float *bmax, float range)
{
	float d2 = 0;
	for (int k = 0; k < 3; k++) {
		float g = BW_MAX(BW_MAX(bmin[k] - amax[k], amin[k] - bmax[k]), 0.0f);
		if (g > range)
			return -1;
		d2 += g * g;
	}
	return d2;
}

/* the boxes SV_Move tests in a MOVE_NORMAL trace for the player: players,
 * monsters and SOLID_BSP entities without a brush model, as origin + mins ..
 * origin + maxs; the nearest max within BOX_RANGE, by edict */
static int collect_boxes(int self, lbox_t *out, int max)
{
	static lbox_t cand[MAX_EDICTS_LINK];
	float pmin[3], pmax[3];
	const float *po = fvec(self, F.origin), *pmn = fvec(self, F.mins), *pmx = fvec(self, F.maxs);
	int n = 0, mask = move_mask(self);
	for (int k = 0; k < 3; k++)
		pmin[k] = po[k] + pmn[k], pmax[k] = po[k] + pmx[k];
	for (int e = 1; e < sv.vm->num_edicts && n < MAX_EDICTS_LINK; e++) {
		int solid, contents;
		const float *o, *mn, *mx;
		lbox_t *b = &cand[n];
		if (e == self || sv.vm->edicts[e].free)
			continue;
		solid = (int)QC_EFloat(sv.vm, e, F.solid);
		if (solid == SOLID_BBOX || solid == SOLID_SLIDEBOX)
			contents = CONTENTS_BODY;
		else if (solid == SOLID_BSP && !ent_has_bsp(e))
			contents = CONTENTS_SOLID;
		else
			continue;
		if (!(contents & mask) || !ent_tested(e, self))
			continue;
		o = fvec(e, F.origin), mn = fvec(e, F.mins), mx = fvec(e, F.maxs);
		for (int k = 0; k < 3; k++)
			b->mins[k] = o[k] + mn[k], b->maxs[k] = o[k] + mx[k];
		if ((b->d2 = box_gap2(pmin, pmax, b->mins, b->maxs, BOX_RANGE)) < 0)
			continue;
		b->e = e, b->solid = solid;
		n++;
	}
	qsort(cand, (size_t)n, sizeof(*cand), cmp_lbox_d);
	n = BW_MIN(n, max);
	memcpy(out, cand, sizeof(*cand) * (size_t)n);
	qsort(out, (size_t)n, sizeof(*out), cmp_lbox_e);
	return n;
}

/* brush entities of the BRUSHINFO list near the player that move or have
 * moved since: the ground entity first, then the nearest */
static int collect_movers(const link_t *l, int self, int ground, int *out)
{
	lbox_t cand[MAX_LBRUSH];
	const float *pmin = fvec(self, F.absmin), *pmax = fvec(self, F.absmax);
	int n = 0, m = 0;
	for (int i = 0; i < l->numbrush; i++) {
		const lbrush_t *b = &l->brush[i];
		float d2;
		if (!ent_live(b->e) || (at_rest(b->e) && !memcmp(fvec(b->e, F.origin), b->org, sizeof(b->org))))
			continue;
		if ((d2 = box_gap2(pmin, pmax, fvec(b->e, F.absmin), fvec(b->e, F.absmax), MOVER_RANGE)) < 0)
			continue;
		cand[n].e = b->e, cand[n].d2 = b->e == ground ? -1 : d2;
		n++;
	}
	qsort(cand, (size_t)n, sizeof(*cand), cmp_lbox_d);
	for (int i = 0; i < n && m < MAX_MOVERS; i++)
		out[m++] = cand[i].e;
	return m;
}

static void send_pstate(link_t *l)
{
	pkt_t p;
	lstate_t s;
	lbox_t boxes[MAX_BOXES];
	int movers[MAX_MOVERS], e = l->client->edict, nmovers, nboxes, status;
	read_state(l, e, &s);
	status = (can_predict(l, e, &s) ? PS_PREDICT : 0) | (int)(s.rules << 2);
	nmovers = collect_movers(l, e, s.groundent, movers);
	nboxes = collect_boxes(e, boxes, MAX_BOXES);
	p_begin(&p, L_PSTATE);
	p_u32(&p, l->session);
	p_u32(&p, l->lastrun);
	p_u16(&p, ++l->pseq);
	p_u8(&p, (unsigned)status);
	p_u8(&p, l->clipped);
	p_u16(&p, l->gaps);
	p_u16(&p, (unsigned)svs.spawncount);
	p_u16(&p, l->varsver);
	p_u16(&p, l->brushver);
	p_u8(&p, l->fixseq);
	p_u8(&p, (unsigned)nmovers);
	p_u16(&p, (uint16_t)l->fixpitch);
	p_u16(&p, (uint16_t)l->fixyaw);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.origin[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.velocity[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.basevel[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.hookvel[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.view_ofs[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.mins[k]);
	for (int k = 0; k < 3; k++)
		p_f32(&p, s.maxs[k]);
	p_u32(&p, s.flags);
	p_u32(&p, s.vflags);
	p_u32(&p, s.gflags);
	p_u8(&p, (unsigned)s.movetype);
	p_u8(&p, (unsigned)s.solid);
	p_u8(&p, (uint8_t)(int8_t)s.waterlevel);
	p_u8(&p, (uint8_t)(int8_t)s.watertype);
	p_u16(&p, (unsigned)s.groundent);
	p_f32(&p, s.gravity);
	p_f32(&p, s.friction);
	p_f32(&p, s.maxspeed);
	p_f32(&p, s.tsj);
	p_f32(&p, s.tst);
	p_f32(&p, s.stamina);
	p_f32(&p, s.speedtarget);
	p_f32(&p, s.speedmod);
	p_f32(&p, s.wsm);
	p_f32(&p, s.wsmfire);
	p_f32(&p, s.wsmzoom);
	p_f32(&p, s.viewzoom);
	p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(s.health, -32768, 32767));
	p_u8(&p, (unsigned)nboxes);
	p_u8(&p, 0);
	for (int i = 0; i < nmovers; i++) {
		const float *o = fvec(movers[i], F.origin), *v = fvec(movers[i], F.velocity);
		p_u16(&p, (unsigned)movers[i]);
		for (int k = 0; k < 3; k++)
			p_f32(&p, o[k]);
		for (int k = 0; k < 3; k++)
			p_f32(&p, v[k]);
	}
	for (int i = 0; i < nboxes; i++) {
		p_u16(&p, (unsigned)boxes[i].e);
		for (int k = 0; k < 3; k++)
			p_f32(&p, boxes[i].mins[k]);
		for (int k = 0; k < 3; k++)
			p_f32(&p, boxes[i].maxs[k]);
		p_u8(&p, (unsigned)boxes[i].solid);
	}
	{
		lwstate_t ws;
		w_fields();
		read_wstate(l, e, &ws);
		p_wstate(&p, l, &ws);
	}
	send_to(&l->from, &p);
	l->pstate_ack = l->lastrun;
	l->lastpstate = Sys_Seconds();
}

/* once a send pass, to a v2 device that has the map; view: a VIEW went out */
static void send_v2(link_t *l, double now, bool view)
{
	int e = l->client->edict;
	v2_sync(l);
	update_vars(l, e);
	if (!l->brushread || now >= l->brushcheck) {
		update_brushes(l, e);
		l->brushcheck = now + BRUSH_CHECK;
	}
	if (l->varssent != l->varsver || !l->varsonce || now >= l->nextvars ||
	    (l->varsack != l->varsver && now >= l->lastvars + PM_RESEND))
		send_pmovevars(l, now);
	if (l->brushsent != l->brushver || now >= l->nextbrush || (l->brushack != l->brushver && now >= l->lastbrush + PM_RESEND))
		send_brushinfo(l, now);
	w_fields();
	update_winfo(l, e, now);
	if (l->wiver && l->wicap && l->wiack != l->wiver &&
	    now >= l->wisent + (l->wisends >= WI_BACKOFF ? WI_SLOW : WI_RESEND))
		send_winfo(l, now);
	/* after each batch of commands (the device's packet rate, at most 30 a
	 * second); with a VIEW only when none ran for a while, so the two rates
	 * don't add up */
	if (l->lastrun != l->pstate_ack || (view && now >= l->lastpstate + PSTATE_IDLE))
		send_pstate(l);
}

/* --- sv_link_log: every command a v2 device's player ran, for tools/nds/pmtest --- */

static void log_flush(strbuf_t *sb)
{
	SB_Append(sb, "\n", 1);
	Con_Print(sb->data);
	sb->len = 0;
}

static void log_state(strbuf_t *sb, const lstate_t *s)
{
	const float *v[7] = {s->origin, s->velocity, s->basevel, s->hookvel, s->view_ofs, s->mins, s->maxs};
	float f[13] = {s->gravity, s->friction, s->maxspeed, s->tsj, s->tst, s->stamina, s->speedtarget, s->speedmod,
	               s->wsm, s->wsmfire, s->wsmzoom, s->viewzoom, s->health};
	for (int i = 0; i < 7; i++)
		SB_Printf(sb, " %a %a %a", (double)v[i][0], (double)v[i][1], (double)v[i][2]);
	SB_Printf(sb, " %u %u %u %d %d %d %d %d", s->flags, s->vflags, s->gflags, s->movetype, s->solid, s->waterlevel,
	          s->watertype, s->groundent);
	for (int i = 0; i < 13; i++)
		SB_Printf(sb, " %a", (double)f[i]);
	SB_Printf(sb, " %u", s->rules);
}

/* map, vars and brush when they changed, then brushpos, boxes and pre */
static void log_before(link_t *l, int e, strbuf_t *sb)
{
	static lbrush_t now[MAX_LBRUSH];
	static lbox_t boxes[MAX_LOGBOXES];
	float v[2 * PMV_COUNT];
	int slot = (int)(l - links), n, moved = 0;
	lstate_t s;
	if (!l->logon) {
		l->logon = true;
		l->logmap = l->logvarsok = l->logbrushok = false;
	}
	if (!l->logmap) {
		SB_Printf(sb, "LINKPM %d map %s %d", slot, sv.mapname, svs.spawncount & 0xffff);
		log_flush(sb);
		l->logmap = true;
	}
	read_vars(e, v);
	if (!l->logvarsok || memcmp(v, l->logvars, sizeof(v))) {
		SB_Printf(sb, "LINKPM %d vars", slot);
		for (int i = 0; i < 2 * PMV_COUNT; i++)
			SB_Printf(sb, " %a", (double)v[i]);
		log_flush(sb);
		memcpy(l->logvars, v, sizeof(v));
		l->logvarsok = true;
	}
	n = build_brushes(e, now);
	if (!l->logbrushok || brushes_differ(l->logbrush, l->numlogbrush, now, n)) {
		SB_Printf(sb, "LINKPM %d brush %d", slot, n);
		for (int i = 0; i < n; i++)
			SB_Printf(sb, " %d %d %d %d %a %a %a", now[i].e, now[i].model, now[i].solid, now[i].skin,
			          (double)now[i].org[0], (double)now[i].org[1], (double)now[i].org[2]);
		log_flush(sb);
		memcpy(l->logbrush, now, sizeof(lbrush_t) * (size_t)n);
		l->numlogbrush = n;
		l->logbrushok = true;
	}
	for (int i = 0; i < l->numlogbrush; i++)
		moved += memcmp(fvec(l->logbrush[i].e, F.origin), l->logbrush[i].org, sizeof(float) * 3) != 0;
	SB_Printf(sb, "LINKPM %d brushpos %d", slot, moved);
	for (int i = 0; i < l->numlogbrush; i++) {
		const float *o = fvec(l->logbrush[i].e, F.origin);
		if (memcmp(o, l->logbrush[i].org, sizeof(float) * 3))
			SB_Printf(sb, " %d %a %a %a", l->logbrush[i].e, (double)o[0], (double)o[1], (double)o[2]);
	}
	log_flush(sb);
	n = collect_boxes(e, boxes, MAX_LOGBOXES);
	SB_Printf(sb, "LINKPM %d boxes %d", slot, n);
	for (int i = 0; i < n; i++)
		SB_Printf(sb, " %d %a %a %a %a %a %a %d", boxes[i].e, (double)boxes[i].mins[0], (double)boxes[i].mins[1],
		          (double)boxes[i].mins[2], (double)boxes[i].maxs[0], (double)boxes[i].maxs[1], (double)boxes[i].maxs[2],
		          boxes[i].solid);
	log_flush(sb);
	read_state(l, e, &s);
	SB_Printf(sb, "LINKPM %d pre", slot);
	log_state(sb, &s);
	log_flush(sb);
}

/* sv_link_log's weapon lines: WEAPONINFO's body when its version changes, and the
 * weapon state before and after each command (tools/nds/weptest) */
static void log_wstate(link_t *l, int e, const char *what, float land, strbuf_t *sb)
{
	lwstate_t s;
	int slot = (int)(l - links);
	w_fields();
	if (l->wiver && l->wiver != l->wilogver) {
		SB_Printf(sb, "LINKPM %d winfo %u %u ", slot, l->wiedict, l->wiver);
		for (int i = 0; i < l->wilen; i++)
			SB_Printf(sb, "%02x", l->wibody[i]);
		log_flush(sb);
		l->wilogver = l->wiver;
	}
	read_wstate(l, e, &s);
	SB_Printf(sb, "LINKPM %d %s %d %u %d %d %d %d %d", slot, what, s.w, s.w && s.w == l->wiedict ? l->wiver : 0, s.state,
	          s.fi, s.flags, s.clip, s.clipsize);
	{
		float f[17] = {s.burst, s.fiv, s.overheat, s.overheatlen, s.reloadstep, s.spin, s.atk, s.idle, s.rel, s.animtime,
		               s.punch[0], s.punch[1], s.punch[2], s.punchvel[0], s.punchvel[1], s.punchvel[2], s.firescale};
		SB_Printf(sb, " %d", s.animseq);
		for (int i = 0; i < 17; i++)
			SB_Printf(sb, " %a", (double)f[i]);
	}
	SB_Printf(sb, " %u %u %d", (unsigned)(int)QC_EFloat(sv.vm, e, pf.vflags), (unsigned)(int)QC_EFloat(sv.vm, e, pf.gflags),
	          s.nammo);
	for (int i = 0; i < s.nammo; i++)
		SB_Printf(sb, " %d %d", s.ammotype[i], s.ammo[i]);
	SB_Printf(sb, " %a %a %d", (double)land, (double)QC_EFloat(sv.vm, e, F.health), (int)QC_EFloat(sv.vm, e, F.waterlevel));
	log_flush(sb);
}

/* --- v2 INPUT: the device's commands, each run once --- */

static void hud_heard(link_t *l, uint16_t ack);

static void input_v2(link_t *l, const uint8_t *d, int len)
{
	client_t *c = l->client;
	uint32_t newest = rd32(d + 34);
	int count = d[38];
	bool here = c->state == CS_SPAWNED && l->devspawn == (svs.spawncount & 0xffff);
	l->v2 = true;
	c->linkcmds = true;
	if (!sv_link_log->ivalue)
		l->logon = false;
	v2_sync(l);
	/* a device makes at most 4 commands a frame: a jump past CMD_WINDOW is a bad packet,
	 * which would otherwise leave every later command looking like a resend */
	if (l->lastrun && newest > l->lastrun && newest - l->lastrun > CMD_WINDOW)
		return;
	if (l->devspawn == (svs.spawncount & 0xffff)) { /* acks for another map mean nothing here */
		const uint8_t *t = d + 44 + 14 * count;
		l->varsack = rd16(d + 40), l->brushack = rd16(d + 42);
		/* after the commands: 'W', the WEAPONINFO version the device has, for which edict */
		if (len >= 44 + 14 * count + 5 && t[0] == 'W') {
			l->wicap = l->wiheard = true; /* it takes WEAPONINFO (version 0: has none yet) */
			if (rd16(t + 3) == l->wiedict)
				l->wiack = (uint16_t)rd16(t + 1);
			t += 5;
		}
		/* then 'H', the HUDINFO version the device has: it draws the HUD itself */
		if (t + 3 <= d + len && t[0] == 'H')
			hud_heard(l, (uint16_t)rd16(t + 1));
	}
	if (!here) {
		/* not in the game, or the device is on another map: nothing runs,
		 * and the commands are never run later either */
		if (newest > l->lastrun)
			l->lastrun = newest;
		return;
	}
	for (int i = 0; i < count; i++) {
		const uint8_t *k = d + 44 + 14 * i;
		uint32_t s = newest - (uint32_t)(count - 1) + (uint32_t)i;
		int16_t pitch = (int16_t)rd16(k + 1), yaw = (int16_t)rd16(k + 3);
		usercmd_t u;
		strbuf_t sb = {0};
		bool log = sv_link_log->ivalue != 0, wasground = false;
		float fallvel = 0, basevel = 0;
		if (s <= l->lastrun)
			continue; /* run already: a resend */
		if (s - l->lastrun > 1)
			l->gaps += (uint16_t)(s - l->lastrun - 1); /* lost for good */
		memset(&u, 0, sizeof(u));
		u.sequence = s;
		u.msec = k[0] / 1000.0f;
		u.angles[0] = -pitch * (360.0f / 65536.0f);
		u.angles[1] = yaw * (360.0f / 65536.0f);
		u.angles[2] = 0;
		for (int a = 0; a < 3; a++)
			u.movevalues[a] = (float)(int16_t)rd16(k + 5 + 2 * a);
		u.buttons = rd16(k + 11);
		u.impulse = k[13];
		u.servertime_ms = 0; /* no lag to rewind: the device sees the others where the server has them */
		if (log) {
			lstate_t pre;
			log_before(l, c->edict, &sb);
			read_state(l, c->edict, &pre);
			/* Physics_Fall's downforce should this command land (player_pmove.qc:750-790) */
			fallvel = pre.flags & FL_ONGROUND ? 0 : -pre.velocity[2];
			basevel = pre.basevel[2];
			wasground = (pre.flags & FL_ONGROUND) != 0;
			log_wstate(l, c->edict, "wpre", 0, &sb);
		}
		if (F.v_angle >= 0) { /* as an old device's INPUT does; the game sets it from the command as well */
			float *va = &QC_EdictFields(sv.vm, c->edict)[F.v_angle].f;
			va[0] = u.angles[0], va[1] = u.angles[1], va[2] = 0;
		}
		SV_RunUsercmd(c, &u);
		if (!l->used || l->client != c) {
			SB_Free(&sb);
			return; /* the game dropped the player */
		}
		if (c->lastucmd.msec < u.msec)
			l->clipped++;
		watch_viewmodel(l); /* each command can start a sequence: a frame can run several */
		l->lastrun = s;
		if (log) {
			const usercmd_t *r = &c->lastucmd;
			lstate_t st;
			int slot = (int)(l - links);
			SB_Printf(&sb, "LINKPM %d cmd %u %d %d %d %d %d %d %u %d %a %d", slot, s, k[0], pitch, yaw,
			          (int)r->movevalues[0], (int)r->movevalues[1], (int)r->movevalues[2], r->buttons, k[13],
			          (double)r->msec, r->impulse);
			log_flush(&sb);
			read_state(l, c->edict, &st);
			SB_Printf(&sb, "LINKPM %d post", slot);
			log_state(&sb, &st);
			log_flush(&sb);
			log_wstate(l, c->edict, "wpost",
			           !wasground && (st.flags & FL_ONGROUND) ? (st.waterlevel ? 0 : fallvel) - basevel : -1, &sb);
			SB_Free(&sb);
		}
		if (c->state != CS_SPAWNED) { /* the command took the player out of the game */
			l->lastrun = BW_MAX(l->lastrun, newest);
			return;
		}
	}
}

static void update_hud(link_t *l);
static void send_hud(link_t *l, double now);

void SV_LinkSend(void)
{
	double now = Sys_Seconds();
	if (sock < 0)
		return;
	flush_lagout();
	for (int i = 0; i < MAX_LINKS; i++) {
		link_t *l = &links[i];
		bool view;
		if (!l->used || !l->client)
			continue;
		/* the load grace covers one quiet spell: a device that is never heard
		 * from again goes after LOAD_GRACE even if the map keeps changing */
		if (now - l->lastrecv > BW_MAX(sv_link_timeout->value, 2) &&
		    (now > l->loading_until || now - l->lastrecv > LOAD_GRACE)) {
			SV_DropClient(l->client, "the link timed out");
			continue;
		}
		/* the game's reliable messages to this client (MSG_ONE, MSG_ALL): a bot's are never sent */
		if (l->client->qcrel.len) {
			SV_LinkGameMessages(l->client, (const uint8_t *)l->client->qcrel.data, (int)l->client->qcrel.len);
			l->client->qcrel.len = 0;
		}
		ev_read(l);
		ev_coopfeed(l);
		if (l->client->state != CS_SPAWNED) {
			l->spawned = false;
			continue;
		}
		if (!l->spawned) {
			/* the game's EV_ANGLE for a first spawn reached nobody: the
			 * client was not in the game yet when it went out */
			const float *a = F.angles >= 0 ? &QC_EdictFields(sv.vm, l->client->edict)[F.angles].f : NULL;
			l->spawned = true;
			raise_fix(l, a ? a[0] : 0, a ? a[1] : 0);
		}
		if (l->spawncount != svs.spawncount || now >= l->nextwelcome)
			send_welcome(l); /* again every 2 s, in case one was lost */
		/* nothing else until the device has the map: while it loads it reads
		 * nothing, and its network stack keeps every packet that comes in
		 * meanwhile, which can leave too little memory for the map. A device
		 * that says which map it has is believed; otherwise the first packet
		 * after the welcome is taken to mean it has loaded (a packet already
		 * on its way when the welcome went out can fool that) */
		if (l->devspawn >= 0 ? l->devspawn != (svs.spawncount & 0xffff) : l->lastrecv < l->welcomed)
			continue;
		watch_viewmodel(l);
		if (l->numfx)
			send_fx(l);
		send_events(l, now);
		if (!l->gamedrew && now >= l->nextdefault) {
			default_screen(l);
			l->nextdefault = now + 1;
		}
		if (l->acked != l->version && now >= l->nextscreen) {
			send_screen(l);
			l->nextscreen = now + SCREEN_RESEND;
		}
		if ((view = now >= l->nextview)) {
			send_view(l);
			send_ents(l);
			if (l->entsseq % 5 == 0)
				send_models(l);
			if (l->entsseq % 4 == 0 || l->vmplays != l->vmsent)
				send_stats(l);
			l->nextview = now + VIEW_INTERVAL;
		}
		if (l->v2)
			send_v2(l, now, view);
		if (l->hudcap) {
			if (view)
				update_hud(l);
			if (l->hudver && l->hudack != l->hudver && (l->hudsentver != l->hudver || now >= l->hudsent + HUD_RESEND))
				send_hud(l, now);
		}
	}
}

/* a float field by name, 0 when the game has no such field */
static float efloat(int e, const char *name)
{
	int ofs = QC_FieldOfs(sv.vm, name);
	return ofs >= 0 ? QC_EdictFields(sv.vm, e)[ofs].f : 0;
}

/* the entity in the player's hand (Nuclide's .m_activeWeapon), 0 for none */
static int active_weapon(int e)
{
	int ofs = QC_FieldOfs(sv.vm, "m_activeWeapon"), w;
	if (ofs < 0 || (w = (int)QC_EdictFields(sv.vm, e)[ofs].u) <= 0 || w >= sv.vm->num_edicts || sv.vm->edicts[w].free)
		return 0;
	return w;
}

/* a class member field: Nuclide's compiler names them Class::__mm_name. Read by
 * its type: m_weaponAnimSequence is an int (NETWORKED_INT), and read as a float
 * it was always 0 */
static float member(int e, const char *cls, const char *name)
{
	char full[96];
	qfld_t f;
	snprintf(full, sizeof(full), "%s::__mm_%s", cls, name);
	f = qfld(full);
	if (f.ofs < 0)
		f = qfld(name);
	return qnum(e, f);
}

/* the view model's sequence, once a server frame and after each command a v2
 * device sends: a sequence started again (another shot, or an idle that is
 * over) shows as its time going back */
static void watch_viewmodel(link_t *l)
{
	int e = l->client->edict, seq = (int)member(e, "ncPlayer", "weaponAnimSequence");
	float t = member(e, "ncPlayer", "weaponAnimTime");
	if (seq != l->vmseq || t < l->vmtime)
		l->vmplays++;
	l->vmseq = seq, l->vmtime = t;
}

/* the player's numbers for the device's own status panel (docs/spec/link.md,
 * STATS): health, armour, score, and the weapon in hand by its class name,
 * without "weapon_" (Nuclide keeps it in .m_activeWeapon) */
static void send_stats(link_t *l)
{
	pkt_t p;
	int e = l->client->edict, w = active_weapon(e);
	const char *wname = "";
	size_t n;
	p_begin(&p, L_STATS);
	p_u32(&p, l->session);
	p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(QC_EFloat(sv.vm, e, F.health), -32768, 32767));
	p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(efloat(e, "armorvalue"), -32768, 32767));
	p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(efloat(e, "frags"), -32768, 32767));
	if (w) {
		wname = QC_GetString(sv.vm, QC_EInt(sv.vm, w, F.classname));
		if (!strncmp(wname, "weapon_", 7))
			wname += 7;
	}
	n = BW_MIN(strlen(wname), 24);
	p_u8(&p, (unsigned)n);
	p_bytes(&p, wname, (int)n);
	/* the view model: its model index (named by MODELS), sequence, and plays */
	p_u16(&p, w ? (unsigned)member(w, "ncWeapon", "viewModel") : 0);
	p_u8(&p, (unsigned)l->vmseq & 255);
	p_u8(&p, l->vmplays);
	l->vmsent = l->vmplays;
	send_to(&l->from, &p);
}

/* --- HUDINFO: what the co-op HUD shows ------------------------------------------------------
 * The PC's co-op HUD (coop/src/hud/jtf.qc, Jtf_Post) reads infokeys the rules
 * set (coop/src/rules/hud_info.qc: *herohud *respawn *revive *prompt
 * *promptAt *usable *stats *xpm *cadence *cadenceN *mate) and serverinfo
 * (coop_wave coop_left coop_notice coop_feed coop_engageCost, Nameplates'
 * nameplates and targetid), the weapon in hand (title, ammoRequired, its
 * decl's crosshair sprite: valve/src/hud/hud.qc HUD_WeaponSwitched), the
 * other players' names, health and teams, the friendly NPCs (Nameplates_Draw)
 * and the obituaries (EV_OBITUARY, HUD_Obituary). A link player gets none of
 * that from the engine (it is a bot to the networking), so it all goes in one
 * packet, a new version whenever any of it differs (docs/spec/link.md) */

enum {
	HK_HEROHUD, HK_RESPAWN, HK_REVIVE, HK_PROMPT, HK_PROMPTAT, HK_USABLE, HK_STATS, HK_XPM, HK_CADENCE, HK_CADENCEN,
	HK_SELFKEYS,
	HK_WAVE = 16, HK_LEFT, HK_NOTICE, HK_FEED, HK_ENGAGECOST, HK_NAMEPLATES, HK_TARGETID, HK_SERVERKEYS_END,
	HK_WTITLE = 32, HK_STATICNAME, HK_DYNAMICNAME, HK_NOAMMO
};
static const char *const hud_selfkeys[HK_SELFKEYS] = {"*herohud", "*respawn", "*revive", "*prompt", "*promptAt",
                                                      "*usable", "*stats", "*xpm", "*cadence", "*cadenceN"};
static const char *const hud_serverkeys[HK_SERVERKEYS_END - HK_WAVE] = {
	"coop_wave", "coop_left", "coop_notice", "coop_feed", "coop_engageCost", "nameplates", "targetid"};
#define EV_OBITUARY 25

static void hud_heard(link_t *l, uint16_t ack)
{
	if (!l->hudcap) {
		/* the HUD a device draws is the Panorama one: the rules leave out the
		 * text lines and centerprints it replaces (jtf.qc Jtf_SetPhud) */
		SV_SetPlayerKey(l->client, "phud", "1", true);
		l->hudcap = true;
	}
	l->hudack = ack;
}

/* EntityDef_GetKeyValue(cls, key) (src/shared/system/entityDef.qc), "" without */
static void def_key(const char *cls, const char *key, char *out, size_t size)
{
	int fn = cls[0] ? QC_FindFunction(sv.vm, "EntityDef_GetKeyValue") : 0;
	const char *s = NULL;
	if (fn > 0) {
		qcval_t r = qc_call(fn, 0, 2, cls, key);
		s = r.i ? QC_GetString(sv.vm, r.i) : NULL;
	}
	snprintf(out, size, "%s", s ? s : "");
}

/* jtf.qc Jtf_WeaponName: a def's inv_name, else the def's name */
static void def_title(const char *def, char *out, size_t size)
{
	def_key(def, "inv_name", out, size);
	if (!out[0])
		snprintf(out, size, "%s", def);
}

/* a token of a sprite list line, as tokenize_console splits it */
static const char *spr_token(const char **p, char *tok, size_t size)
{
	size_t n = 0;
	while (**p == ' ' || **p == '\t' || **p == '\r')
		(*p)++;
	if (!**p || **p == '\n')
		return NULL;
	while (**p && **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n')
		if (n + 1 < size)
			tok[n++] = *(*p)++;
		else
			(*p)++;
	tok[n] = 0;
	return tok;
}

/* the crosshair the PC draws for weapon class cls (hud.qc HUD_WeaponSwitched: the
 * decl's "crosshair", else "<cls>.crosshair"; src/client/InterfaceSpriteSheet.qc
 * finds "<file>.<name>" in sprites/<file>.txt, a name alone in sprites/hud.txt, at
 * resolution 640, and draws that part of the .spr at its size): its pixels as a
 * mask, in its commonest colour. None (w 0) when the PC draws none */
static void hud_crosshair(link_t *l, const char *cls)
{
	char name[128], file[192], want[128], *txt, tok[7][64];
	const char *dot, *p;
	size_t len;
	int sx = 0, sy = 0, sw = 0, sh = 0;
	bool found = false;
	uint8_t *spr;
	l->xw = l->xh = 0;
	def_key(cls, "crosshair", name, sizeof(name));
	if (!name[0])
		snprintf(name, sizeof(name), "%s.crosshair", cls);
	if ((dot = strrchr(name, '.'))) {
		snprintf(file, sizeof(file), "sprites/%.*s.txt", (int)(dot - name), name);
		snprintf(want, sizeof(want), "%s", dot + 1);
	} else {
		snprintf(file, sizeof(file), "sprites/hud.txt");
		snprintf(want, sizeof(want), "%s", name);
	}
	if (!(txt = FS_LoadFile(file, NULL)))
		return;
	for (p = txt; *p && !found;) {
		int c = 0;
		while (c < 7 && spr_token(&p, tok[c], sizeof(tok[c])))
			c++;
		if (c == 7 && spr_token(&p, tok[0], 1))
			c = 8; /* more than 7: not an entry */
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
		if (c == 7 && !strcmp(tok[1], "640") && !strcmp(tok[0], want)) {
			snprintf(file, sizeof(file), "sprites/%s.spr", tok[2]);
			sx = atoi(tok[3]), sy = atoi(tok[4]), sw = atoi(tok[5]), sh = atoi(tok[6]);
			found = true;
		}
	}
	Mem_Free(txt);
	if (!found || sw <= 0 || sh <= 0 || sw > 32 || sh > 32 || !(spr = (uint8_t *)FS_LoadFile(file, &len)))
		return;
	/* Half-Life's IDSP version 2: 40 bytes of header, the palette, then frame 0 */
	if (len > 42 && !memcmp(spr, "IDSP", 4) && rd32(spr + 4) == 2) {
		int fmt = (int)rd32(spr + 12), ncol = rd16(spr + 40), counts[256] = {0}, best = -1;
		size_t pal = 42, fr = pal + (size_t)ncol * 3;
		if (ncol <= 256 && fr + 20 <= len) {
			int fw = (int)rd32(spr + fr + 12), fh = (int)rd32(spr + fr + 16);
			const uint8_t *px = spr + fr + 20;
			if (fw > 0 && fh > 0 && sx >= 0 && sy >= 0 && sx + sw <= fw && sy + sh <= fh &&
			    fr + 20 + (size_t)fw * fh <= len) {
				memset(l->xmask, 0, sizeof(l->xmask));
				for (int y = 0; y < sh; y++)
					for (int x = 0; x < sw; x++) {
						int i = px[(sy + y) * fw + sx + x], c = i;
						const uint8_t *rgb;
						bool on;
						/* SPR_ALPHTEST: 255 is see-through; SPR_INDEXALPHA: the index is the
						 * alpha of colour 255; SPR_ADDITIVE: dark adds nothing */
						if (fmt == 2)
							on = i >= 128, c = 255;
						else if (fmt == 1) {
							rgb = spr + pal + (size_t)BW_MIN(i, ncol - 1) * 3;
							on = rgb[0] + rgb[1] + rgb[2] >= 192;
						} else
							on = fmt != 3 || i != 255;
						if (!on)
							continue;
						l->xmask[(y * sw + x) >> 3] |= (uint8_t)(1 << ((y * sw + x) & 7));
						counts[c]++;
						if (best < 0 || counts[c] > counts[best])
							best = c;
					}
				if (best >= 0 && best < ncol) {
					float rgb[3];
					for (int k = 0; k < 3; k++)
						rgb[k] = spr[pal + (size_t)best * 3 + k] / 255.0f;
					l->xw = (uint8_t)sw, l->xh = (uint8_t)sh, l->xrgb = rgb15(rgb);
				}
			}
		}
	}
	Mem_Free(spr);
}

static void p_hstr(pkt_t *p, const char *s)
{
	size_t n = BW_MIN(strlen(s), 255);
	p_u8(p, (unsigned)n);
	p_bytes(p, s, (int)n);
}

/* a key while the body stays under limit (what the roster, NPCs, crosshair and obituaries keep) */
static void p_hkey(pkt_t *p, int *count, int id, const char *v, int limit)
{
	if (!v[0] || p->len + 2 + (int)BW_MIN(strlen(v), 255) > limit)
		return;
	p_u8(p, (unsigned)id);
	p_hstr(p, v);
	(*count)++;
}

/* the body after the version, built once a VIEW; a new version when it differs */
static void update_hud(link_t *l)
{
	pkt_t p;
	char v[512], tok[64];
	int e = l->client->edict, w = active_weapon(e), countat, count = 0, nat, n = 0, reserve, keylim, nobit;
	qfld_t title = qfld("ncWeapon::__mm_weaponTitle"), ammoreq = qfld("ncWeapon::__mm_fiAmmoRequired");
	qfld_t alliance = qfld("m_iAlliance");
	const float *org = &QC_EdictFields(sv.vm, e)[F.origin].f;
	if (l->hudmap != svs.spawncount) { /* another progs: its strings are others */
		l->hudmap = svs.spawncount;
		l->hudwclass = 0;
		l->hudstatic[0] = l->huddynamic[0] = l->hudstaticname[0] = l->huddynamicname[0] = 0;
	}
	/* the crosshair and the obituaries come last and always fit: the sections before them
	 * stop at what those two need (the body may not pass MAX_HUDBODY: hudbody is that big) */
	nobit = BW_MIN(l->obitkept, HUD_OBITS);
	reserve = 2 + (l->xw ? 2 + (l->xw * l->xh + 7) / 8 : 0) + 2;
	for (int i = nobit; i >= 1; i--) {
		int k = (uint8_t)(l->obitseq - i) % HUD_OBITS;
		if (!l->obit[k].named) { /* by its inv_name (jtf.qc HUD_Obituary), outside the multicast */
			char def[32];
			snprintf(def, sizeof(def), "%s", l->obit[k].weapon);
			if (def[0])
				def_title(def, l->obit[k].weapon, sizeof(l->obit[k].weapon));
			l->obit[k].named = true;
		}
		reserve += 4 + (int)(strlen(l->obit[k].who) + strlen(l->obit[k].by) + strlen(l->obit[k].weapon) + strlen(l->obit[k].how));
	}
	keylim = MAX_HUDBODY - reserve - 2 - 160; /* the roster's and the NPCs' counts, and a few players */
	p.len = 0;
	p_u8(&p, (unsigned)(e - 1));
	countat = p.len;
	p_u8(&p, 0);
	for (int k = 0; k < HK_SELFKEYS; k++)
		p_hkey(&p, &count, k, SV_InfoGet(l->client->userinfo, hud_selfkeys[k], v, sizeof(v)), keylim);
	for (int k = HK_WAVE; k < HK_SERVERKEYS_END; k++)
		p_hkey(&p, &count, k, SV_InfoGet(sv.serverinfo, hud_serverkeys[k - HK_WAVE], v, sizeof(v)), keylim);
	/* the weapon in hand: jtf.qc's title, noAmmo, and held (title against the hero's two) */
	if (w) {
		int32_t cls = QC_EInt(sv.vm, w, F.classname);
		if (cls != l->hudwclass) { /* its crosshair (HUD_WeaponSwitched): looked up on a switch */
			l->hudwclass = cls;
			hud_crosshair(l, QC_GetString(sv.vm, cls));
		}
		p_hkey(&p, &count, HK_WTITLE, qstr(w, title), keylim);
		if (ammoreq.ofs >= 0 && !qnum(w, ammoreq))
			p_hkey(&p, &count, HK_NOAMMO, "1", keylim);
	}
	/* *herohud's static and dynamic weapons (fields 8 and 9) by their inv_name */
	SV_InfoGet(l->client->userinfo, "*herohud", v, sizeof(v));
	{
		const char *f = v;
		char *outs[2] = {l->hudstatic, l->huddynamic}, *names[2] = {l->hudstaticname, l->huddynamicname};
		for (int i = 0; i < 8 && f; i++)
			f = (f = strchr(f, '|')) ? f + 1 : NULL;
		for (int i = 0; i < 2; i++) {
			const char *end = f ? strchr(f, '|') : NULL;
			size_t len = f ? (end ? (size_t)(end - f) : strlen(f)) : 0;
			snprintf(tok, sizeof(tok), "%.*s", (int)BW_MIN(len, sizeof(tok) - 1), f ? f : "");
			if (strcmp(tok, outs[i])) {
				snprintf(outs[i], 64, "%s", tok);
				if (tok[0])
					def_title(tok, names[i], 64);
				else
					names[i][0] = 0;
			}
			f = end ? end + 1 : NULL;
		}
		p_hkey(&p, &count, HK_STATICNAME, l->hudstaticname, keylim);
		p_hkey(&p, &count, HK_DYNAMICNAME, l->huddynamicname, keylim);
	}
	p.data[countat] = (uint8_t)count;
	/* everyone with a name (getplayerkeyvalue's players), by slot, this player too */
	nat = p.len;
	p_u8(&p, 0);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		char name[64], mate[32], cad[8], team[16];
		if (c->state < CS_CONNECTED || !SV_InfoGet(c->userinfo, "name", name, sizeof(name))[0])
			continue;
		SV_InfoGet(c->userinfo, "*mate", mate, sizeof(mate));
		SV_InfoGet(c->userinfo, "*cadence", cad, sizeof(cad));
		SV_InfoGet(c->userinfo, "*team", team, sizeof(team));
		if (p.len + 8 + (int)strlen(name) + (int)strlen(mate) + (int)strlen(team) > MAX_HUDBODY - reserve - 1)
			break;
		p_u8(&p, (unsigned)i);
		p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(c->state == CS_SPAWNED ? QC_EFloat(sv.vm, c->edict, F.health) : 0,
		                                     -32768, 32767));
		p_hstr(&p, name);
		p_hstr(&p, mate);
		p_u8(&p, (unsigned)BW_CLAMP(atoi(cad), 0, 255));
		p_hstr(&p, team);
		n++;
	}
	p.data[BW_MIN(nat, (int)sizeof(p.data) - 1)] = (uint8_t)n;
	/* friendly NPCs within the target ID's reach (Nameplates_Draw: FL_MONSTER, alive, MAL_FRIEND) */
	nat = p.len, n = 0;
	p_u8(&p, 0);
	for (int i = svs.maxclients + 1; alliance.ofs >= 0 && i < sv.vm->num_edicts && n < HUD_MAXNPC; i++) {
		const float *o;
		const char *nn;
		float d2 = 0, hp, mx;
		if (!ent_live(i) || !((int)QC_EFloat(sv.vm, i, F.flags) & FL_MONSTER) || (hp = QC_EFloat(sv.vm, i, F.health)) <= 0 ||
		    (int)qnum(i, alliance) != 0) /* MAL_FRIEND, the enum's first (src/shared/ai/Monster.h) */
			continue;
		o = &QC_EdictFields(sv.vm, i)[F.origin].f;
		for (int k = 0; k < 3; k++)
			d2 += (o[k] - org[k]) * (o[k] - org[k]);
		if (d2 > HUD_NPCRANGE * HUD_NPCRANGE)
			continue;
		nn = F.netname >= 0 ? QC_GetString(sv.vm, QC_EInt(sv.vm, i, F.netname)) : "";
		if (p.len + 7 + (int)BW_MIN(strlen(nn), HUD_NAME) > MAX_HUDBODY - reserve)
			break;
		mx = efloat(i, "max_health");
		p_u16(&p, (unsigned)i);
		p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(ceilf(hp), -32768, 32767));
		p_u16(&p, (uint16_t)(int16_t)BW_CLAMP(mx > 0 ? mx : hp, -32768, 32767));
		p_u8(&p, (unsigned)BW_MIN(strlen(nn), HUD_NAME));
		p_bytes(&p, nn, (int)BW_MIN(strlen(nn), HUD_NAME));
		n++;
	}
	p.data[nat] = (uint8_t)n;
	/* the crosshair */
	p_u8(&p, l->xw), p_u8(&p, l->xh);
	if (l->xw) {
		p_u16(&p, l->xrgb);
		p_bytes(&p, l->xmask, (l->xw * l->xh + 7) / 8);
	}
	/* the newest obituaries */
	p_u8(&p, l->obitseq), p_u8(&p, (unsigned)nobit);
	for (int i = nobit; i >= 1; i--) {
		int k = (uint8_t)(l->obitseq - i) % HUD_OBITS;
		p_hstr(&p, l->obit[k].who), p_hstr(&p, l->obit[k].by), p_hstr(&p, l->obit[k].weapon), p_hstr(&p, l->obit[k].how);
	}
	if (p.len > MAX_HUDBODY) { /* cannot happen with the budget above; never send a cut body */
		Con_Printf("link: %s: HUDINFO body %d bytes, over %d: not sent\n", l->client->name, p.len, MAX_HUDBODY);
		return;
	}
	if (p.len != l->hudlen || memcmp(p.data, l->hudbody, (size_t)p.len)) {
		memcpy(l->hudbody, p.data, (size_t)p.len);
		l->hudlen = p.len;
		l->hudver = (uint16_t)(l->hudver == 65535 ? 1 : l->hudver + 1); /* 0 is none */
	}
}

static void send_hud(link_t *l, double now)
{
	pkt_t p;
	p_begin(&p, L_HUDINFO);
	p_u32(&p, l->session);
	p_u16(&p, l->hudver);
	p_bytes(&p, l->hudbody, l->hudlen);
	send_to(&l->from, &p);
	l->hudsent = now, l->hudsentver = l->hudver;
}

/* an EV_OBITUARY (SVPF_combat_Obituary: target, attacker, weapon decl, means of death) */
static void hud_obituary(link_t *l, const uint8_t *data, int len)
{
	char s[4][32]; /* as kept: longer is cut */
	int pos = 2;
	for (int k = 0; k < 4; k++) {
		int n = 0;
		while (pos < len && data[pos]) {
			if (n + 1 < (int)sizeof(s[k]))
				s[k][n++] = (char)data[pos];
			pos++;
		}
		s[k][n] = 0;
		if (pos++ >= len)
			return;
	}
	{
		int k = l->obitseq % HUD_OBITS;
		snprintf(l->obit[k].who, sizeof(l->obit[k].who), "%s", s[0]);
		snprintf(l->obit[k].by, sizeof(l->obit[k].by), "%s", s[1]);
		snprintf(l->obit[k].weapon, sizeof(l->obit[k].weapon), "%s", s[2]);
		l->obit[k].named = false;
		snprintf(l->obit[k].how, sizeof(l->obit[k].how), "%.15s", s[3]);
		l->obitseq++;
		if (l->obitkept < HUD_OBITS)
			l->obitkept++;
	}
}

/* --- bullet impacts ------------------------------------------------------------------------
 * The PC client draws impacts from the game's CSQC events (Nuclide's
 * SVC_CGAMEPACKET with EV_SURFIMPACT / EV_SURFIMPACTID, which start with the
 * impact point) and its muzzle flashes and tracers itself. A DS runs no CSQC,
 * so the server reads those multicasts as they go to a device's client and
 * passes the point on, with where the shooter's eye was for a tracer */
#define CGAMEPACKET 83
#define EV_SURFIMPACT 33
#define EV_SURFIMPACTID 34
#define EV_ANGLE 10 /* Client_FixAngle, spawns: the game turns the player */

static float rdfloat(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

void SV_LinkMulticast(client_t *c, const uint8_t *data, int len, int cause)
{
	link_t *l;
	if (len >= 14 && data[0] == CGAMEPACKET && data[1] == EV_ANGLE) {
		if ((l = link_for_edict(c->edict)))
			raise_fix(l, rdfloat(data + 2), rdfloat(data + 6));
		return;
	}
	if (len >= 6 && data[0] == CGAMEPACKET && data[1] == EV_OBITUARY && (l = link_for_edict(c->edict)) && l->hudcap)
		hud_obituary(l, data, len); /* the HUD's feed (HUDINFO); EVENT skips it for such a device */
	if (len >= 2 && data[0] == CGAMEPACKET && data[1] != EV_SURFIMPACT && data[1] != EV_SURFIMPACTID) {
		SV_LinkGameMessages(c, data, len); /* read in SV_LinkSend (EVENT) */
		return;
	}
	if (len < 14 || data[0] != CGAMEPACKET)
		return;
	if (!(l = link_for_edict(c->edict)) || l->numfx >= MAX_FX)
		return;
	l->fx[l->numfx].flags = 0;
	for (int k = 0; k < 3; k++)
		l->fx[l->numfx].to[k] = rdfloat(data + 2 + k * 4);
	if (cause >= 1 && cause < sv.vm->num_edicts && !sv.vm->edicts[cause].free) { /* a player's or a monster's shot */
		const qcval_t *f = QC_EdictFields(sv.vm, cause);
		l->fx[l->numfx].flags = FX_SHOT | (cause == c->edict ? FX_OWN : 0);
		for (int k = 0; k < 3; k++)
			l->fx[l->numfx].from[k] = f[F.origin + k].f + (F.view_ofs >= 0 ? f[F.view_ofs + k].f : 0);
	}
	l->numfx++;
}

static void send_fx(link_t *l)
{
	pkt_t p;
	p_begin(&p, L_FX);
	p_u32(&p, l->session);
	p_u8(&p, (unsigned)l->numfx);
	for (int i = 0; i < l->numfx; i++) {
		p_u8(&p, l->fx[i].flags);
		for (int k = 0; k < 3; k++)
			p_u32(&p, (uint32_t)(int32_t)(l->fx[i].to[k] * 8));
		for (int k = 0; k < 3; k++)
			p_u32(&p, (uint32_t)(int32_t)(l->fx[i].from[k] * 8));
	}
	l->numfx = 0;
	send_to(&l->from, &p);
}

/* --- events (EVENT) -------------------------------------------------------------------------
 * What the PC's client shows from the server besides the world: prints, chat and
 * centerprints (svc_print, svc_centerprint), and the game's CSQC events (Nuclide's
 * SVC_CGAMEPACKET, src/client/event.qc Event_Parse): view shakes, fades, damage,
 * game_text and env_message titles, the kill feed, pickups. A link player is a bot to
 * the engine, so none of that reaches it as messages; the engine hands it here
 * instead (SV_LinkPrint, SV_LinkCenterPrint, SV_LinkMulticast, and the client's
 * reliable QC buffer, read in SV_LinkSend). It is kept raw until SV_LinkSend, where
 * the game's code may run again (a pickup's name, a weapon's), and is turned into
 * records the device draws as the PC's client does. Records go in order, each once,
 * and again until the device says it has them (docs/spec/link.md, EVENT) */
enum { RAW_GAME = 1, RAW_PRINT, RAW_CENTER };
enum { LE_PRINT = 1, LE_CENTER, LE_SHAKE, LE_FADE, LE_DAMAGE, LE_TEXT, LE_FEED, LE_PICKUP, LE_TEXTAGAIN };
/* Nuclide's src/common/c_events.h */
enum { EVN_PICKUP = 7, EVN_DAMAGE = 8, EVN_HITNOTIFY = 9, EVN_ANGLE = 10, EVN_SHAKE = 15, EVN_HUDHINT = 16, EVN_FADE = 17,
       EVN_TEXT = 18, EVN_TEXT_STRING = 19, EVN_MESSAGE = 20, EVN_OBITUARY = 25, EVN_CHAT = 28, EVN_CHAT_TEAM = 29 };
#define PRINT_CHAT 3

static link_t *link_of(const client_t *c)
{
	link_t *l;
	if (!c || c->link <= 0 || c->link > MAX_LINKS)
		return NULL;
	l = &links[c->link - 1];
	return l->used && l->client == c ? l : NULL;
}

static void ev_raw(link_t *l, int kind, int level, const void *d, int len)
{
	uint8_t *o;
	len = BW_MIN(len, 0xffff);
	if (!l || l->evrawlen + 4 + len > EV_RAW) {
		if (l)
			l->evdropped++;
		return;
	}
	o = l->evraw + l->evrawlen;
	o[0] = (uint8_t)kind, o[1] = (uint8_t)level, o[2] = (uint8_t)(len & 0xff), o[3] = (uint8_t)(len >> 8);
	memcpy(o + 4, d, (size_t)len);
	l->evrawlen += 4 + len;
}

void SV_LinkPrint(client_t *c, int level, const char *text) { ev_raw(link_of(c), RAW_PRINT, level, text, (int)strlen(text)); }
void SV_LinkCenterPrint(client_t *c, const char *text) { ev_raw(link_of(c), RAW_CENTER, 0, text, (int)strlen(text)); }
void SV_LinkGameMessages(client_t *c, const uint8_t *d, int len) { ev_raw(link_of(c), RAW_GAME, 0, d, len); }

/* the text without the game's colour codes (^0-^9, ^xRGB, ^b and the like), at most size - 1 bytes */
static void ev_plain(char *out, int size, const char *s, int len)
{
	int n = 0;
	for (int i = 0; i < len && s[i] && n < size - 1; i++) {
		/* titles.qc Titles_ParseFunString's escapes: colours, \R a space, \n and \N new lines */
		if (s[i] == '\\' && i + 1 < len && strchr("yrgbwdRnN", s[i + 1]) && s[i + 1]) {
			char e = s[++i];
			if (e == 'R' || e == 'n' || e == 'N')
				out[n++] = e == 'R' ? ' ' : '\n';
			continue;
		}
		if (s[i] == '^' && i + 1 < len) {
			if (s[i + 1] == 'x' && i + 4 < len && isxdigit((unsigned char)s[i + 2]) && isxdigit((unsigned char)s[i + 3]) &&
			    isxdigit((unsigned char)s[i + 4])) {
				i += 4;
				continue;
			}
			if (isdigit((unsigned char)s[i + 1]) || s[i + 1] == 'b' || s[i + 1] == 'd' || s[i + 1] == 's' || s[i + 1] == 'r' ||
			    s[i + 1] == 'm' || s[i + 1] == 'h') {
				i++;
				continue;
			}
			if (s[i + 1] == '^')
				i++;
		}
		out[n++] = s[i];
	}
	out[n] = 0;
}

/* one record; the oldest goes when the device is EV_RING behind */
static void ev_put(link_t *l, int type, const uint8_t *d, int len)
{
	uint8_t *r;
	if (len + 2 > EV_REC)
		len = EV_REC - 2;
	if ((uint16_t)(l->evnext - l->evack - 1) >= EV_RING) {
		l->evack++;
		l->evdropped++;
	}
	r = l->evring[l->evnext % EV_RING];
	r[0] = (uint8_t)type, r[1] = (uint8_t)len;
	memcpy(r + 2, d, (size_t)len);
	l->evlen[l->evnext % EV_RING] = (uint8_t)(len + 2);
	if (sv_link_log->ivalue) {
		static const char *names[] = {"?", "print", "center", "shake", "fade", "damage", "text", "feed", "pickup", "again"};
		char show[64] = "";
		int off = type == LE_PRINT ? 1 : type == LE_TEXT ? 32 : type == LE_FEED ? 1 : (type == LE_CENTER || type == LE_PICKUP) ? 0 : -1;
		if (off >= 0 && len > off)
			snprintf(show, sizeof(show), " \"%.*s\"", BW_MIN(len - off, 48), (const char *)d + off);
		if (type == LE_DAMAGE && len >= 8)
			snprintf(show, sizeof(show), " at %d %d %d take %d", (int16_t)rd16(d), (int16_t)rd16(d + 2), (int16_t)rd16(d + 4),
			         (int16_t)rd16(d + 6));
		Con_Printf("LINKEV %s %u %s %d bytes%s\n", l->client->name, l->evnext, names[type < 10 ? type : 0], len, show);
	}
	l->evnext++;
	l->evcount++;
}

/* a record of fixed fields then text (the rest of it). game_text sent again as it was
 * (the co-op hero line, 4 times a second) is only its channel: the device starts it over */
static void ev_text(link_t *l, int type, const uint8_t *head, int hlen, const char *text, int tlen)
{
	uint8_t b[EV_REC];
	char plain[EV_REC];
	int n;
	memcpy(b, head, (size_t)hlen);
	ev_plain(plain, EV_REC - 2 - hlen, text, tlen);
	n = (int)strlen(plain);
	memcpy(b + hlen, plain, (size_t)n);
	if (type == LE_TEXT && b[0] < 5) {
		uint32_t h = 2166136261u;
		for (int i = 0; i < hlen + n; i++)
			h = (h ^ b[i]) * 16777619u;
		h |= 1;
		if (h == l->evtext[b[0]]) {
			ev_put(l, LE_TEXTAGAIN, b, 1);
			return;
		}
		l->evtext[b[0]] = h;
	}
	ev_put(l, type, b, hlen + n);
}

/* a NUL-terminated string at d (n bytes left): its length, -1 if it runs out */
static int ev_str(const uint8_t *d, int n)
{
	const uint8_t *z = memchr(d, 0, (size_t)BW_MAX(n, 0));
	return z ? (int)(z - d) : -1;
}

static int qc_fn(const char *name) { return sv.vm ? QC_FindFunction(sv.vm, name) : 0; }

/* EntityDef_NameFromNetID (src/shared/system/entityDef.qc): EV_PICKUP's item */
static const char *ev_itemclass(int32_t id)
{
	int fn = qc_fn("EntityDef_NameFromNetID");
	const char *s;
	if (fn <= 0)
		return "";
	sv.vm->globals[OFS_PARM(0)].i = id;
	sv.vm->argc = 1;
	if (!QC_Execute(sv.vm, fn) || !sv.vm->globals[OFS_RETURN].i)
		return "";
	s = QC_GetString(sv.vm, sv.vm->globals[OFS_RETURN].i);
	return s ? s : "";
}

/* entityDef.GetString(def, "inv_name"), as coop/src/hud/jtf.qc Jtf_WeaponName, else the def */
static void ev_weaponname(const char *def, char *out, int size)
{
	int fn = qc_fn("EntityDef_GetKeyValue");
	const char *s = NULL;
	if (fn > 0 && def[0]) {
		qcval_t r = qc_call(fn, 0, 2, def, "inv_name");
		s = r.i ? QC_GetString(sv.vm, r.i) : NULL;
	}
	snprintf(out, (size_t)size, "%s", s && s[0] ? s : def);
}

static bool word_in(const char *list, const char *word)
{
	size_t n = strlen(word);
	for (const char *p = list; (p = strstr(p, word)); p += n)
		if ((p == list || isspace((unsigned char)p[-1])) && (!p[n] || isspace((unsigned char)p[n])))
			return true;
	return false;
}

/* a line of the kill feed: attacker (or the victim, with "died"), weapon, victim, and whether
 * it is about us (the PC's feed rings those) */
static void ev_feed(link_t *l, const char *a, const char *w, const char *v)
{
	uint8_t b[EV_REC];
	char pa[64], pw[64], pv[64], me[64];
	int n = 1;
	if (l->hudcap) /* a device that draws the HUD has the feed in HUDINFO (obituaries, coop_feed) */
		return;
	ev_plain(pa, sizeof(pa), a, (int)strlen(a));
	ev_plain(pw, sizeof(pw), w, (int)strlen(w));
	ev_plain(pv, sizeof(pv), v, (int)strlen(v));
	ev_plain(me, sizeof(me), l->client->name, (int)strlen(l->client->name));
	b[0] = (!strcmp(pa, me) || (pv[0] && !strcmp(pv, me))) ? 1 : 0;
	b[n++] = (uint8_t)strlen(pa), memcpy(b + n, pa, strlen(pa)), n += (int)strlen(pa);
	b[n++] = (uint8_t)strlen(pw), memcpy(b + n, pw, strlen(pw)), n += (int)strlen(pw);
	memcpy(b + n, pv, strlen(pv)), n += (int)strlen(pv);
	ev_put(l, LE_FEED, b, n);
}

/* EV_OBITUARY as coop/src/hud/jtf.qc HUD_Obituary posts it to the Panorama feed: only
 * players' deaths; "self" in the means of death leaves out the attacker */
static void ev_obituary(link_t *l, const char *target, const char *attacker, const char *wdef, const char *mod)
{
	bool player = false;
	char wname[64];
	for (int i = 0; i < svs.maxclients && target[0]; i++)
		if (svs.clients[i].state == CS_SPAWNED && !strcmp(svs.clients[i].name, target))
			player = true;
	if (!player)
		return;
	if (word_in(mod, "self"))
		attacker = "";
	if (!attacker[0]) {
		ev_feed(l, target, "died", "");
		return;
	}
	ev_weaponname(wdef, wname, sizeof(wname));
	ev_feed(l, attacker, wname[0] ? wname : "killed", target);
}

/* Util_ChatFormat (src/shared/system/util.qc:75), without its colours */
static void ev_chat(link_t *l, int sender, int team, const char *msg, int mlen)
{
	char line[EV_REC], m[EV_REC], key[32], tname[64];
	uint8_t lv = PRINT_CHAT;
	const char *name = sender >= 0 && sender < svs.maxclients ? svs.clients[sender].name : "";
	ev_plain(m, sizeof(m), msg, mlen);
	if (team == 0)
		snprintf(line, sizeof(line), "%.48s: %.160s", name, m);
	else {
		snprintf(key, sizeof(key), "team_%d", team);
		SV_InfoGet(sv.serverinfo, key, tname, sizeof(tname));
		snprintf(line, sizeof(line), "%.48s (%.24s): %.140s", name, tname, m);
	}
	ev_text(l, LE_PRINT, &lv, 1, line, (int)strlen(line));
}

static void put_f32(uint8_t *o, const uint8_t *src) { memcpy(o, src, 4); } /* little-endian on both ends */

/* one CSQC event (after SVC_CGAMEPACKET and its id): bytes read, -1 for an event whose
 * length isn't known here (the rest of the stream is left) */
static int ev_game(link_t *l, int ev, const uint8_t *d, int n)
{
	uint8_t b[64];
	int s0, s1, s2, s3;
	switch (ev) {
	case EVN_DAMAGE: /* coords, int take, int flags (src/shared/game/Player.qc:2370) */
		if (n < 20)
			return -1;
		for (int k = 0; k < 3; k++) {
			int v = (int)BW_CLAMP(rdfloat(d + k * 4), -32767.0f, 32767.0f);
			b[k * 2] = (uint8_t)(v & 0xff), b[k * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
		}
		{
			int take = (int)BW_CLAMP((int32_t)rd32(d + 12), -32767, 32767);
			b[6] = (uint8_t)(take & 0xff), b[7] = (uint8_t)((take >> 8) & 0xff);
		}
		memcpy(b + 8, d + 16, 4);
		ev_put(l, LE_DAMAGE, b, 12);
		return 20;
	case EVN_HITNOTIFY: /* the PC's client does nothing with it */
		return 0;
	case EVN_ANGLE:
		if (n < 12)
			return -1;
		raise_fix(l, rdfloat(d), rdfloat(d + 4));
		return 12;
	case EVN_SHAKE: /* duration, amplitude, frequency; a spectator feels none (event.qc EV_Shake) */
		if (n < 12)
			return -1;
		if (F.classname >= 0 && strcmp(QC_GetString(sv.vm, QC_EInt(sv.vm, l->client->edict, F.classname)), "spectator"))
			ev_put(l, LE_SHAKE, d, 12);
		return 12;
	case EVN_FADE: /* r g b a, duration, hold, flags (fade.qc Fade_Parse) */
		if (n < 13)
			return -1;
		ev_put(l, LE_FADE, d, 13);
		return 13;
	case EVN_HUDHINT: { /* event.qc EV_HUDHint: a chat line */
		char line[EV_REC];
		uint8_t lv = PRINT_CHAT;
		if ((s0 = ev_str(d, n)) < 0)
			return -1;
		snprintf(line, sizeof(line), "Hint: %.*s", s0, (const char *)d);
		ev_text(l, LE_PRINT, &lv, 1, line, (int)strlen(line));
		return s0 + 1;
	}
	case EVN_TEXT:
	case EVN_TEXT_STRING: /* text.qc GameText_ParseString: chan, string, x, y, effect, colours, times */
		if (n < 1 || (s0 = ev_str(d + 1, n - 1)) < 0 || n < 1 + s0 + 1 + 31)
			return -1;
		{
			const uint8_t *t = d + 1 + s0 + 1;
			b[0] = d[0];
			put_f32(b + 1, t), put_f32(b + 5, t + 4);
			memcpy(b + 9, t + 8, 7); /* effect, colour 1, colour 2 */
			for (int k = 0; k < 4; k++)
				put_f32(b + 16 + k * 4, t + 15 + k * 4);
			ev_text(l, LE_TEXT, b, 32, (const char *)d + 1, s0);
		}
		return 1 + s0 + 1 + 31;
	case EVN_MESSAGE: /* text.qc GameMessage_Parse: a titles.txt name (or the text), sound, volume, attenuation */
		if ((s0 = ev_str(d, n)) < 0 || (s1 = ev_str(d + s0 + 1, n - s0 - 1)) < 0 || n < s0 + s1 + 2 + 5)
			return -1;
		{
			/* GameMessage_Setup's defaults for a name titles.txt lacks; channel 0 is the queue */
			static const float def[] = {-1, 0.75f, 0.5f, 0.5f, 2.0f, 0};
			float f[6];
			memcpy(f, def, sizeof(f));
			b[0] = 0xff;
			memcpy(b + 1, &f[0], 4), memcpy(b + 5, &f[1], 4);
			b[9] = 0;
			memset(b + 10, 255, 6);
			memcpy(b + 16, &f[2], 4), memcpy(b + 20, &f[3], 4), memcpy(b + 24, &f[4], 4), memcpy(b + 28, &f[5], 4);
			ev_text(l, LE_TEXT, b, 32, (const char *)d, s0);
		}
		return s0 + s1 + 2 + 5;
	case EVN_OBITUARY: /* target, attacker, weapon decl, means of death (src/server/api.qc:300) */
		if ((s0 = ev_str(d, n)) < 0 || (s1 = ev_str(d + s0 + 1, n - s0 - 1)) < 0 ||
		    (s2 = ev_str(d + s0 + s1 + 2, n - s0 - s1 - 2)) < 0 || (s3 = ev_str(d + s0 + s1 + s2 + 3, n - s0 - s1 - s2 - 3)) < 0)
			return -1;
		ev_obituary(l, (const char *)d, (const char *)d + s0 + 1, (const char *)d + s0 + s1 + 2,
		            (const char *)d + s0 + s1 + s2 + 3);
		return s0 + s1 + s2 + s3 + 4;
	case EVN_PICKUP: { /* the item's entityDef network id (src/shared/game/Item.qc:823) */
		const char *cls;
		if (n < 4)
			return -1;
		cls = ev_itemclass((int32_t)rd32(d));
		if (cls[0])
			ev_put(l, LE_PICKUP, (const uint8_t *)cls, (int)BW_MIN(strlen(cls), (size_t)EV_REC - 2));
		return 4;
	}
	case EVN_CHAT:
	case EVN_CHAT_TEAM: /* sender's slot, team, text (src/server/GameRules.qc:354) */
		if (n < 2 || (s0 = ev_str(d + 2, n - 2)) < 0)
			return -1;
		ev_chat(l, d[0], ev == EVN_CHAT ? 0 : d[1], (const char *)d + 2, s0);
		return 2 + s0 + 1;
	default:
		return -1;
	}
}

/* what came since the last frame, as records */
static void ev_read(link_t *l)
{
	int i = 0;
	while (i + 4 <= l->evrawlen) {
		const uint8_t *h = l->evraw + i, *d = h + 4;
		int kind = h[0], level = h[1], len = h[2] | h[3] << 8;
		i += 4 + len;
		if (kind == RAW_PRINT) {
			uint8_t lv = (uint8_t)level;
			/* a print can end in a newline; the PC's lines don't show it */
			while (len > 0 && (d[len - 1] == '\n' || d[len - 1] == '\r'))
				len--;
			if (len > 0)
				ev_text(l, LE_PRINT, &lv, 1, (const char *)d, len);
		} else if (kind == RAW_CENTER)
			ev_text(l, LE_CENTER, NULL, 0, (const char *)d, len);
		else
			for (int j = 0, used; j + 2 <= len && d[j] == CGAMEPACKET; j += 2 + used)
				if ((used = ev_game(l, d[j + 1], d + j + 2, len - j - 2)) < 0)
					break;
	}
	l->evrawlen = 0;
}

/* serverinfo coop_feed, "seq|who|verb|whom": the co-op rules' team events (revives) for the
 * Panorama feed (coop/src/hud/jtf.qc Jtf_Post); the one up when we joined isn't news */
static void ev_coopfeed(link_t *l)
{
	char feed[96], *f[4] = {0}, *p;
	int n = 0;
	SV_InfoGet(sv.serverinfo, "coop_feed", feed, sizeof(feed));
	if (!feed[0]) {
		l->evfeedinit = true;
		return;
	}
	if (!strcmp(feed, l->evfeed))
		return;
	snprintf(l->evfeed, sizeof(l->evfeed), "%s", feed);
	if (!l->evfeedinit) {
		l->evfeedinit = true;
		return;
	}
	for (p = feed; n < 4; n++) {
		f[n] = p;
		if (!(p = strchr(p, '|')))
			break;
		*p++ = 0;
	}
	if (n >= 3)
		ev_feed(l, f[1], f[2], f[3]);
}

static void send_events(link_t *l, double now)
{
	pkt_t p;
	uint16_t first = (uint16_t)(l->evack + 1);
	int count = 0;
	if (l->evnext == first || !l->evcap)
		return;
	if (l->evnext == l->evsentnext && now - l->evsent < EV_RESEND)
		return;
	p_begin(&p, L_EVENT);
	p_u32(&p, l->session);
	p_u16(&p, first);
	p_u8(&p, 0);
	for (uint16_t s = first; s != l->evnext && count < 255; s++, count++) {
		int n = l->evlen[s % EV_RING];
		if (p.len + n > (int)sizeof(p.data))
			break;
		p_bytes(&p, l->evring[s % EV_RING], n);
	}
	p.data[5 + 6] = (uint8_t)count;
	l->evsent = now, l->evsentnext = l->evnext;
	send_to(&l->from, &p);
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
	c->linkcmds = false;
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
		if (!l->used || !l->client || (argc > 5 && Str_ICmp(argv[5], l->client->name)))
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

/* link_god [0|1]: link players take no damage (takedamage 0), for tests that
 * watch a fight from where it happens */
static void SV_LinkGod_f(int argc, const char **argv)
{
	int td = sv.active ? QC_FieldOfs(sv.vm, "takedamage") : -1;
	bool on = argc < 2 || atoi(argv[1]);
	if (td < 0)
		return;
	if (!Cvar_Value("sv_cheats")) {
		Con_Printf("link_god needs sv_cheats 1\n");
		return;
	}
	for (int i = 0; i < MAX_LINKS; i++)
		if (links[i].used && links[i].client) {
			QC_EdictFields(sv.vm, links[i].client->edict)[td].f = on ? 0 : 2;
			Con_Printf("link: %s %s\n", links[i].client->name, on ? "takes no damage" : "takes damage");
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
		if (l->v2)
			Con_Printf("  link v2 (join %d): ran to %u, %u lost, %u clipped, pstate %u, vars %u/%u, brush %u/%u, "
			           "weapon %u/%u%s\n",
			           l->joinver, l->lastrun, l->gaps, l->clipped, l->pseq, l->varsack, l->varsver, l->brushack,
			           l->brushver, l->wiack, l->wiver, l->wicap ? "" : " (device takes none)");
		Con_Printf("  events: %u made, %u acked of %u, %u dropped%s\n", l->evcount, (uint16_t)(l->evack),
		           (uint16_t)(l->evnext - 1), l->evdropped, l->evcap ? "" : " (device takes none)");
	}
}

void SV_LinkInit(void)
{
	Cmd_AddD("link_stats", SV_LinkStats_f, "Show the link players and how fast their devices run");
	Cmd_AddD("link_teleport", SV_LinkTeleport_f, "Put the link players (or the one named) at x y z [yaw [name]] (sv_cheats 1)");
	Cmd_AddD("link_god", SV_LinkGod_f, "Link players take no damage: link_god [0|1] (sv_cheats 1)");
	sv_link_port = Cvar_Get("sv_link_port", "27901", 0, "UDP port for link players (Nintendo DS), 0 for none");
	sv_link_max = Cvar_Get("sv_link_max", "4", 0, "Player slots link players may take");
	sv_link_timeout = Cvar_Get("sv_link_timeout", "10", 0, "Seconds without input before a link player is dropped");
	sv_link_log = Cvar_Get("sv_link_log", "0", 0, "1: print every command a link v2 player runs, with its state (LINKPM lines)");
	sv_link_lag = Cvar_Get("sv_link_lag", "0", 0, "Milliseconds each link packet is held, each way, to test latency");
}
