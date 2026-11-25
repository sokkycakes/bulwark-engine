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
	char notice[200]; /* shown under the lobby, not an error */
	char roomcode[8];
	char map[64];
	char ruleset[32];
	char timelimit[16], fraglimit[16];
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

static const char *const phase_names[] = {"IDLE", "CREATING", "JOINING", "WAITING",
                                          "COUNTDOWN", "STARTING", "INGAME", "CLOSING"};

lobby_phase_t Lobby_Phase(void) { return L.phase; }
bool Lobby_Active(void) { return L.phase != LOBBY_IDLE; }

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

static json_t *settings_json(void)
{
	json_t *s = JSON_New(JSON_OBJECT);
	JSON_SetString(s, "ruleset", L.ruleset);
	JSON_SetString(s, "servertype", "LISTEN SERVER");
	JSON_SetString(s, "timelimit", L.timelimit);
	JSON_SetString(s, "fraglimit", L.fraglimit);
	return s;
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
	JSON_SetString(j, "notice", L.phase != LOBBY_IDLE ? L.notice : "");
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
	JSON_Set(j, "settings", active ? settings_json() : JSON_New(JSON_OBJECT));
	JSON_Set(j, "players", active ? players_json() : JSON_New(JSON_ARRAY));
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
	JSON_Set(j, "settings", settings_json());
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

static const char *const builtin_rulesets[] = {"DUEL", "DEATHMATCH", "TEAMDM", "DOMINATION", NULL};

static const lobbymode_t *find_mode(const char *id)
{
	for (int i = 0; i < nummodes; i++)
		if (!Str_ICmp(modes[i].id, id))
			return &modes[i];
	return NULL;
}

static bool valid_ruleset(const char *id)
{
	for (int i = 0; builtin_rulesets[i]; i++)
		if (!Str_ICmp(builtin_rulesets[i], id))
			return true;
	return find_mode(id) != NULL;
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
	L.notice[0] = 0;
	Str_Copy(L.map, lobby_defaultmap->string, sizeof(L.map));
	Str_Copy(L.ruleset, *lobby_defaultruleset->string ? lobby_defaultruleset->string : "DUEL", sizeof(L.ruleset));
	Str_Copy(L.timelimit, "0", sizeof(L.timelimit));
	Str_Copy(L.fraglimit, "0", sizeof(L.fraglimit));
	for (char *p = L.ruleset; *p; p++)
		*p = (char)toupper((unsigned char)*p);
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
	}
#ifdef __EMSCRIPTEN__
	/* a browser tab can't take connections: the lobby and its game are
	 * this player's alone, and a room code would lead nobody here */
	Str_Copy(L.notice, "This lobby is hosted in your browser, so others can't join it. "
		"To play together, join a lobby hosted in the desktop game.", sizeof(L.notice));
#else
	else if (!strcmp(L.network, "online")) {
		static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		for (int i = 0; i < 6; i++)
			L.roomcode[i] = letters[rand() % (int)(sizeof(letters) - 1)];
		L.roomcode[6] = 0;
	}
#endif
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
		Con_Printf("usage: lobby_join <address|room code>\n");
		return;
	}
	new_session();
	Str_Copy(L.jointarget, argv[1], sizeof(L.jointarget));
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
#ifdef __EMSCRIPTEN__
			close_with("Room codes only work in the desktop game. Ask the host for their address and join with that.");
#else
			close_with(va("No lobby with room code %s was found on this network", argv[1]));
#endif
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
		SB_Printf(&sb, "set sv_background 0\nset coop 0\nset deathmatch 1\nset maxplayers %d\nset sv_playerslots %d\n"
		               "set g_gametype %s\n",
		          L.max, L.max, gametype);
		if (atoi(L.timelimit) > 0)
			SB_Printf(&sb, "set timelimit %s\n", L.timelimit);
		if (atoi(L.fraglimit) > 0)
			SB_Printf(&sb, "set fraglimit %s\n", L.fraglimit);
		if (mode && mode->startcmds[0])
			SB_Printf(&sb, "%s\n", mode->startcmds);
		SB_Printf(&sb, "map %s\n", L.map);
		/* ahead of anything waiting in the buffer (a script's waits) */
		Cbuf_InsertText(sb.data);
		SB_Free(&sb);
	}
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
	if (!sv.active || cl.state != ca_active || (occupied() > 1 && !NET_ServerPort()))
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
		JSON_SetNumber(j, "port", NET_ServerPort());
		JSON_SetString(j, "map", L.map);
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
	if (!valid_ruleset(id))
		return false;
	for (; id[n] && n < (int)sizeof(up) - 1; n++)
		up[n] = (char)toupper((unsigned char)id[n]);
	up[n] = 0;
	if (strcmp(L.ruleset, up)) {
		cancel_countdown("The rules changed");
		Str_Copy(L.ruleset, up, sizeof(L.ruleset));
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

static void LobbySet_f(int argc, const char **argv)
{
	if (argc < 3 || !can_change()) {
		Con_Printf("usage: lobby_set <ruleset|map> <value> (host only)\n");
		return;
	}
	if (!Str_ICmp(argv[1], "ruleset") && !set_ruleset(argv[2]))
		Con_Printf("lobby_set: unknown ruleset %s\n", argv[2]);
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
	return FS_FileExists(va("maps/%s.bsp", id)) || FS_FileExists(va("maps/%s.scene", id));
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
	lobby_defaultmap = Cvar_Get("lobby_defaultmap", "envtest", 0, "The map a new lobby picks");
	lobby_defaultruleset = Cvar_Get("lobby_defaultruleset", "DUEL", 0, "The rules a new lobby picks");
	lobby_snapshot = Cvar_Get("lobby_snapshot", "", CVAR_READONLY | CVAR_NOSAVE, "The lobby's state as JSON");
	Cmd_AddD("lobby_create", LobbyCreate_f, "lobby_create <lan|online|offline>: host a lobby");
	Cmd_AddD("lobby_create_lan", LobbyCreate_f, "Host a lobby on this network");
	Cmd_AddD("lobby_create_online", LobbyCreate_f, "Host a lobby with a room code");
	Cmd_AddD("lobby_create_offline", LobbyCreate_f, "Host a lobby nobody else can join (tests)");
	Cmd_AddD("lobby_join", LobbyJoin_f, "lobby_join <address|room code>: join a lobby");
	Cmd_AddD("lobby_ready", LobbyReady_f, "lobby_ready [1|0]: say you are ready, or not");
	Cmd_AddD("lobby_cancel", LobbyCancel_f, "Stop the lobby's countdown (host)");
	Cmd_AddD("lobby_start", LobbyStart_f, "Start the lobby's game now (host)");
	Cmd_AddD("lobby_close", LobbyClose_f, "Leave the lobby, or close it when hosting");
	Cmd_AddD("lobby_set", LobbySet_f, "lobby_set <ruleset|map> <value>: change the lobby's game (host)");
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
