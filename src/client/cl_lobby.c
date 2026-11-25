/* Bulwark engine - the pre-game lobby (docs/spec/lobby.md).
 *
 * A host-authoritative party that exists before any game server: the host
 * owns seats, map, rules and the countdown; guests only send what they want
 * (ready, chat, leave). When the countdown ends, or the host starts, the host
 * starts a listen server and tells every guest where to connect.
 *
 * It runs on its own UDP socket (lobby_port, 27501, for the host), apart
 * from the game protocol. Datagrams are "BWLB1 " and a JSON object whose
 * "t" says what it is; the format is Bulwark's own (the reference engine's
 * wasn't documented). Room codes ("online" lobbies) are found by LAN
 * discovery, and, when Bulwark Live is connected (live_connect), over the
 * internet: an online lobby's room code is its Live party's code, and the
 * lobby's datagrams go to "live:<id>" peers through net/live.c (punched or
 * relayed, split into pieces that fit a Live datagram).
 * ISC licence, see LICENSE. */
#include "lobby.h"
#include "dedlaunch.h"
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/json.h"
#include "../net/live.h"
#include "../net/net.h"
#include "../server/sv_local.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../net/net_compat.h"

#define LOBBY_MAGIC "BWLB1 "
#define LOBBY_SEATS 16
#define LOBBY_CHAT 10
#define LOBBY_TIMEOUT 10.0
#define LOBBY_MAX_INTENTS 32

typedef struct {
	bool used;
	char name[64];
	bool ready, connected;
	/* host side */
	struct sockaddr_in addr;
	bool local;          /* the host's own seat */
	uint32_t token;
	uint32_t lastseq;    /* newest intent applied */
	double lastheard;
	bool startacked;
} seat_t;

typedef struct {
	int id, seat;
	char name[64];
	char text[256];
} chatline_t;

typedef struct {
	uint32_t seq;
	char json[600];
	double nextsend;
} intent_t;

typedef struct {
	char id[32], name[64];
	char startcmds[512];
	int numteams;
} lobbymode_t;

static struct {
	lobby_phase_t phase;
	bool host;
	char network[8];
	uint32_t sid;
	int revision;
	int localseat;
	char status[128], error[160];
	char roomcode[8];
	char map[64];
	bool mapchosen;  /* the host picked the map; a ruleset change keeps it */
	char ruleset[32];
	char timelimit[16], fraglimit[16];
	/* the server the lobby starts (docs/spec/lobby.md, "Hosting options") */
	char hostname[64];
	char password[64];    /* host: the lobby's and the server's password */
	bool locked;          /* guests: the host has a password */
	int bots;             /* bots besides the lobby's players; -1 fills every free slot */
	bool listed;          /* sv_public 1 */
	bool dedicated;       /* a dedicated server on this machine instead of a listen server */
	bool dedpending;      /* host: waiting for the local server to stop before starting it */
	int dedport;
	char rconpass[20];
	char dedcmds[1536];
	double nextconnect;
	/* guests: a password to send, and whether the host asked for one */
	char joinpass[64];
	bool needpass;
	double countdown_end;
	double settle_until;
	seat_t seats[LOBBY_SEATS];
	int max;
	chatline_t chat[LOBBY_CHAT];
	int numchat, nextchat;
	/* the socket */
	int sock;
	struct sockaddr_in hostaddr; /* guests: where the host is */
	char jointarget[80];
	bool livejoin;   /* joining by room code over Bulwark Live: waiting for the party */
	double livejoin_until;
	int joinattempts;
	double nextjoin, nextping, nextsnap, lastfromhost;
	uint32_t token;              /* guests: from the host's accept */
	uint32_t nextseq;
	intent_t intents[LOBBY_MAX_INTENTS];
	int numintents;
	/* starting */
	uint32_t startnonce, seenstart;
	double startdeadline;
	bool startsent;
	double closing_until;
	int published_rev;
} L = {.sock = -1, .localseat = -1};

/* discovered lobbies, for gethostcache */
#define MAX_FOUND 32
static struct {
	char addr[64], name[64], map[64], code[8];
	int players, max;
} found[MAX_FOUND];
static int numfound;

static lobbymode_t modes[16];
static int nummodes;

static cvar_t *lobby_port, *lobby_readytime, *lobby_maxplayers, *lobby_defaultmap, *lobby_defaultruleset;
static cvar_t *lobby_snapshot;
static cvar_t *lobby_hostname, *lobby_password, *lobby_bots, *lobby_public, *lobby_servertype;

static const char *const phase_names[] = {"IDLE", "CREATING", "JOINING", "WAITING",
                                          "COUNTDOWN", "STARTING", "INGAME", "CLOSING"};

lobby_phase_t Lobby_Phase(void) { return L.phase; }
bool Lobby_Active(void) { return L.phase != LOBBY_IDLE; }
bool Lobby_AskingPassword(void) { return L.needpass && L.jointarget[0]; }

/* --- the socket ---------------------------------------------------------------------- */

static bool sock_open(int port)
{
	struct sockaddr_in a;
	int one = 1;
	if (L.sock >= 0)
		close(L.sock);
	L.sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (L.sock < 0)
		return false;
	setsockopt(L.sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	fcntl(L.sock, F_SETFL, fcntl(L.sock, F_GETFL) | O_NONBLOCK);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)port);
	if (bind(L.sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
		Con_Printf("lobby: can't use UDP port %d: %s\n", port, strerror(errno));
		close(L.sock);
		L.sock = -1;
		return false;
	}
	return true;
}

static void sock_close(void)
{
	if (L.sock >= 0)
		close(L.sock);
	L.sock = -1;
}

/* A Bulwark Live peer is kept in a sockaddr_in with this family and the
 * peer's id in sin_addr, so seats and the host address work unchanged. */
#define LIVE_FAMILY 0xB7
/* a Live datagram carries about 1400 bytes; longer lobby messages are cut
 * into pieces: "u16 message, u8 index, u8 count" then the bytes */
#define LIVE_PIECE 1200

static bool is_live(const struct sockaddr_in *a) { return a->sin_family == LIVE_FAMILY; }

static void live_addr(uint32_t id, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = LIVE_FAMILY;
	out->sin_addr.s_addr = id;
	out->sin_port = LIVE_VPORT_LOBBY;
}

static void raw_send(const struct sockaddr_in *to, const char *data, size_t len)
{
	static uint16_t msgid;
	if (!is_live(to)) {
		if (L.sock >= 0)
			sendto(L.sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
		return;
	}
	int count = (int)((len + LIVE_PIECE - 1) / LIVE_PIECE);
	if (count < 1 || count > 255)
		return;
	msgid++;
	for (int i = 0; i < count; i++) {
		uint8_t piece[LIVE_PIECE + 4];
		size_t n = BW_MIN((size_t)LIVE_PIECE, len - (size_t)i * LIVE_PIECE);
		piece[0] = (uint8_t)(msgid >> 8);
		piece[1] = (uint8_t)msgid;
		piece[2] = (uint8_t)i;
		piece[3] = (uint8_t)count;
		memcpy(piece + 4, data + (size_t)i * LIVE_PIECE, n);
		Live_SendLobby(to->sin_addr.s_addr, piece, n + 4);
	}
}

static void send_to(const struct sockaddr_in *to, const json_t *msg)
{
	strbuf_t sb = {0};
	if (L.sock < 0 && !is_live(to))
		return;
	SB_AppendStr(&sb, LOBBY_MAGIC);
	JSON_Write(&sb, msg, -1);
	raw_send(to, sb.data, sb.len);
	SB_Free(&sb);
}

static bool resolve(const char *target, int defport, struct sockaddr_in *out)
{
	netadr_t a;
	if (!Str_ICmp(target, "localhost"))
		target = "127.0.0.1";
	if (!NET_StringToAdr(target, &a, defport) || a.type != NA_IP4)
		return false;
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	memcpy(&out->sin_addr, a.ip, 4);
	out->sin_port = htons(a.port);
	return true;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_family == b->sin_family && a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static uint32_t random32(void) { return ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)(Sys_Seconds() * 1000); }

/* --- state ---------------------------------------------------------------------------- */

static void changed(void) { L.revision++; }

static int occupied(void)
{
	int n = 0;
	for (int i = 0; i < L.max; i++)
		n += L.seats[i].used;
	return n;
}

static bool all_ready(void)
{
	int n = 0;
	for (int i = 0; i < L.max; i++) {
		if (!L.seats[i].used)
			continue;
		if (!L.seats[i].ready)
			return false;
		n++;
	}
	return n > 0;
}

/* back from the countdown: everyone has to ready again */
static void cancel_countdown(const char *why)
{
	if (L.phase != LOBBY_COUNTDOWN)
		return;
	L.phase = LOBBY_WAITING;
	L.countdown_end = 0;
	for (int i = 0; i < L.max; i++)
		L.seats[i].ready = false;
	Str_Copy(L.status, why, sizeof(L.status));
	changed();
}

static void reset_session(void)
{
	sock_close();
	memset(L.seats, 0, sizeof(L.seats));
	L.phase = LOBBY_IDLE;
	L.host = false;
	L.localseat = -1;
	L.numchat = 0;
	L.numintents = 0;
	L.livejoin = false;
	L.roomcode[0] = 0;
	L.status[0] = 0;
	L.countdown_end = 0;
	L.startsent = false;
	L.revision++;
}

/* ends the session with a reason everyone sees for a moment */
static void close_with(const char *error)
{
	if (L.phase == LOBBY_IDLE)
		return;
	Str_Copy(L.error, error ? error : "", sizeof(L.error));
	if (error && *error)
		Con_Printf("lobby: %s\n", error);
	L.phase = LOBBY_CLOSING;
	L.closing_until = Sys_Seconds() + 1.5;
	sock_close();
	changed();
}

static void add_chat(int id, int seat, const char *name, const char *text)
{
	chatline_t *c;
	for (int i = 0; i < L.numchat; i++)
		if (L.chat[i].id == id)
			return;
	if (L.numchat == LOBBY_CHAT) {
		memmove(L.chat, L.chat + 1, sizeof(chatline_t) * (LOBBY_CHAT - 1));
		L.numchat--;
	}
	c = &L.chat[L.numchat++];
	c->id = id;
	c->seat = seat;
	Str_Copy(c->name, name, sizeof(c->name));
	Str_Copy(c->text, text, sizeof(c->text));
	Con_Printf("%s: %s\n", name, text);
}

/* --- snapshots ------------------------------------------------------------------------ */

static bool can_ready(void) { return L.localseat >= 0 && (L.phase == LOBBY_WAITING || L.phase == LOBBY_COUNTDOWN); }
static bool can_start(void) { return L.host && L.phase == LOBBY_WAITING; }
static bool can_cancel(void) { return L.host && L.phase == LOBBY_COUNTDOWN; }
static bool can_change(void) { return L.host && (L.phase == LOBBY_WAITING || L.phase == LOBBY_COUNTDOWN); }
static bool can_chat(void)
{
	return L.localseat >= 0 && (L.phase == LOBBY_WAITING || L.phase == LOBBY_COUNTDOWN || L.phase == LOBBY_STARTING);
}

/* the server's name when the host gave none */
static const char *server_name(void)
{
	static char name[96];
	if (L.hostname[0])
		return L.hostname;
	snprintf(name, sizeof(name), "%s's server", L.seats[0].used && L.seats[0].name[0] ? L.seats[0].name : "Stiletto");
	return name;
}

/* for_host: the host's own page, which may see the password */
static json_t *settings_json(bool for_host)
{
	json_t *s = JSON_New(JSON_OBJECT);
	JSON_SetString(s, "ruleset", L.ruleset);
	JSON_SetString(s, "servertype", L.dedicated ? "DEDICATED SERVER" : "LISTEN SERVER");
	JSON_SetString(s, "timelimit", L.timelimit);
	JSON_SetString(s, "fraglimit", L.fraglimit);
	JSON_SetString(s, "hostname", L.host ? server_name() : L.hostname);
	JSON_SetNumber(s, "maxplayers", L.max);
	JSON_SetNumber(s, "bots", L.bots);
	JSON_SetBool(s, "public", L.listed);
	JSON_SetBool(s, "password", L.host ? L.password[0] != 0 : L.locked);
	if (for_host)
		JSON_SetString(s, "passwordText", L.password);
	return s;
}

/* what this engine's lobby can host, for the page's server settings */
static json_t *hosting_json(void)
{
	json_t *h = JSON_New(JSON_OBJECT), *types = JSON_New(JSON_ARRAY);
	cvar_t *ls = Cvar_Find("live_server");
	JSON_Append(types, JSON_NewString("listen"));
	if (DedLaunch_Supported())
		JSON_Append(types, JSON_NewString("dedicated"));
	JSON_Set(h, "serverTypes", types);
	JSON_SetNumber(h, "maxSeats", LOBBY_SEATS);
	JSON_SetBool(h, "online", Live_Connected());
	JSON_SetBool(h, "onlineConfigured", ls && ls->string[0]);
	JSON_SetBool(h, "dedicatedRunning", DedLaunch_Running());
	return h;
}

static json_t *players_json(void)
{
	json_t *arr = JSON_New(JSON_ARRAY);
	for (int i = 0; i < L.max; i++) {
		json_t *p;
		if (!L.seats[i].used)
			continue;
		p = JSON_New(JSON_OBJECT);
		JSON_SetNumber(p, "seat", i);
		JSON_SetString(p, "name", L.seats[i].name);
		JSON_SetBool(p, "host", i == 0);
		JSON_SetBool(p, "ready", L.seats[i].ready);
		JSON_SetBool(p, "connected", L.seats[i].connected || L.seats[i].local);
		JSON_Append(arr, p);
	}
	return arr;
}

static json_t *chat_json(void)
{
	json_t *arr = JSON_New(JSON_ARRAY);
	for (int i = 0; i < L.numchat; i++) {
		json_t *c = JSON_New(JSON_OBJECT);
		JSON_SetNumber(c, "id", L.chat[i].id);
		JSON_SetNumber(c, "seat", L.chat[i].seat);
		JSON_SetString(c, "name", L.chat[i].name);
		JSON_SetString(c, "text", L.chat[i].text);
		JSON_Append(arr, c);
	}
	return arr;
}

/* --- what the host can pick: maps and rulesets ------------------------------------------ */

/* The game names its lobby's choices in scripts/lobby.txt, one per line:
 *   map <id> "<title>"
 *   ruleset <ID> "<title>" [<map the ruleset picks>]
 * A listed map whose maps/<id>.bsp (or .scene) isn't there is left out. With
 * no file, every map in maps/ is offered under its own name, with the
 * built-in rulesets below. MenuQC's lobby_registermode adds rulesets either
 * way. The page shows these lists ("maps", "rulesets" in getlobby) and falls
 * back to its own when they are missing. */
#define LOBBY_MAX_MAPS 64
#define LOBBY_MAX_RULESETS 24

typedef struct {
	char id[64], name[64];
} lobbymap_t;

typedef struct {
	char id[32], name[64], map[64];
	bool available; /* its progs are in this game */
	bool seatbots;  /* the rules fill their own seats with bots (arena_bots) */
} lobbyruleset_t;

static const struct {
	const char *id, *name;
	bool seatbots;
} builtin_rulesets[] = {
	{"DUEL", "Duel", false},       {"DEATHMATCH", "Deathmatch", false}, {"TEAMDM", "Team Deathmatch", false},
	{"DOMINATION", "Domination", false}, {"FREEZETAG", "Freeze Tag", true}, {"ARCHSTILETTO", "Archstiletto", true},
};

static struct {
	int fsgen;
	bool loaded, fromfile;
	lobbymap_t maps[LOBBY_MAX_MAPS];
	int nummaps;
	lobbyruleset_t rulesets[LOBBY_MAX_RULESETS];
	int numrulesets;
} cat = {.fsgen = -1};

static bool map_present(const char *id)
{
	return FS_FileExists(va("maps/%s.bsp", id)) || FS_FileExists(va("maps/%s.scene", id));
}

/* Nuclide runs a ruleset from progs/<id in lower case>.dat; without it the
 * server has no rules and nobody spawns, so such a ruleset can't be picked */
static bool progs_present(const char *id)
{
	char path[64];
	int n = snprintf(path, sizeof(path), "progs/%s.dat", id);
	for (int i = 6; i < n; i++)
		path[i] = (char)tolower((unsigned char)path[i]);
	return FS_FileExists(path);
}

static void add_map(const char *id, const char *name)
{
	if (cat.nummaps == LOBBY_MAX_MAPS || !*id)
		return;
	for (int i = 0; i < cat.nummaps; i++)
		if (!Str_ICmp(cat.maps[i].id, id))
			return;
	Str_Copy(cat.maps[cat.nummaps].id, id, sizeof(cat.maps[0].id));
	Str_Copy(cat.maps[cat.nummaps].name, *name ? name : id, sizeof(cat.maps[0].name));
	cat.nummaps++;
}

static void add_ruleset(const char *id, const char *name, const char *map, bool seatbots)
{
	lobbyruleset_t *r = NULL;
	char up[32];
	int n = 0;
	for (; id[n] && n < (int)sizeof(up) - 1; n++)
		up[n] = (char)toupper((unsigned char)id[n]);
	up[n] = 0;
	if (!n)
		return;
	for (int i = 0; i < cat.numrulesets; i++)
		if (!strcmp(cat.rulesets[i].id, up))
			r = &cat.rulesets[i];
	if (!r) {
		if (cat.numrulesets == LOBBY_MAX_RULESETS)
			return;
		r = &cat.rulesets[cat.numrulesets++];
	}
	Str_Copy(r->id, up, sizeof(r->id));
	Str_Copy(r->name, *name ? name : up, sizeof(r->name));
	Str_Copy(r->map, map, sizeof(r->map));
	r->available = progs_present(up);
	r->seatbots = seatbots;
}

static void list_map(const char *name, const char *searchpath, void *ctx)
{
	char id[64];
	const char *base = strrchr(name, '/');
	size_t n;
	(void)searchpath;
	(void)ctx;
	base = base ? base + 1 : name;
	n = strlen(base);
	if (n < 5 || n - 4 >= sizeof(id) || Str_ICmp(base + n - 4, ".bsp"))
		return;
	memcpy(id, base, n - 4);
	id[n - 4] = 0;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_')
			return;
	add_map(id, id);
}

static int cmp_maps(const void *a, const void *b)
{
	return Str_ICmp(((const lobbymap_t *)a)->id, ((const lobbymap_t *)b)->id);
}

static void parse_catalog(const char *text)
{
	char line[512], kind[16], id[64], name[64], map[64], flag[32];
	while (*text) {
		const char *end = strchr(text, '\n'), *p;
		size_t n = end ? (size_t)(end - text) : strlen(text);
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, text, n);
		line[n] = 0;
		text += end ? (size_t)(end - text) + 1 : strlen(text);
		kind[0] = id[0] = name[0] = map[0] = flag[0] = 0;
		if (!(p = COM_Parse(line, kind, sizeof(kind))))
			continue;
		if ((p = COM_Parse(p, id, sizeof(id))))
			if ((p = COM_Parse(p, name, sizeof(name))))
				if ((p = COM_Parse(p, map, sizeof(map))))
					COM_Parse(p, flag, sizeof(flag));
		if (!Str_ICmp(kind, "map")) {
			if (map_present(id))
				add_map(id, name);
			else
				Con_DPrintf("lobby: scripts/lobby.txt lists map %s, which isn't in this game\n", id);
		} else if (!Str_ICmp(kind, "ruleset"))
			add_ruleset(id, name, map, !Str_ICmp(flag, "seatbots"));
	}
}

/* (re)built when the lobby opens or the search path changes */
static void load_catalog(void)
{
	char *text;
	if (cat.loaded && cat.fsgen == FS_Generation())
		return;
	cat.loaded = true;
	cat.fsgen = FS_Generation();
	cat.nummaps = cat.numrulesets = 0;
	text = FS_LoadFile("scripts/lobby.txt", NULL);
	cat.fromfile = text != NULL;
	if (text) {
		parse_catalog(text);
		Mem_Free(text);
	}
	if (!cat.nummaps) {
		FS_List("maps/*.bsp", true, list_map, NULL);
		qsort(cat.maps, (size_t)cat.nummaps, sizeof(cat.maps[0]), cmp_maps);
	}
	if (!cat.numrulesets)
		for (size_t i = 0; i < BW_ARRAY_COUNT(builtin_rulesets); i++)
			add_ruleset(builtin_rulesets[i].id, builtin_rulesets[i].name, "", builtin_rulesets[i].seatbots);
}

static const lobbyruleset_t *find_ruleset(const char *id)
{
	load_catalog();
	for (int i = 0; i < cat.numrulesets; i++)
		if (!Str_ICmp(cat.rulesets[i].id, id))
			return &cat.rulesets[i];
	return NULL;
}

/* A game with no rules progs at all (not Nuclide) runs any ruleset alike */
static bool ruleset_runs(const char *id)
{
	const lobbyruleset_t *r = find_ruleset(id); /* loads the catalog; its answer is cached */
	if (r ? r->available : progs_present(id))
		return true;
	for (int i = 0; i < cat.numrulesets; i++)
		if (cat.rulesets[i].available)
			return false;
	return true;
}

/* the map a ruleset starts on: lobby_defaultmap when set, else the ruleset's
 * own, else the first listed (docs/spec/lobby.md) */
static const char *ruleset_map(const char *id)
{
	const lobbyruleset_t *r = find_ruleset(id);
	load_catalog();
	if (*lobby_defaultmap->string)
		return lobby_defaultmap->string;
	if (r && r->map[0] && map_present(r->map))
		return r->map;
	return cat.nummaps ? cat.maps[0].id : "start";
}

static json_t *maps_json(void)
{
	json_t *arr = JSON_New(JSON_ARRAY);
	load_catalog();
	for (int i = 0; i < cat.nummaps; i++) {
		json_t *m = JSON_New(JSON_OBJECT);
		JSON_SetString(m, "id", cat.maps[i].id);
		JSON_SetString(m, "name", cat.maps[i].name);
		JSON_Append(arr, m);
	}
	return arr;
}

static json_t *rulesets_json(void)
{
	json_t *arr = JSON_New(JSON_ARRAY);
	load_catalog();
	for (int i = 0; i < cat.numrulesets; i++) {
		json_t *r = JSON_New(JSON_OBJECT);
		JSON_SetString(r, "id", cat.rulesets[i].id);
		JSON_SetString(r, "name", cat.rulesets[i].name);
		JSON_SetString(r, "map", cat.rulesets[i].map);
		JSON_SetBool(r, "available", ruleset_runs(cat.rulesets[i].id));
		JSON_SetBool(r, "seatbots", cat.rulesets[i].seatbots);
		JSON_Append(arr, r);
	}
	for (int i = 0; i < nummodes; i++) {
		json_t *r;
		if (find_ruleset(modes[i].id))
			continue;
		r = JSON_New(JSON_OBJECT);
		JSON_SetString(r, "id", modes[i].id);
		JSON_SetString(r, "name", modes[i].name[0] ? modes[i].name : modes[i].id);
		JSON_SetString(r, "map", "");
		JSON_SetBool(r, "available", ruleset_runs(modes[i].id));
		JSON_Append(arr, r);
	}
	return arr;
}

/* what the lobby page reads (getlobby) */
static json_t *local_snapshot(void)
{
	json_t *j = JSON_New(JSON_OBJECT);
	char sid[16];
	double now = Sys_Seconds();
	bool active = L.phase != LOBBY_IDLE;
	snprintf(sid, sizeof(sid), "%08x", L.sid);
	JSON_SetBool(j, "active", active);
	JSON_SetString(j, "sessionId", active ? sid : "");
	JSON_SetNumber(j, "revision", L.revision);
	JSON_SetNumber(j, "uiVersion", 1);
	JSON_SetString(j, "network", L.network);
	JSON_SetBool(j, "isHost", L.host);
	JSON_SetNumber(j, "localSeat", L.localseat);
	JSON_SetString(j, "phase", phase_names[L.phase]);
	JSON_SetString(j, "status", L.status);
	JSON_SetString(j, "error", L.error);
	JSON_SetString(j, "roomCode", L.roomcode);
	JSON_SetString(j, "map", active ? L.map : "");
	JSON_SetNumber(j, "max", L.max);
	JSON_SetNumber(j, "countdownEnd", L.phase == LOBBY_COUNTDOWN ? L.countdown_end : 0);
	JSON_SetNumber(j, "serverTime", now);
	JSON_SetBool(j, "canReady", active && can_ready());
	JSON_SetBool(j, "canStart", active && can_start());
	JSON_SetBool(j, "canCancel", active && can_cancel());
	JSON_SetBool(j, "canChangeMap", active && can_change());
	JSON_SetBool(j, "canLeave", active);
	JSON_SetBool(j, "canChat", active && can_chat());
	JSON_Set(j, "settings", active ? settings_json(L.host) : JSON_New(JSON_OBJECT));
	JSON_Set(j, "hosting", hosting_json());
	JSON_SetBool(j, "needPassword", L.needpass && (L.phase == LOBBY_IDLE || L.phase == LOBBY_CLOSING));
	JSON_SetString(j, "joinTarget", L.needpass ? L.jointarget : "");
	/* an "online" lobby without Bulwark Live is one this network can join only */
	JSON_SetString(j, "warning", active && L.host && !strcmp(L.network, "online") && !Live_Connected()
	                                 ? "Online services are not connected in this build: this room code works on this network only."
	                                 : "");
	JSON_Set(j, "players", active ? players_json() : JSON_New(JSON_ARRAY));
	JSON_Set(j, "maps", maps_json());
	JSON_Set(j, "rulesets", rulesets_json());
	return j;
}

/* the host's state as a guest needs it; ack is that guest's newest intent */
static void send_snapshot(int seat)
{
	json_t *j = JSON_New(JSON_OBJECT);
	double now = Sys_Seconds();
	JSON_SetString(j, "t", "snap");
	JSON_SetNumber(j, "sid", L.sid);
	JSON_SetNumber(j, "rev", L.revision);
	JSON_SetString(j, "phase", phase_names[L.phase]);
	JSON_SetString(j, "status", L.status);
	JSON_SetString(j, "map", L.map);
	JSON_SetString(j, "code", L.roomcode);
	JSON_SetString(j, "network", L.network);
	JSON_SetNumber(j, "max", L.max);
	JSON_SetNumber(j, "remaining", L.phase == LOBBY_COUNTDOWN ? BW_MAX(0, L.countdown_end - now) : -1);
	JSON_SetNumber(j, "ack", L.seats[seat].lastseq);
	JSON_SetNumber(j, "seat", seat);
	JSON_Set(j, "settings", settings_json(false));
	JSON_Set(j, "players", players_json());
	JSON_Set(j, "chat", chat_json());
	send_to(&L.seats[seat].addr, j);
	JSON_Free(j);
}

static void broadcast_snapshot(void)
{
	for (int i = 1; i < L.max; i++)
		if (L.seats[i].used && !L.seats[i].local)
			send_snapshot(i);
	L.nextsnap = Sys_Seconds() + 1.0;
}

static void send_simple(const struct sockaddr_in *to, const char *type, const char *key, const char *value)
{
	json_t *j = JSON_New(JSON_OBJECT);
	JSON_SetString(j, "t", type);
	JSON_SetNumber(j, "sid", L.sid);
	if (key)
		JSON_SetString(j, key, value);
	send_to(to, j);
	JSON_Free(j);
}

/* mirrors for the menu module's lobby screen and for tests */
static void publish(void)
{
	strbuf_t sb = {0};
	json_t *j;
	if (L.published_rev == L.revision && L.phase != LOBBY_COUNTDOWN)
		return;
	L.published_rev = L.revision;
	j = local_snapshot();
	JSON_Write(&sb, j, -1);
	JSON_Free(j);
	Cvar_SetDirect(lobby_snapshot, sb.data ? sb.data : "");
	if (developer && L.phase != LOBBY_IDLE) {
		FILE *f = fopen("lobby_snapshot.json", "w");
		if (f) {
			fwrite(sb.data, 1, sb.len, f);
			fclose(f);
		}
	}
	SB_Free(&sb);
	Cvar_Set("lobby_room_code", L.roomcode);
	Cvar_Set("lobby_host_name", L.seats[0].used ? L.seats[0].name : "");
	Cvar_Set("lobby_state_str", L.phase == LOBBY_IDLE ? "" : phase_names[L.phase]);
	Cvar_Set("lobby_player_count", va("%d", L.phase == LOBBY_IDLE ? 0 : occupied()));
	Cvar_Set("lobby_max_players", va("%d", L.max));
	for (int i = 0; i < 4; i++) {
		const seat_t *s = &L.seats[i];
		Cvar_Set(va("lobby_player%d_name", i + 1), s->used ? va("%s%s", s->ready ? "[READY] " : "", s->name) : "");
	}
}

/* --- modes ---------------------------------------------------------------------------- */

static const lobbymode_t *find_mode(const char *id)
{
	for (int i = 0; i < nummodes; i++)
		if (!Str_ICmp(modes[i].id, id))
			return &modes[i];
	return NULL;
}

static bool valid_ruleset(const char *id)
{
	return find_ruleset(id) != NULL || find_mode(id) != NULL;
}

/* lobby_defaultruleset when it can run, else the first listed that can */
static const char *default_ruleset(void)
{
	const char *want = *lobby_defaultruleset->string ? lobby_defaultruleset->string : "DUEL";
	if (valid_ruleset(want) && ruleset_runs(want))
		return want;
	for (int i = 0; i < cat.numrulesets; i++)
		if (cat.rulesets[i].available) {
			Con_Printf("^3lobby: ruleset %s isn't in this game (its progs are missing); the lobby starts on %s\n", want,
			           cat.rulesets[i].name);
			return cat.rulesets[i].id;
		}
	return want;
}

static void bi_lobby_registermode(qcvm_t *vm)
{
	lobbymode_t *m;
	const char *id = QC_PSTR(vm, 0);
	if (!*id)
		return;
	m = (lobbymode_t *)find_mode(id);
	if (!m) {
		if (nummodes == (int)BW_ARRAY_COUNT(modes))
			return;
		m = &modes[nummodes++];
		memset(m, 0, sizeof(*m));
	}
	Str_Copy(m->id, id, sizeof(m->id));
	Str_Copy(m->name, QC_PSTR(vm, 1), sizeof(m->name));
	Str_Copy(m->startcmds, vm->argc > 2 ? QC_PSTR(vm, 2) : "", sizeof(m->startcmds));
}

static void bi_lobby_addteam(qcvm_t *vm)
{
	lobbymode_t *m = (lobbymode_t *)find_mode(QC_PSTR(vm, 0));
	if (m)
		m->numteams++; /* the HTML lobby shows one list; teams are kept for counting */
}

const qc_builtin_t lobby_builtins[] = {
	{"lobby_registermode", bi_lobby_registermode}, {"lobby_addteam", bi_lobby_addteam}, {NULL, NULL}};

/* --- creating and joining ------------------------------------------------------------- */

/* a server name or password that can go into a config and a userinfo
 * string: printable, no quotes, backslashes or semicolons, under 64 bytes */
static bool text_ok(const char *t)
{
	if (strlen(t) > 63)
		return false;
	for (const unsigned char *p = (const unsigned char *)t; *p; p++)
		if (*p < 0x20 || *p == '"' || *p == '\\' || *p == ';' || *p == 0x7f)
			return false;
	return true;
}

static void local_name(char *out, size_t size)
{
	cvar_t *n = Cvar_Find("name");
	Str_Copy(out, n && *n->string ? n->string : "player", size);
}

static void new_session(void)
{
	reset_session();
	L.sid = random32();
	L.max = BW_CLAMP(lobby_maxplayers->ivalue, 1, LOBBY_SEATS);
	L.error[0] = 0;
	cat.loaded = false; /* a new lobby looks at the game's files again */
	Str_Copy(L.ruleset, default_ruleset(), sizeof(L.ruleset));
	Str_Copy(L.timelimit, "0", sizeof(L.timelimit));
	Str_Copy(L.fraglimit, "0", sizeof(L.fraglimit));
	/* the hosting options start from the lobby_* cvars (the console can preset them) */
	Str_Copy(L.hostname, text_ok(lobby_hostname->string) ? lobby_hostname->string : "", sizeof(L.hostname));
	Str_Copy(L.password, text_ok(lobby_password->string) ? lobby_password->string : "", sizeof(L.password));
	L.locked = false;
	L.bots = !Str_ICmp(lobby_bots->string, "fill") ? -1 : BW_CLAMP(lobby_bots->ivalue, 0, LOBBY_SEATS - 1);
	L.listed = lobby_public->ivalue != 0;
	L.dedicated = !Str_ICmp(lobby_servertype->string, "dedicated") && DedLaunch_Supported();
	L.dedpending = false;
	L.dedport = 0;
	L.joinpass[0] = 0;
	L.needpass = false;
	for (char *p = L.ruleset; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	/* lobby_defaultmap, when set, wins; otherwise the ruleset's own map */
	Str_Copy(L.map, ruleset_map(L.ruleset), sizeof(L.map));
	L.mapchosen = false;
}

static void LobbyCreate_f(int argc, const char **argv)
{
	const char *kind = argc > 1 ? argv[1] : "lan";
	if (!strncmp(argv[0], "lobby_create_", 13))
		kind = argv[0] + 13;
	if (Str_ICmp(kind, "lan") && Str_ICmp(kind, "online") && Str_ICmp(kind, "offline")) {
		Con_Printf("usage: lobby_create <lan|online|offline>\n");
		return;
	}
	new_session();
	Str_Copy(L.network, kind, sizeof(L.network));
	for (char *p = L.network; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	if (strcmp(L.network, "offline") && !sock_open(lobby_port->ivalue)) {
		close_with(va("Couldn't open the lobby port %d", lobby_port->ivalue));
		return;
	}
	if (!strcmp(L.network, "online") && Live_Connected()) {
		/* the room code is the Live party's; Lobby_Frame picks it up */
		Cbuf_InsertText(va("live_party %d 1\n", L.max));
	} else if (!strcmp(L.network, "online")) {
		static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		for (int i = 0; i < 6; i++)
			L.roomcode[i] = letters[rand() % (int)(sizeof(letters) - 1)];
		L.roomcode[6] = 0;
	}
	L.host = true;
	L.localseat = 0;
	L.seats[0].used = true;
	L.seats[0].local = true;
	L.seats[0].connected = true;
	local_name(L.seats[0].name, sizeof(L.seats[0].name));
	L.phase = LOBBY_WAITING;
	Str_Copy(L.status, "Waiting for players", sizeof(L.status));
	changed();
	Con_Printf("lobby: created (%s)%s%s\n", L.network, L.roomcode[0] ? ", room code " : "", L.roomcode);
}

static bool looks_like_address(const char *t)
{
	return strchr(t, '.') || strchr(t, ':') || !Str_ICmp(t, "localhost");
}

static void discover(double wait);

static void LobbyJoin_f(int argc, const char **argv)
{
	char target[80];
	if (argc < 2) {
		Con_Printf("usage: lobby_join <address|room code> [password]\n");
		return;
	}
	if (argc > 2 && !text_ok(argv[2])) {
		Con_Printf("lobby_join: a password can't have quotes, backslashes or semicolons\n");
		return;
	}
	new_session();
	Str_Copy(L.jointarget, argv[1], sizeof(L.jointarget));
	Str_Copy(L.joinpass, argc > 2 ? argv[2] : "", sizeof(L.joinpass));
	Str_Copy(L.network, looks_like_address(argv[1]) ? "lan" : "online", sizeof(L.network));
	if (!sock_open(0)) {
		close_with("Couldn't open a socket");
		return;
	}
	Str_Copy(target, argv[1], sizeof(target));
	if (!looks_like_address(target)) {
		/* a room code: a lobby on this network advertising it */
		bool ok = false;
		discover(0.3);
		for (int i = 0; i < numfound && !ok; i++)
			if (!Str_ICmp(found[i].code, target)) {
				Str_Copy(target, found[i].addr, sizeof(target));
				ok = true;
			}
		if (!ok && Live_Connected()) {
			/* not on this network: join the Live party with that code, and
			 * then its leader's lobby */
			Str_Copy(L.roomcode, argv[1], sizeof(L.roomcode));
			for (char *p = L.roomcode; *p; p++)
				*p = (char)toupper((unsigned char)*p);
			Cbuf_InsertText(va("live_join %s\n", L.roomcode));
			L.livejoin = true;
			L.livejoin_until = Sys_Seconds() + 10;
			L.phase = LOBBY_JOINING;
			L.joinattempts = 0;
			L.nextjoin = 0;
			L.lastfromhost = Sys_Seconds();
			snprintf(L.status, sizeof(L.status), "Finding room %s...", L.roomcode);
			changed();
			return;
		}
		if (!ok) {
			cvar_t *ls = Cvar_Find("live_server");
			close_with(va("No lobby with room code %s was found on this network%s", argv[1],
			              ls && ls->string[0] ? " (Bulwark Live isn't connected)"
			                                  : ". Online services are not connected in this build"));
			return;
		}
		Str_Copy(L.roomcode, argv[1], sizeof(L.roomcode));
	}
	if (!resolve(target, lobby_port->ivalue, &L.hostaddr)) {
		close_with(va("Unknown address %s", target));
		return;
	}
	L.phase = LOBBY_JOINING;
	L.joinattempts = 0;
	L.nextjoin = 0;
	L.lastfromhost = Sys_Seconds();
	snprintf(L.status, sizeof(L.status), "Joining %s...", argv[1]);
	changed();
}

/* --- guests' intents ------------------------------------------------------------------- */

static void queue_intent(const char *kind, const char *key, const char *value)
{
	intent_t *in;
	json_t *j;
	strbuf_t sb = {0};
	if (L.numintents == LOBBY_MAX_INTENTS)
		return;
	in = &L.intents[L.numintents++];
	in->seq = ++L.nextseq;
	in->nextsend = 0;
	j = JSON_New(JSON_OBJECT);
	JSON_SetString(j, "t", "intent");
	JSON_SetNumber(j, "sid", L.sid);
	JSON_SetNumber(j, "tok", L.token);
	JSON_SetNumber(j, "seq", in->seq);
	JSON_SetString(j, "kind", kind);
	if (key)
		JSON_SetString(j, key, value);
	JSON_Write(&sb, j, -1);
	JSON_Free(j);
	Str_Copy(in->json, sb.data, sizeof(in->json));
	SB_Free(&sb);
}

static void send_intents(void)
{
	double now = Sys_Seconds();
	for (int i = 0; i < L.numintents; i++) {
		intent_t *in = &L.intents[i];
		if (now < in->nextsend)
			continue;
		in->nextsend = now + 0.25;
		{
			strbuf_t sb = {0};
			SB_AppendStr(&sb, LOBBY_MAGIC);
			SB_AppendStr(&sb, in->json);
			raw_send(&L.hostaddr, sb.data, sb.len);
			SB_Free(&sb);
		}
	}
}

static void acked(uint32_t ack)
{
	int out = 0;
	for (int i = 0; i < L.numintents; i++)
		if (L.intents[i].seq > ack)
			L.intents[out++] = L.intents[i];
	L.numintents = out;
}

/* --- the host's rules ------------------------------------------------------------------ */

static int next_chat_id(void) { return ++L.nextchat; }

static void host_ready(int seat, bool ready)
{
	if (!L.seats[seat].used || L.seats[seat].ready == ready)
		return;
	if (!ready)
		cancel_countdown("Countdown cancelled");
	L.seats[seat].ready = ready;
	changed();
}

static void host_chat(int seat, const char *text)
{
	if (!*text)
		return;
	add_chat(next_chat_id(), seat, L.seats[seat].name, text);
	changed();
}

static void remove_seat(int seat)
{
	if (!L.seats[seat].used)
		return;
	Con_Printf("lobby: %s left\n", L.seats[seat].name);
	memset(&L.seats[seat], 0, sizeof(L.seats[seat]));
	cancel_countdown("A player left");
	L.settle_until = Sys_Seconds() + 0.5;
	changed();
}

static void begin_start(void);

static void host_join(const struct sockaddr_in *from, const json_t *msg)
{
	json_t *reply;
	int seat = -1;
	const char *name = JSON_GetString(msg, "name", "player");
	if ((int)JSON_GetNumber(msg, "proto", 0) != 1) {
		send_simple(from, "reject", "reason", "Protocol version mismatch");
		return;
	}
	for (int i = 1; i < L.max; i++)
		if (L.seats[i].used && same_addr(&L.seats[i].addr, from))
			seat = i; /* a repeated request: the same seat */
	if (seat < 0) {
		if (L.phase != LOBBY_WAITING) {
			send_simple(from, "reject", "reason", "The game is starting");
			return;
		}
		if (L.password[0] && strcmp(JSON_GetString(msg, "pass", ""), L.password)) {
			json_t *no = JSON_New(JSON_OBJECT);
			bool tried = JSON_GetString(msg, "pass", "")[0] != 0;
			JSON_SetString(no, "t", "reject");
			JSON_SetNumber(no, "sid", L.sid);
			JSON_SetString(no, "reason", tried ? "Wrong password" : "This lobby needs a password");
			JSON_SetBool(no, "needpass", true);
			send_to(from, no);
			JSON_Free(no);
			Con_Printf("lobby: turned %s away (%s password)\n", *name ? name : "player", tried ? "wrong" : "no");
			return;
		}
		for (int i = 1; i < L.max && seat < 0; i++)
			if (!L.seats[i].used)
				seat = i;
		if (seat < 0) {
			send_simple(from, "reject", "reason", "The lobby is full");
			return;
		}
		memset(&L.seats[seat], 0, sizeof(L.seats[seat]));
		L.seats[seat].used = true;
		L.seats[seat].addr = *from;
		L.seats[seat].token = random32() | 1;
		Str_Copy(L.seats[seat].name, *name ? name : "player", sizeof(L.seats[seat].name));
		Con_Printf("lobby: %s joined\n", L.seats[seat].name);
		L.settle_until = Sys_Seconds() + 0.5;
		changed();
	}
	L.seats[seat].connected = true;
	L.seats[seat].lastheard = Sys_Seconds();
	reply = JSON_New(JSON_OBJECT);
	JSON_SetString(reply, "t", "accept");
	JSON_SetNumber(reply, "sid", L.sid);
	JSON_SetNumber(reply, "seat", seat);
	JSON_SetNumber(reply, "tok", L.seats[seat].token);
	send_to(from, reply);
	JSON_Free(reply);
	send_snapshot(seat);
}

static int seat_for(const struct sockaddr_in *from, const json_t *msg)
{
	uint32_t tok = (uint32_t)JSON_GetNumber(msg, "tok", 0);
	for (int i = 1; i < L.max; i++)
		if (L.seats[i].used && L.seats[i].token == tok && same_addr(&L.seats[i].addr, from))
			return i;
	return -1;
}

static void host_intent(int seat, const json_t *msg)
{
	uint32_t seq = (uint32_t)JSON_GetNumber(msg, "seq", 0);
	const char *kind = JSON_GetString(msg, "kind", "");
	seat_t *s = &L.seats[seat];
	if (seq <= s->lastseq) {
		send_snapshot(seat); /* already done: acknowledge again */
		return;
	}
	s->lastseq = seq;
	if (!strcmp(kind, "ready") && (L.phase == LOBBY_WAITING || L.phase == LOBBY_COUNTDOWN))
		host_ready(seat, true);
	else if (!strcmp(kind, "unready") && (L.phase == LOBBY_WAITING || L.phase == LOBBY_COUNTDOWN))
		host_ready(seat, false);
	else if (!strcmp(kind, "chat") && L.phase >= LOBBY_WAITING && L.phase <= LOBBY_STARTING)
		host_chat(seat, JSON_GetString(msg, "text", ""));
	else if (!strcmp(kind, "leave")) {
		remove_seat(seat);
		return;
	}
	send_snapshot(seat);
}

/* --- starting the game ------------------------------------------------------------------ */

static void begin_start(void)
{
	const lobbymode_t *mode = find_mode(L.ruleset);
	char gametype[32];
	int n = 0;
	if (L.phase != LOBBY_WAITING && L.phase != LOBBY_COUNTDOWN)
		return;
	L.phase = LOBBY_STARTING;
	Str_Copy(L.status, "Starting the game...", sizeof(L.status));
	L.startnonce = random32() | 1;
	L.startdeadline = Sys_Seconds() + 30;
	L.startsent = false;
	changed();
	/* Nuclide loads progs/<g_gametype>.dat, named in lower case */
	for (const char *p = L.ruleset; *p && n < (int)sizeof(gametype) - 1; p++)
		gametype[n++] = (char)tolower((unsigned char)*p);
	gametype[n] = 0;
	{
		char path[64];
		snprintf(path, sizeof(path), "progs/%s.dat", gametype);
		if (!FS_FileExists(path))
			Con_Printf("^3lobby: %s isn't in this game; the server will use its default rules\n", path);
	}
	{
		strbuf_t sb = {0};
		/* Nuclide's bots: bot_minClients keeps that many players (bots
		 * included) on the server, on every map; -1 leaves bots alone */
		int bots = L.bots < 0 ? L.max : BW_MIN(L.max, occupied() + L.bots);
		const lobbyruleset_t *rs = find_ruleset(L.ruleset);
		SB_Printf(&sb, "set sv_background 0\nset coop 0\nset deathmatch 1\nset maxplayers %d\nset sv_playerslots %d\n"
		               "set g_gametype %s\n",
		          L.max, L.max, gametype);
		SB_Printf(&sb, "set hostname \"%s\"\nset sv_password \"%s\"\nset sv_public %d\n", server_name(), L.password,
		          L.listed ? 1 : 0);
		/* rules that seat bots themselves (Stiletto's Freeze Tag and
		 * Archstiletto: arena_bots) would fight a quota: they get theirs
		 * switched on or off instead */
		if (rs && rs->seatbots)
			SB_Printf(&sb, "set bot_minClients -1\nset arena_bots %d\n", L.bots != 0 ? 1 : 0);
		else
			SB_Printf(&sb, "set bot_minClients %d\n", L.bots == 0 ? 0 : bots);
		if (atoi(L.timelimit) > 0)
			SB_Printf(&sb, "set timelimit %s\n", L.timelimit);
		if (atoi(L.fraglimit) > 0)
			SB_Printf(&sb, "set fraglimit %s\n", L.fraglimit);
		if (mode && mode->startcmds[0])
			SB_Printf(&sb, "%s\n", mode->startcmds);
		if (L.dedicated) {
			/* the same settings for a server program of its own (cl_dedlaunch.c),
			 * started once the title's local map has stopped */
			snprintf(L.rconpass, sizeof(L.rconpass), "%08x%08x", random32(), random32());
			SB_Printf(&sb, "set rcon_password %s\nmap %s\n", L.rconpass, L.map);
			Str_Copy(L.dedcmds, sb.data, sizeof(L.dedcmds));
			L.dedpending = true;
			L.dedport = 0;
			Str_Copy(L.status, "Starting the dedicated server...", sizeof(L.status));
			if (sv.active || cl.state != ca_disconnected)
				Cbuf_InsertText("disconnect\nkillserver\n");
		} else {
			SB_Printf(&sb, "map %s\n", L.map);
			/* ahead of anything waiting in the buffer (a script's waits) */
			Cbuf_InsertText(sb.data);
		}
		SB_Free(&sb);
	}
}

/* dedicated: start the server program, then connect to it like a guest */
static bool host_dedicated_frame(double now)
{
	if (L.dedpending) {
		char err[160];
		if (sv.active || cl.state != ca_disconnected)
			return false; /* the title's map is still stopping */
		L.dedpending = false;
		L.dedport = DedLaunch_Start(L.dedcmds, err, sizeof(err));
		if (!L.dedport) {
			close_with(err);
			return false;
		}
		Cvar_Set("rcon_password", L.rconpass);
		Cvar_Set("rcon_address", va("127.0.0.1:%d", L.dedport));
		L.nextconnect = now + 0.5;
		return false;
	}
	if (!DedLaunch_Running()) {
		close_with("The dedicated server stopped; dedicated.log in your game folder says why");
		return false;
	}
	if (cl.state == ca_disconnected && now >= L.nextconnect) {
		/* the host knows the password; connect retries for ten seconds */
		Cvar_Set("password", L.password);
		Cbuf_AddText(va("connect 127.0.0.1:%d\n", L.dedport));
		L.nextconnect = now + 12;
	}
	return cl.state == ca_active;
}

/* the host's server is up: tell the guests where it is */
static void host_starting_frame(void)
{
	double now = Sys_Seconds();
	bool all = true;
	if (now > L.startdeadline) {
		close_with("The game didn't start");
		return;
	}
	if (L.dedicated ? !host_dedicated_frame(now) : (!sv.active || cl.state != ca_active || (occupied() > 1 && !NET_ServerPort())))
		return; /* still loading */
	if (L.phase == LOBBY_STARTING) {
		L.phase = LOBBY_INGAME;
		Str_Copy(L.status, "In game", sizeof(L.status));
		L.startdeadline = now + 10; /* for the guests' answers */
		changed();
	}
	for (int i = 1; i < L.max; i++) {
		json_t *j;
		if (!L.seats[i].used || L.seats[i].startacked)
			continue;
		all = false;
		j = JSON_New(JSON_OBJECT);
		JSON_SetString(j, "t", "start");
		JSON_SetNumber(j, "sid", L.sid);
		JSON_SetNumber(j, "nonce", L.startnonce);
		JSON_SetNumber(j, "port", L.dedicated ? L.dedport : NET_ServerPort());
		JSON_SetString(j, "map", L.map);
		JSON_SetString(j, "pass", L.password);
		send_to(&L.seats[i].addr, j);
		JSON_Free(j);
	}
	if (all) {
		/* everyone is on their way: the party is over */
		Con_DPrintf("lobby: every guest has the game's address\n");
		reset_session();
	}
}

/* --- receiving -------------------------------------------------------------------------- */

static void guest_snapshot(const json_t *j)
{
	const char *phase = JSON_GetString(j, "phase", "WAITING");
	double remaining = JSON_GetNumber(j, "remaining", -1);
	const json_t *players = JSON_Get(j, "players"), *settings = JSON_Get(j, "settings"), *chat = JSON_Get(j, "chat");
	lobby_phase_t p = L.phase;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(phase_names); i++)
		if (!strcmp(phase_names[i], phase))
			p = (lobby_phase_t)i;
	if (L.phase == LOBBY_INGAME)
		return; /* already on the way */
	if (p == LOBBY_INGAME)
		p = LOBBY_STARTING; /* until the start message comes */
	L.phase = p;
	Str_Copy(L.status, JSON_GetString(j, "status", ""), sizeof(L.status));
	Str_Copy(L.map, JSON_GetString(j, "map", ""), sizeof(L.map));
	Str_Copy(L.roomcode, JSON_GetString(j, "code", L.roomcode), sizeof(L.roomcode));
	L.max = BW_CLAMP((int)JSON_GetNumber(j, "max", L.max), 1, LOBBY_SEATS);
	L.localseat = (int)JSON_GetNumber(j, "seat", L.localseat);
	L.countdown_end = remaining >= 0 ? Sys_Seconds() + remaining : 0;
	if (settings) {
		Str_Copy(L.ruleset, JSON_GetString(settings, "ruleset", L.ruleset), sizeof(L.ruleset));
		Str_Copy(L.timelimit, JSON_GetString(settings, "timelimit", L.timelimit), sizeof(L.timelimit));
		Str_Copy(L.fraglimit, JSON_GetString(settings, "fraglimit", L.fraglimit), sizeof(L.fraglimit));
		Str_Copy(L.hostname, JSON_GetString(settings, "hostname", L.hostname), sizeof(L.hostname));
		L.bots = (int)JSON_GetNumber(settings, "bots", L.bots);
		L.listed = JSON_GetBool(settings, "public", L.listed);
		L.locked = JSON_GetBool(settings, "password", L.locked);
		L.dedicated = !strcmp(JSON_GetString(settings, "servertype", ""), "DEDICATED SERVER");
	}
	if (players) {
		memset(L.seats, 0, sizeof(L.seats));
		JSON_FOREACH(pl, players)
		{
			int s = (int)JSON_GetNumber(pl, "seat", -1);
			if (s < 0 || s >= LOBBY_SEATS)
				continue;
			L.seats[s].used = true;
			Str_Copy(L.seats[s].name, JSON_GetString(pl, "name", ""), sizeof(L.seats[s].name));
			L.seats[s].ready = JSON_GetBool(pl, "ready", false);
			L.seats[s].connected = JSON_GetBool(pl, "connected", true);
		}
	}
	JSON_FOREACH(c, chat)
	add_chat((int)JSON_GetNumber(c, "id", 0), (int)JSON_GetNumber(c, "seat", -1), JSON_GetString(c, "name", ""),
	         JSON_GetString(c, "text", ""));
	acked((uint32_t)JSON_GetNumber(j, "ack", 0));
	if ((int)JSON_GetNumber(j, "rev", 0) != L.revision)
		L.revision = (int)JSON_GetNumber(j, "rev", 0);
	L.published_rev = -1;
}

static void guest_start(const json_t *j)
{
	uint32_t nonce = (uint32_t)JSON_GetNumber(j, "nonce", 0);
	int port = (int)JSON_GetNumber(j, "port", PORT_SERVER);
	json_t *ack = JSON_New(JSON_OBJECT);
	JSON_SetString(ack, "t", "startack");
	JSON_SetNumber(ack, "sid", L.sid);
	JSON_SetNumber(ack, "tok", L.token);
	JSON_SetNumber(ack, "nonce", nonce);
	send_to(&L.hostaddr, ack);
	JSON_Free(ack);
	if (nonce == L.seenstart)
		return; /* a repeat: connect once */
	L.seenstart = nonce;
	/* a passworded server checks the "password" userinfo key */
	Cvar_Set("password", JSON_GetString(j, "pass", L.joinpass));
	if (is_live(&L.hostaddr)) {
		Con_Printf("lobby: the game is on live:%u\n", (unsigned)L.hostaddr.sin_addr.s_addr);
		Cbuf_InsertText(va("%sconnect live:%u\n", sv.active ? "killserver\n" : "", (unsigned)L.hostaddr.sin_addr.s_addr));
	} else {
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &L.hostaddr.sin_addr, ip, sizeof(ip));
		Con_Printf("lobby: the game is on %s:%d\n", ip, port);
		/* the title's background map gives way to the real game */
		Cbuf_InsertText(va("%sconnect %s:%d\n", sv.active ? "killserver\n" : "", ip, port));
	}
	L.phase = LOBBY_INGAME;
	Str_Copy(L.status, "Connecting to the game...", sizeof(L.status));
	changed();
	/* acknowledge a while longer in case the first answer was lost, then close */
	L.startdeadline = Sys_Seconds() + 3;
}

static void info_reply(const struct sockaddr_in *to)
{
	json_t *j = JSON_New(JSON_OBJECT);
	JSON_SetString(j, "t", "info");
	JSON_SetNumber(j, "proto", 1);
	JSON_SetString(j, "name", L.seats[0].name);
	JSON_SetString(j, "map", L.map);
	JSON_SetString(j, "code", L.roomcode);
	JSON_SetNumber(j, "players", occupied());
	JSON_SetNumber(j, "max", L.max);
	send_to(to, j);
	JSON_Free(j);
}

static void handle(const struct sockaddr_in *from, const json_t *j)
{
	const char *t = JSON_GetString(j, "t", "");
	uint32_t sid = (uint32_t)JSON_GetNumber(j, "sid", 0);
	if (!strcmp(t, "discover")) {
		if (L.host && L.phase == LOBBY_WAITING)
			info_reply(from);
		return;
	}
	if (L.host) {
		int seat;
		if (!strcmp(t, "join")) {
			host_join(from, j);
			return;
		}
		if (sid != L.sid || (seat = seat_for(from, j)) < 0)
			return; /* not one of ours */
		L.seats[seat].lastheard = Sys_Seconds();
		L.seats[seat].connected = true;
		if (!strcmp(t, "intent"))
			host_intent(seat, j);
		else if (!strcmp(t, "startack") && (uint32_t)JSON_GetNumber(j, "nonce", 0) == L.startnonce)
			L.seats[seat].startacked = true;
		return;
	}
	/* guests listen to their host only */
	if (!same_addr(from, &L.hostaddr))
		return;
	if (!strcmp(t, "accept") && L.phase == LOBBY_JOINING) {
		L.sid = sid;
		L.token = (uint32_t)JSON_GetNumber(j, "tok", 0);
		L.localseat = (int)JSON_GetNumber(j, "seat", -1);
		L.phase = LOBBY_WAITING;
		L.lastfromhost = Sys_Seconds();
		Con_Printf("lobby: joined, seat %d\n", L.localseat);
		changed();
		return;
	}
	if (!strcmp(t, "reject") && L.phase == LOBBY_JOINING) {
		/* the page asks for the password and joins again (joinpass:) */
		L.needpass = JSON_GetBool(j, "needpass", false);
		close_with(JSON_GetString(j, "reason", "The host turned us away"));
		return;
	}
	if (sid != L.sid)
		return;
	L.lastfromhost = Sys_Seconds();
	if (!strcmp(t, "snap"))
		guest_snapshot(j);
	else if (!strcmp(t, "close"))
		close_with(JSON_GetString(j, "reason", "The host closed the lobby"));
	else if (!strcmp(t, "start"))
		guest_start(j);
}

static void datagram(const struct sockaddr_in *fromp, char *buf)
{
	struct sockaddr_in from = *fromp;
	json_t *j;
	{
		if (strncmp(buf, LOBBY_MAGIC, strlen(LOBBY_MAGIC)))
			return;
		if ((j = JSON_Parse(buf + strlen(LOBBY_MAGIC), NULL, 0))) {
			if (L.phase == LOBBY_IDLE || L.phase == LOBBY_CLOSING) {
				/* discovery replies arrive on an idle socket */
				if (!strcmp(JSON_GetString(j, "t", ""), "info") && numfound < MAX_FOUND) {
					char ip[INET_ADDRSTRLEN];
					bool dup = false;
					inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
					for (int i = 0; i < numfound; i++)
						dup |= !strcmp(found[i].addr, va("%s:%d", ip, ntohs(from.sin_port)));
					if (!dup) {
						snprintf(found[numfound].addr, sizeof(found[0].addr), "%s:%d", ip, ntohs(from.sin_port));
						Str_Copy(found[numfound].name, JSON_GetString(j, "name", ""), sizeof(found[0].name));
						Str_Copy(found[numfound].map, JSON_GetString(j, "map", ""), sizeof(found[0].map));
						Str_Copy(found[numfound].code, JSON_GetString(j, "code", ""), sizeof(found[0].code));
						found[numfound].players = (int)JSON_GetNumber(j, "players", 0);
						found[numfound].max = (int)JSON_GetNumber(j, "max", 0);
						numfound++;
					}
				}
			} else {
				handle(&from, j);
			}
			JSON_Free(j);
		}
	}
}

/* pieces of one lobby message from a Live peer, put back together */
#define LIVE_ASSEMBLY 8
static struct {
	uint32_t from;
	uint16_t msgid;
	int count, have;
	uint32_t got[8]; /* bit per piece */
	char data[255 * 64];
	size_t len[255];
	double started;
} assembly[LIVE_ASSEMBLY];

static void live_piece(uint32_t from, const uint8_t *p, size_t n)
{
	if (n < 4)
		return;
	uint16_t msgid = (uint16_t)(p[0] << 8 | p[1]);
	int index = p[2], count = p[3];
	size_t len = n - 4;
	if (!count || index >= count || len > LIVE_PIECE || (size_t)count * LIVE_PIECE > sizeof(assembly[0].data))
		return;
	int slot = -1, oldest = 0;
	for (int i = 0; i < LIVE_ASSEMBLY; i++) {
		if (assembly[i].count && assembly[i].from == from && assembly[i].msgid == msgid)
			slot = i;
		if (assembly[i].started < assembly[oldest].started)
			oldest = i;
	}
	if (slot < 0) {
		slot = oldest;
		memset(&assembly[slot].got, 0, sizeof(assembly[slot].got));
		assembly[slot].from = from;
		assembly[slot].msgid = msgid;
		assembly[slot].count = count;
		assembly[slot].have = 0;
		assembly[slot].started = Sys_Seconds();
	}
	if (assembly[slot].count != count || (assembly[slot].got[index / 32] & (1u << (index % 32))))
		return;
	assembly[slot].got[index / 32] |= 1u << (index % 32);
	memcpy(assembly[slot].data + (size_t)index * LIVE_PIECE, p + 4, len);
	assembly[slot].len[index] = len;
	if (++assembly[slot].have < count)
		return;
	/* whole: pieces before the last are full, so the message is contiguous */
	size_t total = (size_t)(count - 1) * LIVE_PIECE + assembly[slot].len[count - 1];
	char *msg = Mem_Alloc(total + 1);
	memcpy(msg, assembly[slot].data, total);
	msg[total] = 0;
	assembly[slot].count = 0;
	assembly[slot].started = 0;
	struct sockaddr_in a;
	live_addr(from, &a);
	datagram(&a, msg);
	Mem_Free(msg);
}

static void receive(void)
{
	char buf[16384];
	uint8_t piece[1500];
	uint32_t from_id;
	size_t pn;
	while ((pn = Live_GetLobby(piece, sizeof(piece), &from_id)))
		live_piece(from_id, piece, pn);
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n;
		if (L.sock < 0)
			return;
		n = recvfrom(L.sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			return;
		buf[n] = 0;
		datagram(&from, buf);
	}
}

/* --- discovery ------------------------------------------------------------------------- */

/* asks this network (and this machine) for lobbies, waiting up to wait seconds */
static void discover(double wait)
{
	struct sockaddr_in to;
	json_t *j = JSON_New(JSON_OBJECT);
	bool own = L.sock < 0;
	lobby_phase_t saved = L.phase;
	if (own && !sock_open(0)) {
		JSON_Free(j);
		return;
	}
	numfound = 0;
	JSON_SetString(j, "t", "discover");
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)lobby_port->ivalue);
#ifndef __EMSCRIPTEN__ /* a page can't broadcast, and its sockets reach only WebSocket servers */
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	send_to(&to, j);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	send_to(&to, j);
#endif
	JSON_Free(j);
	/* replies land in found[] while the session looks idle */
	L.phase = LOBBY_IDLE;
	for (double end = Sys_Seconds() + wait; Sys_Seconds() < end;) {
		receive();
		Sys_Sleep(0.01);
	}
	L.phase = saved;
	if (own)
		sock_close();
}

/* --- console commands ----------------------------------------------------------------- */

static void LobbyReady_f(int argc, const char **argv)
{
	bool ready = argc < 2 || atoi(argv[1]) != 0;
	if (!can_ready())
		return;
	if (L.host)
		host_ready(0, ready);
	else
		queue_intent(ready ? "ready" : "unready", NULL, NULL);
}

static void LobbyCancel_f(int argc, const char **argv)
{
	if (can_cancel())
		cancel_countdown("Countdown cancelled");
}

static void LobbyStart_f(int argc, const char **argv)
{
	if (can_start())
		begin_start();
}

static void LobbyClose_f(int argc, const char **argv)
{
	L.needpass = false; /* the page's password question is answered with Leave */
	if (L.phase == LOBBY_IDLE)
		return;
	if (L.host) {
		for (int i = 1; i < L.max; i++)
			if (L.seats[i].used)
				send_simple(&L.seats[i].addr, "close", "reason", "The host closed the lobby");
	} else if (L.sock >= 0 && L.token) {
		/* one try is enough: the host times silent guests out anyway */
		queue_intent("leave", NULL, NULL);
		send_intents();
	}
	reset_session();
}

static void set_map(const char *id)
{
	L.mapchosen = true;
	if (!strcmp(L.map, id))
		return;
	cancel_countdown("The map changed");
	Str_Copy(L.map, id, sizeof(L.map));
	changed();
}

static bool set_ruleset(const char *id)
{
	char up[32];
	int n = 0;
	if (!valid_ruleset(id) || !ruleset_runs(id))
		return false;
	for (; id[n] && n < (int)sizeof(up) - 1; n++)
		up[n] = (char)toupper((unsigned char)id[n]);
	up[n] = 0;
	if (strcmp(L.ruleset, up)) {
		cancel_countdown("The rules changed");
		Str_Copy(L.ruleset, up, sizeof(L.ruleset));
		/* until the host picks a map, each ruleset brings its own (Duel: duel_alley) */
		if (!L.mapchosen && !*lobby_defaultmap->string)
			Str_Copy(L.map, ruleset_map(up), sizeof(L.map));
		changed();
	}
	return true;
}

static bool set_limits(int minutes, int frags)
{
	char t[16], f[16];
	if (minutes < 0 || minutes > 180 || frags < 0 || frags > 999)
		return false;
	snprintf(t, sizeof(t), "%d", minutes);
	snprintf(f, sizeof(f), "%d", frags);
	if (strcmp(t, L.timelimit) || strcmp(f, L.fraglimit)) {
		cancel_countdown("The limits changed");
		Str_Copy(L.timelimit, t, sizeof(L.timelimit));
		Str_Copy(L.fraglimit, f, sizeof(L.fraglimit));
		changed();
	}
	return true;
}

/* the server the lobby starts; NULL leaves a setting as it is. Refuses a
 * player count below the seats in use, more bots than free slots, a name or
 * password that can't go into a config, and Dedicated where programs can't
 * be started. */
static const char *set_server(const char *type, const char *max, const char *bots, const char *listed,
                              const char *name, const char *pass)
{
	bool dedicated = L.dedicated, pub = L.listed;
	int m = L.max, b = L.bots;
	if (type) {
		if (!Str_ICmp(type, "dedicated"))
			dedicated = true;
		else if (!Str_ICmp(type, "listen"))
			dedicated = false;
		else
			return "the server type is listen or dedicated";
		if (dedicated && !DedLaunch_Supported())
			return "this platform can't start a dedicated server";
	}
	if (max) {
		char *end;
		long v = strtol(max, &end, 10);
		if (*end || v < 1 || v > LOBBY_SEATS)
			return va("max players is 1 to %d", LOBBY_SEATS);
		for (int i = (int)v; i < LOBBY_SEATS; i++)
			if (L.seats[i].used)
				return "there are more players in the lobby than that";
		m = (int)v;
	}
	if (bots) {
		char *end;
		long v = strtol(bots, &end, 10);
		if (!Str_ICmp(bots, "fill") || !strcmp(bots, "-1"))
			b = -1;
		else if (*end || v < 0 || v >= LOBBY_SEATS)
			return "bots is a number or fill";
		else
			b = (int)v;
	}
	if (b > 0 && b > m - occupied())
		return "that's more bots than free player slots";
	if (listed)
		pub = atoi(listed) != 0;
	if ((name && !text_ok(name)) || (pass && !text_ok(pass)))
		return "names and passwords can't have quotes, backslashes or semicolons (63 bytes at most)";
	if (dedicated != L.dedicated || m != L.max || b != L.bots || pub != L.listed ||
	    (name && strcmp(name, L.hostname)) || (pass && strcmp(pass, L.password))) {
		cancel_countdown("The server settings changed");
		L.dedicated = dedicated;
		L.max = m;
		L.bots = b;
		L.listed = pub;
		if (name)
			Str_Copy(L.hostname, name, sizeof(L.hostname));
		if (pass)
			Str_Copy(L.password, pass, sizeof(L.password));
		changed();
	}
	return NULL;
}

static void LobbySet_f(int argc, const char **argv)
{
	static const char *const keys[] = {"servertype", "maxplayers", "bots", "public", "hostname", "password"};
	if (argc < 2 || !can_change()) {
		Con_Printf("usage: lobby_set <ruleset|map|servertype|maxplayers|bots|public|hostname|password> <value> (host only)\n");
		return;
	}
	for (int k = 0; k < (int)BW_ARRAY_COUNT(keys); k++) {
		const char *v[6] = {0}, *why;
		if (Str_ICmp(argv[1], keys[k]))
			continue;
		/* an empty or missing value clears the name or password */
		v[k] = argc > 2 ? argv[2] : "";
		if ((why = set_server(v[0], v[1], v[2], v[3], v[4], v[5])))
			Con_Printf("lobby_set: %s\n", why);
		return;
	}
	if (argc < 3) {
		Con_Printf("usage: lobby_set <ruleset|map> <value> (host only)\n");
		return;
	}
	if (!Str_ICmp(argv[1], "ruleset") && !set_ruleset(argv[2])) {
		if (valid_ruleset(argv[2]))
			Con_Printf("lobby_set: ruleset %s isn't in this game (its progs are missing)\n", argv[2]);
		else
			Con_Printf("lobby_set: unknown ruleset %s\n", argv[2]);
	}
	else if (!Str_ICmp(argv[1], "map"))
		set_map(argv[2]);
}

static void LobbyLimits_f(int argc, const char **argv)
{
	if (argc < 3 || !can_change() || !set_limits(atoi(argv[1]), atoi(argv[2])))
		Con_Printf("usage: lobby_limits <minutes 0-180> <frags 0-999> (host only)\n");
}

/* UTF-8 text from hex, or false when it isn't valid */
static bool decode_hex(const char *hex, char *out, size_t size)
{
	size_t n = strlen(hex), o = 0;
	if (n == 0 || n % 2 || n / 2 > 240 || n / 2 >= size)
		return false;
	for (size_t i = 0; i < n; i += 2) {
		int hi = isxdigit((unsigned char)hex[i]) ? (isdigit((unsigned char)hex[i]) ? hex[i] - '0' : (tolower(hex[i]) - 'a' + 10)) : -1;
		int lo = isxdigit((unsigned char)hex[i + 1]) ? (isdigit((unsigned char)hex[i + 1]) ? hex[i + 1] - '0' : (tolower(hex[i + 1]) - 'a' + 10)) : -1;
		if (hi < 0 || lo < 0)
			return false;
		out[o] = (char)(hi * 16 + lo);
		if ((unsigned char)out[o] < 0x20)
			return false;
		o++;
	}
	out[o] = 0;
	/* well-formed UTF-8 */
	for (size_t i = 0; i < o;) {
		unsigned char c = (unsigned char)out[i];
		int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (extra < 0 || i + (size_t)extra >= o + (extra ? 0 : 1))
			return false;
		for (int k = 1; k <= extra; k++)
			if (((unsigned char)out[i + k] & 0xc0) != 0x80)
				return false;
		i += (size_t)extra + 1;
	}
	return true;
}

static bool chat(const char *hex)
{
	char text[256];
	if (!can_chat() || !decode_hex(hex, text, sizeof(text)))
		return false;
	if (L.host)
		host_chat(0, text);
	else
		queue_intent("chat", "text", text);
	return true;
}

static void LobbyChatHex_f(int argc, const char **argv)
{
	if (argc < 2 || !chat(argv[1]))
		Con_Printf("usage: lobby_chat_hex <utf-8 bytes as hex> (in a lobby)\n");
}

static void LobbySnapshot_f(int argc, const char **argv)
{
	strbuf_t sb = {0};
	json_t *j = local_snapshot();
	JSON_Write(&sb, j, -1);
	JSON_Free(j);
	Con_Printf("%s\n", sb.data);
	{
		FILE *f = fopen("lobby_snapshot.json", "w");
		if (f) {
			fwrite(sb.data, 1, sb.len, f);
			fclose(f);
		}
	}
	SB_Free(&sb);
}

static void LobbyChatSnapshot_f(int argc, const char **argv)
{
	strbuf_t sb = {0};
	json_t *j = chat_json();
	JSON_Write(&sb, j, -1);
	JSON_Free(j);
	Con_Printf("%s\n", sb.data);
	SB_Free(&sb);
}

/* --- the bridge --------------------------------------------------------------------------- */

static void reply(strbuf_t *out, bool ok, const char *error)
{
	if (ok)
		SB_AppendStr(out, "{\"ok\":true}");
	else
		SB_Printf(out, "{\"ok\":false,\"error\":\"%s\"}", error ? error : "not allowed now");
}

static bool map_id_ok(const char *id)
{
	size_t n = strlen(id);
	if (n < 1 || n > 63)
		return false;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_')
			return false;
	return map_present(id);
}

static void lobby_action(const char *a, strbuf_t *out)
{
	if (!strcmp(a, "ready") || !strcmp(a, "unready")) {
		if (!can_ready())
			return reply(out, false, NULL);
		LobbyReady_f(2, (const char *[]){"lobby_ready", !strcmp(a, "ready") ? "1" : "0"});
		return reply(out, true, NULL);
	}
	if (!strcmp(a, "start")) {
		if (!can_start())
			return reply(out, false, NULL);
		begin_start();
		return reply(out, true, NULL);
	}
	if (!strcmp(a, "cancel")) {
		if (!can_cancel())
			return reply(out, false, NULL);
		cancel_countdown("Countdown cancelled");
		return reply(out, true, NULL);
	}
	if (!strcmp(a, "close")) {
		LobbyClose_f(1, (const char *[]){"lobby_close"});
		return reply(out, true, NULL);
	}
	if (!strncmp(a, "setmap:", 7)) {
		if (!can_change() || !map_id_ok(a + 7))
			return reply(out, false, "unknown map");
		set_map(a + 7);
		return reply(out, true, NULL);
	}
	if (!strncmp(a, "setruleset:", 11)) {
		if (!can_change() || !set_ruleset(a + 11))
			return reply(out, false, "unknown ruleset");
		return reply(out, true, NULL);
	}
	if (!strncmp(a, "setlimits:", 10)) {
		int m, f;
		char extra;
		if (!can_change() || sscanf(a + 10, "%d:%d%c", &m, &f, &extra) != 2 || !set_limits(m, f))
			return reply(out, false, "bad limits");
		return reply(out, true, NULL);
	}
	if (!strncmp(a, "chat:", 5))
		return reply(out, chat(a + 5), "can't send that");
	if (!strncmp(a, "setserver:", 10)) {
		/* setserver:<listen|dedicated>:<max>:<bots|fill>:<public 0|1>:<name hex>:<password hex> */
		char buf[600], *f[6], name[256] = "", pass[256] = "";
		const char *why;
		int nf = 0;
		Str_Copy(buf, a + 10, sizeof(buf));
		f[nf++] = buf;
		for (char *p = buf; *p && nf < 6; p++)
			if (*p == ':') {
				*p = 0;
				f[nf++] = p + 1;
			}
		if (!can_change() || nf != 6 || strchr(f[5], ':'))
			return reply(out, false, "bad server settings");
		if ((f[4][0] && !decode_hex(f[4], name, sizeof(name))) || (f[5][0] && !decode_hex(f[5], pass, sizeof(pass))))
			return reply(out, false, "bad text");
		if ((why = set_server(f[0], f[1], f[2], f[3], name, pass))) {
			strbuf_t e = {0};
			SB_AppendStr(out, "{\"ok\":false,\"error\":");
			json_t *msg = JSON_NewString(why);
			JSON_Write(&e, msg, -1);
			JSON_Free(msg);
			SB_AppendStr(out, e.data);
			SB_AppendStr(out, "}");
			SB_Free(&e);
			return;
		}
		return reply(out, true, NULL);
	}
	if (!strncmp(a, "joinpass:", 9)) {
		/* the guest answers the host's password question: join again with it */
		char pass[256], target[80];
		if (L.phase != LOBBY_IDLE && L.phase != LOBBY_CLOSING)
			return reply(out, false, NULL);
		if (!L.needpass || !L.jointarget[0] || !decode_hex(a + 9, pass, sizeof(pass)) || !text_ok(pass))
			return reply(out, false, "bad password");
		Str_Copy(target, L.jointarget, sizeof(target));
		reset_session();
		LobbyJoin_f(3, (const char *[]){"lobby_join", target, pass});
		return reply(out, true, NULL);
	}
	reply(out, false, "unknown action");
}

bool Lobby_Query(const char *req, strbuf_t *out)
{
	if (!strcmp(req, "getlobby")) {
		json_t *j = local_snapshot();
		JSON_Write(out, j, -1);
		JSON_Free(j);
	} else if (!strcmp(req, "getlobbychat")) {
		json_t *j = chat_json();
		JSON_Write(out, j, -1);
		JSON_Free(j);
	} else if (!strncmp(req, "lobby_action:", 13)) {
		lobby_action(req + 13, out);
	} else if (!strcmp(req, "hostcache_refresh")) {
		discover(0.25);
		SB_AppendStr(out, "ok");
	} else if (!strcmp(req, "gethostcache")) {
		SB_AppendStr(out, "[");
		for (int i = 0; i < numfound; i++)
			SB_Printf(out, "%s{\"addr\":\"%s\",\"name\":\"%s\",\"map\":\"%s\",\"players\":%d,\"max\":%d,\"lobby\":true}",
			          i ? "," : "", found[i].addr, found[i].name, found[i].map, found[i].players, found[i].max);
		SB_AppendStr(out, "]");
	} else {
		return false;
	}
	return true;
}

/* --- each frame ------------------------------------------------------------------------- */

void Lobby_Frame(void)
{
	double now = Sys_Seconds();
	receive();
	switch (L.phase) {
	case LOBBY_IDLE:
		break;
	case LOBBY_CLOSING:
		if (now >= L.closing_until) {
			char keep[160];
			Str_Copy(keep, L.error, sizeof(keep));
			reset_session();
			Str_Copy(L.error, keep, sizeof(L.error));
		}
		break;
	case LOBBY_JOINING:
		if (L.livejoin) {
			/* wait for the Live party, then talk to its leader's lobby */
			if (!Str_ICmp(Live_PartyCode(), L.roomcode) && Live_PartyLeader()) {
				live_addr(Live_PartyLeader(), &L.hostaddr);
				L.livejoin = false;
				L.lastfromhost = now;
				snprintf(L.status, sizeof(L.status), "Joining %s...", L.roomcode);
				changed();
			} else if (now >= L.livejoin_until) {
				close_with(va("No room %s on Bulwark Live", L.roomcode));
			}
			break;
		}
		if (now >= L.nextjoin) {
			json_t *j;
			char name[64];
			if (++L.joinattempts > 20) {
				close_with("No answer from the lobby host");
				break;
			}
			L.nextjoin = now + 0.5;
			local_name(name, sizeof(name));
			j = JSON_New(JSON_OBJECT);
			JSON_SetString(j, "t", "join");
			JSON_SetNumber(j, "proto", 1);
			JSON_SetString(j, "name", name);
			if (L.joinpass[0])
				JSON_SetString(j, "pass", L.joinpass);
			send_to(&L.hostaddr, j);
			JSON_Free(j);
		}
		break;
	default:
		if (L.host) {
			/* an online lobby takes its Live party's room code */
			if (!strcmp(L.network, "online") && Live_PartyCode()[0] && strcmp(L.roomcode, Live_PartyCode())) {
				Str_Copy(L.roomcode, Live_PartyCode(), sizeof(L.roomcode));
				Con_Printf("lobby: room code %s (Bulwark Live)\n", L.roomcode);
				changed();
			}
			for (int i = 1; i < L.max; i++)
				if (L.seats[i].used && now - L.seats[i].lastheard > LOBBY_TIMEOUT)
					remove_seat(i);
			if (L.phase == LOBBY_WAITING && all_ready() && now >= L.settle_until) {
				L.phase = LOBBY_COUNTDOWN;
				L.countdown_end = now + BW_MAX(0, lobby_readytime->value);
				Str_Copy(L.status, "Starting soon", sizeof(L.status));
				changed();
			}
			if (L.phase == LOBBY_COUNTDOWN && now >= L.countdown_end)
				begin_start();
			if (L.phase == LOBBY_STARTING || L.phase == LOBBY_INGAME)
				host_starting_frame();
			if (L.phase != LOBBY_IDLE && (L.revision != L.published_rev || now >= L.nextsnap))
				broadcast_snapshot();
		} else {
			if (L.phase == LOBBY_INGAME) {
				if (now >= L.startdeadline)
					reset_session();
				break;
			}
			if (now - L.lastfromhost > LOBBY_TIMEOUT) {
				close_with("The lobby host stopped answering");
				break;
			}
			send_intents();
			if (now >= L.nextping) {
				json_t *j = JSON_New(JSON_OBJECT);
				L.nextping = now + 1;
				JSON_SetString(j, "t", "intent");
				JSON_SetNumber(j, "sid", L.sid);
				JSON_SetNumber(j, "tok", L.token);
				JSON_SetString(j, "kind", "ping");
				JSON_SetNumber(j, "seq", 0);
				send_to(&L.hostaddr, j);
				JSON_Free(j);
			}
		}
		break;
	}
	publish();
}

void Lobby_Init(void)
{
	lobby_port = Cvar_Get("lobby_port", "27501", 0, "UDP port a lobby host listens on");
	lobby_readytime = Cvar_Get("lobby_readytime", "5", CVAR_ARCHIVE, "Seconds of countdown once every lobby player is ready");
	lobby_maxplayers = Cvar_Get("lobby_maxplayers", "16", 0, "Seats in a lobby");
	lobby_defaultmap = Cvar_Get("lobby_defaultmap", "", 0, "The map a new lobby picks; empty picks the ruleset's own (scripts/lobby.txt)");
	lobby_defaultruleset = Cvar_Get("lobby_defaultruleset", "DUEL", 0, "The rules a new lobby picks");
	lobby_snapshot = Cvar_Get("lobby_snapshot", "", CVAR_READONLY | CVAR_NOSAVE, "The lobby's state as JSON");
	lobby_hostname = Cvar_Get("lobby_hostname", "", CVAR_ARCHIVE, "The server name a new lobby starts with; empty: <name>'s server");
	lobby_password = Cvar_Get("lobby_password", "", 0, "The password a new lobby starts with (joining it, and its server)");
	lobby_bots = Cvar_Get("lobby_bots", "0", CVAR_ARCHIVE, "Bots a new lobby's server adds besides the players, or fill");
	lobby_public = Cvar_Get("lobby_public", "0", CVAR_ARCHIVE, "1: a new lobby's server is listed on the master servers (sv_public)");
	lobby_servertype = Cvar_Get("lobby_servertype", "listen", CVAR_ARCHIVE, "listen or dedicated: the server a new lobby starts");
	DedLaunch_Init();
	Cmd_AddD("lobby_create", LobbyCreate_f, "lobby_create <lan|online|offline>: host a lobby");
	Cmd_AddD("lobby_create_lan", LobbyCreate_f, "Host a lobby on this network");
	Cmd_AddD("lobby_create_online", LobbyCreate_f, "Host a lobby with a room code");
	Cmd_AddD("lobby_create_offline", LobbyCreate_f, "Host a lobby nobody else can join (tests)");
	Cmd_AddD("lobby_join", LobbyJoin_f, "lobby_join <address|room code> [password]: join a lobby");
	Cmd_AddD("lobby_ready", LobbyReady_f, "lobby_ready [1|0]: say you are ready, or not");
	Cmd_AddD("lobby_cancel", LobbyCancel_f, "Stop the lobby's countdown (host)");
	Cmd_AddD("lobby_start", LobbyStart_f, "Start the lobby's game now (host)");
	Cmd_AddD("lobby_close", LobbyClose_f, "Leave the lobby, or close it when hosting");
	Cmd_AddD("lobby_set", LobbySet_f,
	         "lobby_set <ruleset|map|servertype|maxplayers|bots|public|hostname|password> <value>: change the lobby's game (host)");
	Cmd_AddD("lobby_limits", LobbyLimits_f, "lobby_limits <minutes> <frags>: the game's limits (host)");
	Cmd_AddD("lobby_chat_hex", LobbyChatHex_f, "lobby_chat_hex <hex>: say something in the lobby (UTF-8 as hex)");
	Cmd_AddD("lobby_snapshot", LobbySnapshot_f, "Print the lobby's state and write lobby_snapshot.json");
	Cmd_AddD("lobby_chat_snapshot", LobbyChatSnapshot_f, "Print the lobby's chat");
	L.max = BW_CLAMP(lobby_maxplayers->ivalue, 1, LOBBY_SEATS);
	publish();
}

void Lobby_Shutdown(void)
{
	LobbyClose_f(1, (const char *[]){"lobby_close"});
}
