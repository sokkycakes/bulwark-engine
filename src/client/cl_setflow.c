/* Bulwark engine - the match flow around Bulwark Live sets: the in-game set
 * view (hero select, set strip, results, the rematch vote), the search
 * ribbon while training, leaving a set and what comes after it (search
 * again or the online hub). The rules of a set are the game's (Stiletto's
 * arena director publishes them in serverinfo arena_* keys); this file only
 * shows them and sends the player's choices. docs/spec/setflow.md
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../net/live.h"
#include "../render/render.h"
#include "../server/sv_local.h"
#include "../ui/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETFLOW_URL "fte://data/web/setflow/index.html"
#define ONLINE_URL "fte://data/web/online/index.html"

static cvar_t *set_after, *setflow_enabled, *setflow_training_map, *setflow_layout;

/* the serverinfo keys the view reads, kept while in a set game because
 * serverinfo is cleared on disconnect */
static const char *const arena_keys[] = {
	"arena_state", "arena_set", "arena_set_serial", "arena_set_closed", "arena_set_log", "arena_set_left",
	"arena_deadline", "arena_lock", "arena_vote", "arena_ents", "arena_hero1", "arena_hero2", "arena_heroes",
	"arena_name1", "arena_name2", "arena_score1", "arena_score2", "arena_round", "arena_result", "arena_winner"};
#define NKEYS (sizeof(arena_keys) / sizeof(arena_keys[0]))

static struct {
	int view;
	bool view_input;
	bool set_game;      /* the game we're in plays sets */
	bool was_set_game;  /* ... and the one we just left did */
	int serial;         /* arena_set_serial when we joined, -1 unknown */
	bool leaving;       /* the set closed: leaving the server */
	double leave_at;
	char closed[64];    /* why the set closed */
	char playlist[32];  /* the Live playlist of the set, for searching again */
	char notice[160];
	int notice_seq;
	int prevstate;
	char my_hero[33];   /* what this player picked (hidden from serverinfo until the reveal) */
	uint64_t match_seen;   /* the newest Live match id */
	double match_seen_at;  /* when it arrived */
	char last[NKEYS][256]; /* the arena keys of the last frame in a set game */
	int last_me;
	bool searching_after; /* set_after search: queue once Live has us back */
	bool training;        /* the map we run is a training map while searching */
	bool prevstate_training;
} sf = {.serial = -1, .prevstate = ca_disconnected, .last_me = -1};

static bool in_game(void) { return cl.state == ca_active; }

static void key(const char *k, char *out, size_t n)
{
	const char *v = CL_ServerKey(k, out, n);
	if (!v)
		out[0] = 0;
	else if (v != out)
		Str_Copy(out, v, n);
}

static int key_int(const char *k, int dflt)
{
	char b[32];
	key(k, b, sizeof(b));
	return b[0] ? atoi(b) : dflt;
}

/* the n-th space-separated number of a key */
static int word_int(const char *s, int n, int dflt)
{
	for (int i = 0; *s; i++) {
		while (*s == ' ')
			s++;
		if (!*s)
			break;
		if (i == n)
			return atoi(s);
		while (*s && *s != ' ')
			s++;
	}
	return dflt;
}

/* this player's slot in the set, from arena_ents, or -1 */
static int my_slot(void)
{
	char ents[64];
	key("arena_ents", ents, sizeof(ents));
	for (int i = 0; i < 2; i++) {
		int e = word_int(ents, i, 0);
		if (e && e == cl.playerent)
			return i;
	}
	return -1;
}

static bool set_game_now(void)
{
	char s[64];
	if (!in_game())
		return false;
	key("arena_set", s, sizeof(s));
	return s[0] && atoi(s) > 0;
}

static void notice(const char *msg)
{
	Str_Copy(sf.notice, msg, sizeof(sf.notice));
	sf.notice_seq++;
	Con_Printf("setflow: %s\n", msg);
}

static const char *live_state(void)
{
	cvar_t *v = Cvar_Find("live_state");
	return v && v->string[0] == '{' ? v->string : "{}";
}

static bool live_queued(void) { return strstr(live_state(), "\"queue\":{") != NULL; }

/* --- hero pictures ----------------------------------------------------------------
 * Shots of each hero's model (r_modelshot.c), the way FTE menus show a
 * player model through a render target: the select's stage shows the hero
 * under the cursor, posed in its idle and drawn again every frame
 * ($rt:heropreview), and the roster tiles show portraits drawn once
 * ($rt:portrait_<hero>). The model and its idle come from the hero's
 * entityDef (decls/def/heroes/<hero>.def, following "inherit"). */

#define PREVIEW_W 512
#define PREVIEW_H 608
#define PORTRAIT_W 264
#define PORTRAIT_H 224
#define MAX_HEROPICS 24

typedef struct {
	char id[33];
	char model[128], anim[64];
	bool looked; /* the def was read */
	bool drawn;  /* its portrait */
	bool want;   /* asked for a portrait */
} heropic_t;

static heropic_t heropics[MAX_HEROPICS];
static char preview_id[33];
static double preview_start;

/* the block of entityDef name in the file at path, or NULL (Mem_Free it) */
static char *def_block(const char *path, const char *name)
{
	char tok[256], *data = FS_LoadFile(path, NULL), *out = NULL;
	const char *p = data;
	if (!data)
		return NULL;
	while ((p = COM_Parse(p, tok, sizeof(tok)))) {
		if (Str_ICmp(tok, "entityDef"))
			continue;
		if (!(p = COM_Parse(p, tok, sizeof(tok))))
			break;
		if (Str_ICmp(tok, name))
			continue;
		{
			const char *start = strchr(p, '{'), *end = start ? strchr(start, '}') : NULL;
			if (start && end) {
				out = Mem_Alloc((size_t)(end - start) + 1);
				memcpy(out, start + 1, (size_t)(end - start - 1));
			}
		}
		break;
	}
	Mem_Free(data);
	return out;
}

/* looks up key in entityDef name, then in what it inherits */
static bool def_key(const char *name, const char *k, char *val, size_t n, int depth)
{
	static const char *const files[] = {"decls/def/heroes/%s.def", "decls/def/%s.def", "decls/def/player.def",
	                                    "decls/def/heroes.def"};
	char path[128], tok[256], key_[64], inherit[64] = "";
	char *block = NULL;
	bool found = false;
	if (depth > 4)
		return false;
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]) && !block; i++) {
		snprintf(path, sizeof(path), files[i], name);
		block = def_block(path, name);
	}
	if (!block)
		return false;
	for (const char *p = block; (p = COM_Parse(p, key_, sizeof(key_)));) {
		if (!(p = COM_Parse(p, tok, sizeof(tok))))
			break;
		if (!Str_ICmp(key_, k)) {
			Str_Copy(val, tok, n);
			found = true;
		} else if (!Str_ICmp(key_, "inherit")) {
			Str_Copy(inherit, tok, sizeof(inherit));
		}
	}
	Mem_Free(block);
	return found || (inherit[0] && def_key(inherit, k, val, n, depth + 1));
}

static bool id_ok(const char *id)
{
	size_t n = strlen(id);
	if (!n || n > 32)
		return false;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_')
			return false;
	return true;
}

static heropic_t *heropic(const char *id)
{
	heropic_t *h = NULL;
	if (!id_ok(id))
		return NULL;
	for (int i = 0; i < MAX_HEROPICS && !h; i++)
		if (!heropics[i].id[0] || !strcmp(heropics[i].id, id))
			h = &heropics[i];
	if (!h)
		return NULL;
	if (!h->looked) {
		Str_Copy(h->id, id, sizeof(h->id));
		h->looked = true;
		def_key(id, "model", h->model, sizeof(h->model), 0);
		def_key(id, "act_idle", h->anim, sizeof(h->anim), 0);
		/* "a,b": the first */
		if (strchr(h->anim, ','))
			*strchr(h->anim, ',') = 0;
	}
	return h;
}

static void shot(const char *name, const heropic_t *h, float time, float yaw, int w, int hgt, bool portrait)
{
	r_modelshot_t s = {0};
	s.model = h ? h->model : NULL;
	s.anim = h ? h->anim : NULL;
	s.time = time;
	s.yaw = yaw;
	s.width = w;
	s.height = hgt;
	s.portrait = portrait;
	R_ModelShot(name, &s);
}

/* between R_BeginFrame and the client game's frame: draws the pictures */
void SF_Render(void)
{
	char state[32];
	int drawn = 0;
	if (!R_Active())
		return;
	for (int i = 0; i < MAX_HEROPICS && drawn < 2; i++) {
		heropic_t *h = &heropics[i];
		if (!h->want || h->drawn)
			continue;
		h->drawn = true;
		shot(va("portrait_%s", h->id), h, 0, 15, PORTRAIT_W, PORTRAIT_H, true);
		drawn++;
	}
	key("arena_state", state, sizeof(state));
	if (preview_id[0] && sf.view && !strcmp(state, "SELECT")) {
		heropic_t *h = heropic(preview_id);
		shot("heropreview", h, (float)(Sys_Seconds() - preview_start), 20, PREVIEW_W, PREVIEW_H, false);
	}
}

/* --- the view --------------------------------------------------------------------- */

static void close_view(void)
{
	if (sf.view)
		UI_Close(sf.view);
	sf.view = 0;
	sf.view_input = false;
}

static bool view_wants_input(void)
{
	char state[32], locks[32], votes[32];
	int me;
	if (sf.leaving)
		return false;
	if (!sf.set_game)
		return false;
	me = my_slot();
	if (me < 0)
		return false;
	key("arena_state", state, sizeof(state));
	key("arena_lock", locks, sizeof(locks));
	key("arena_vote", votes, sizeof(votes));
	if (!strcmp(state, "SELECT"))
		return word_int(locks, me, 0) == 0;
	if (!strcmp(state, "SET END"))
		return word_int(votes, me, 0) == 0;
	return false;
}

static void view_frame(void)
{
	bool want = setflow_enabled->ivalue && in_game() && (sf.set_game || sf.leaving || live_queued() || sf.training);
	if (sf.view && !UI_IsOpen(sf.view)) {
		sf.view = 0;
		sf.view_input = false;
	}
	if (!want) {
		close_view();
		return;
	}
	if (!sf.view) {
		/* the stage's picture exists before the layout looks it up */
		shot("heropreview", NULL, 0, 0, PREVIEW_W, PREVIEW_H, false);
		preview_id[0] = 0;
		sf.view = UI_Open(setflow_layout->string, 0);
		if (!sf.view)
			sf.view = UI_Open("layout/web/setflow.xml", 0);
		sf.view_input = false;
		if (!sf.view)
			return;
	}
	bool input = view_wants_input();
	if (input != sf.view_input) {
		UI_SetFlags(sf.view, input ? UI_INPUT : 0);
		sf.view_input = input;
	}
}

/* --- leaving a set ---------------------------------------------------------------- */

static const char *closed_text(const char *why)
{
	if (!strcmp(why, "rematch declined"))
		return "Rematch declined";
	if (!strcmp(why, "time"))
		return "No rematch: time ran out";
	if (!strcmp(why, "left"))
		return "Your opponent left the set";
	if (!strcmp(why, "void"))
		return "The set was void";
	return "The set is over";
}

static void after_set(bool may_search)
{
	sf.was_set_game = false;
	/* local games after this one aren't sets */
	if (Cvar_Find("arena_setFirstTo"))
		Cvar_Set("arena_setFirstTo", "0");
	if (may_search && !Str_ICmp(set_after->string, "search") && sf.playlist[0]) {
		sf.searching_after = true;
		WC_OpenPage(ONLINE_URL "?search=1");
	} else {
		WC_OpenPage(ONLINE_URL);
	}
}

static void state_frame(void)
{
	bool now = set_game_now();
	if (now) {
		int serial = key_int("arena_set_serial", 0);
		if (!sf.set_game) {
			/* joined a set game */
			sf.serial = serial;
			sf.leaving = false;
			sf.my_hero[0] = 0;
			cvar_t *pl = Cvar_Find("live_match_playlist");
			Str_Copy(sf.playlist, pl && pl->string[0] ? pl->string : "duel", sizeof(sf.playlist));
		}
		sf.set_game = sf.was_set_game = true;
		sf.training = false;
		for (size_t i = 0; i < NKEYS; i++)
			key(arena_keys[i], sf.last[i], sizeof(sf.last[i]));
		sf.last_me = my_slot();
		{
			/* one line a state, for logs and the scripted tests */
			static char prev[64];
			char st[32], set[64], lock[16], vote[16];
			key("arena_state", st, sizeof(st));
			key("arena_set", set, sizeof(set));
			key("arena_lock", lock, sizeof(lock));
			key("arena_vote", vote, sizeof(vote));
			char line[64];
			snprintf(line, sizeof(line), "%s|%s|%s|%s", st, set, lock, vote);
			if (strcmp(line, prev)) {
				Str_Copy(prev, line, sizeof(prev));
				Con_Printf("setflow: state \"%s\" set \"%s\" lock \"%s\" vote \"%s\" me %d\n", st, set, lock, vote, sf.last_me);
			}
		}
		if (!sf.leaving && serial > sf.serial) {
			char why[64];
			key("arena_set_closed", why, sizeof(why));
			Str_Copy(sf.closed, why, sizeof(sf.closed));
			sf.leaving = true;
			sf.leave_at = Sys_Seconds() + 2.5;
			notice(closed_text(why));
		}
		if (sf.leaving && Sys_Seconds() >= sf.leave_at) {
			sf.leave_at = 1e30;
			Cbuf_AddText("disconnect\n");
		}
	} else {
		sf.set_game = false;
	}

	/* the set game ended */
	if (sf.prevstate != ca_disconnected && cl.state == ca_disconnected && sf.was_set_game) {
		/* leaving a set yourself goes back to the hub without searching */
		bool self = !sf.leaving && !cl.disconnect_reason[0];
		if (!sf.leaving)
			notice(self ? "You left the set" : "The set ended: the host left or the connection dropped");
		sf.leaving = false;
		after_set(!self);
	}
	sf.prevstate = cl.state;

	/* left the training map: back to the hub */
	if (sf.prevstate_training && cl.state == ca_disconnected && !sf.was_set_game) {
		sf.training = false;
		Cvar_Set("g_nomotd", "0");
		WC_OpenPage(ONLINE_URL);
	}
	sf.prevstate_training = sf.training && cl.state != ca_disconnected;

	/* searching again once the set's server is gone */
	if (sf.searching_after && cl.state == ca_disconnected && !sv.active) {
		sf.searching_after = false;
		if (Live_Connected() && !live_queued())
			Cbuf_AddText(va("live_queue %s\n", sf.playlist));
	}

	/* a new Live match */
	uint64_t id = Live_MatchId();
	if (id && id != sf.match_seen) {
		sf.match_seen = id;
		sf.match_seen_at = Sys_Seconds();
		sf.training = false;
	}
}

void SF_Frame(void)
{
	state_frame();
	view_frame();
}

/* --- the bridge ------------------------------------------------------------------- */

static void json_str(strbuf_t *sb, const char *s)
{
	SB_Append(sb, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			SB_Printf(sb, "\\%c", c);
		else if (c < 0x20)
			SB_Printf(sb, "\\u%04x", c);
		else
			SB_Append(sb, s, 1);
	}
	SB_Append(sb, "\"", 1);
}

static void q_getflow(strbuf_t *out)
{
	char my_hero[64] = "";
	bool live_arena = sf.set_game;
	SB_AppendStr(out, "{\"ingame\":");
	SB_AppendStr(out, in_game() ? "true" : "false");
	SB_Printf(out, ",\"setgame\":%s,\"leaving\":%s,\"training\":%s,\"host\":%s", sf.set_game ? "true" : "false",
	          sf.leaving ? "true" : "false", sf.training ? "true" : "false",
	          Live_MatchHost() && Live_MatchHost() == Live_MyId() ? "true" : "false");
	SB_Printf(out, ",\"me\":%d,\"myid\":%u", live_arena ? my_slot() : sf.last_me, Live_MyId());
	if (in_game() && cl.playerslot >= 0) {
		CL_PlayerKey(cl.playerslot, "*hero", my_hero, sizeof(my_hero));
	}
	SB_AppendStr(out, ",\"myhero\":");
	json_str(out, my_hero[0] ? my_hero : sf.my_hero);
	SB_AppendStr(out, ",\"arena\":{");
	for (size_t i = 0; i < NKEYS; i++) {
		char v[256];
		if (live_arena)
			key(arena_keys[i], v, sizeof(v));
		else
			Str_Copy(v, sf.last[i], sizeof(v));
		SB_Printf(out, "%s\"%s\":", i ? "," : "", arena_keys[i] + 6);
		json_str(out, v);
	}
	SB_AppendStr(out, "},\"notice\":");
	json_str(out, sf.notice);
	SB_Printf(out, ",\"notice_seq\":%d,\"after\":", sf.notice_seq);
	json_str(out, set_after->string);
	SB_Printf(out, ",\"match_age\":%.1f", sf.match_seen ? Sys_Seconds() - sf.match_seen_at : 1e6);
	SB_AppendStr(out, ",\"live\":");
	SB_AppendStr(out, live_state());
	SB_AppendStr(out, "}");
}

static bool hero_ok(const char *id)
{
	char ids[1024];
	size_t n = strlen(id);
	if (!n || n > 32)
		return false;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_')
			return false;
	key("arena_heroes", ids, sizeof(ids));
	for (const char *p = ids; *p;) {
		while (*p == ' ')
			p++;
		const char *e = p;
		while (*e && *e != ' ')
			e++;
		if ((size_t)(e - p) == n && !strncmp(p, id, n))
			return true;
		p = e;
	}
	return false;
}

static bool playlist_ok(const char *p)
{
	size_t n = strlen(p);
	if (!n || n > 31)
		return false;
	for (; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
			return false;
	return true;
}

/* flow:<verb>[:<arg>] */
static void q_flow(const char *a, strbuf_t *out)
{
	const char *arg = strchr(a, ':');
	char verb[32];
	size_t n = arg ? (size_t)(arg - a) : strlen(a);
	if (n >= sizeof(verb))
		n = sizeof(verb) - 1;
	memcpy(verb, a, n);
	verb[n] = 0;
	arg = arg ? arg + 1 : "";
	bool ok = true;

	if (!strcmp(verb, "connect")) {
		Cbuf_AddText("live_connect\n");
	} else if (!strcmp(verb, "queue")) {
		const char *pl = *arg ? arg : "duel";
		if (!playlist_ok(pl))
			ok = false;
		else if (!Live_Connected())
			Cbuf_AddText(va("set live_onconnect \"live_queue %s\"\nlive_connect\n", pl));
		else
			Cbuf_AddText(va("live_queue %s\n", pl));
		if (ok)
			Str_Copy(sf.playlist, pl, sizeof(sf.playlist));
	} else if (!strcmp(verb, "game")) {
		/* the Live game id, when the game's configs didn't set one */
		cvar_t *g = Cvar_Find("live_game");
		if (!playlist_ok(arg) || (g && g->string[0]))
			ok = false;
		else
			Cvar_Set("live_game", arg);
	} else if (!strcmp(verb, "cancel")) {
		sf.searching_after = false;
		Cbuf_AddText("live_cancel\n");
	} else if (!strcmp(verb, "training")) {
		/* a local map to practise on while searching: no sets there */
		sf.training = true;
		Cbuf_AddText(va("set arena_setFirstTo 0\nset g_nomotd 1\nset g_gametype duel\nset deathmatch 1\nset coop 0\nset sv_background 0\nmap %s\n",
		                setflow_training_map->string));
	} else if (!strcmp(verb, "pick") || !strcmp(verb, "lock")) {
		if (!sf.set_game || !hero_ok(arg)) {
			ok = false;
		} else {
			Str_Copy(sf.my_hero, arg, sizeof(sf.my_hero));
			Cbuf_AddText(va("cmd selecthero %s\n", arg));
			if (!strcmp(verb, "lock"))
				Cbuf_AddText("cmd lockhero\n");
		}
	} else if (!strcmp(verb, "rematch") || !strcmp(verb, "rematch_hero") || !strcmp(verb, "leave")) {
		if (!sf.set_game)
			ok = false;
		else
			Cbuf_AddText(va("cmd set_%s\n", verb));
	} else if (!strcmp(verb, "after")) {
		if (!strcmp(arg, "search") || !strcmp(arg, "menu"))
			Cvar_Set("set_after", arg);
		else
			ok = false;
	} else if (!strcmp(verb, "hub")) {
		WC_OpenPage(ONLINE_URL);
	} else if (!strcmp(verb, "quitset")) {
		/* leave a running set from the pause menu: a forfeit */
		Cbuf_AddText("disconnect\n");
	} else if (!strcmp(verb, "card")) {
		Cbuf_AddText(va("live_card %u\n", (unsigned)strtoul(arg, NULL, 10)));
	} else if (!strcmp(verb, "preview")) {
		/* the hero the select's stage shows ("" for none); says whether
		 * it has a model to show */
		heropic_t *h = *arg ? heropic(arg) : NULL;
		if (*arg && !h) {
			ok = false;
		} else {
			if (strcmp(preview_id, arg)) {
				Str_Copy(preview_id, arg, sizeof(preview_id));
				preview_start = Sys_Seconds();
			}
			SB_Printf(out, "{\"ok\":true,\"model\":%s}", h && h->model[0] && R_Active() ? "true" : "false");
			return;
		}
	} else if (!strcmp(verb, "portraits")) {
		/* id,id,...: their portraits ($rt:portrait_<id>) exist from now on
		 * and are drawn over the next frames; lists those with a model */
		char ids[512], *p, *e;
		bool first = true;
		Str_Copy(ids, arg, sizeof(ids));
		SB_AppendStr(out, "{\"ok\":true,\"have\":[");
		for (p = ids; p && *p; p = e) {
			heropic_t *h;
			if ((e = strchr(p, ',')))
				*e++ = 0;
			if (!(h = heropic(p)) || !h->model[0] || !R_Active())
				continue;
			h->want = true;
			if (!h->drawn)
				shot(va("portrait_%s", h->id), NULL, 0, 0, PORTRAIT_W, PORTRAIT_H, true);
			SB_Printf(out, "%s\"%s\"", first ? "" : ",", h->id);
			first = false;
		}
		SB_AppendStr(out, "]}");
		return;
	} else if (!strcmp(verb, "dismiss")) {
		sf.notice[0] = 0;
		sf.notice_seq++;
	} else {
		ok = false;
	}
	SB_AppendStr(out, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

bool SF_Query(const char *req, strbuf_t *out)
{
	if (!strcmp(req, "getflow")) {
		q_getflow(out);
		return true;
	}
	if (!strncmp(req, "flow:", 5)) {
		q_flow(req + 5, out);
		return true;
	}
	return false;
}

bool SF_SetGame(void)
{
	/* serverinfo is there before the game is active, when the client game
	 * starts and asks for its team menu */
	char s[64];
	if (cl.state == ca_disconnected)
		return false;
	if (sf.training)
		return true;
	key("arena_set", s, sizeof(s));
	return s[0] && atoi(s) > 0;
}

/* in a running set (not yet decided): leaving it is a forfeit */
bool SF_InRunningSet(void)
{
	char s[64];
	if (!sf.set_game || sf.leaving)
		return false;
	key("arena_set", s, sizeof(s));
	return word_int(s, 5, 0) == 0;
}

void SF_Init(void)
{
	set_after = Cvar_Get("set_after", "search", CVAR_ARCHIVE,
	                     "After leaving a Live set: search (queue the same playlist again) or menu");
	setflow_enabled = Cvar_Get("setflow_enabled", "1", 0, "Show the set view in Live set games");
	setflow_layout = Cvar_Get("setflow_layout", "layout/web/setflow-v2.xml", CVAR_ARCHIVE,
	                          "The set view's layout: setflow-v2.xml (Deadlock-style hero select) or setflow.xml");
	setflow_training_map = Cvar_Get("setflow_training_map", "duel_alley", CVAR_ARCHIVE,
	                                "Map played while searching for a match");
}
