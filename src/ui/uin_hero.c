/* Bulwark engine - native UI: the hero picker (design 6.4), the page
 * cl_webcore.c opens as ql-menu?mode=arena for "menu_webcore arena".
 *
 * Two faces on one 640x480 panel (ql-menu's scoreInset):
 * - duel: a port of ql-menu/app.js drawArenaSelector, as Bulwark's
 *   Panorama port draws it (data/panorama/scripts/web/ql-menu.js), driven
 *   by the client game's webcore_arena_snapshot (getarena) and answered
 *   through arena_action:<...>;
 * - the other rules: the eight heroes in the same chips, picked with the
 *   command each rules file takes.
 * Which one is decided on every poll: duel is serverinfo mode "duel" or a
 * live arena snapshot, so the picker follows a map change.
 *
 * UI_PreCommand (ui.h) lives here too: it is how the picker opens on a
 * first deathmatch spawn instead of the client game's mouse-only one.
 * ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../common/json.h"

#include <ctype.h>
#include <stdio.h>

#define MAX_HEROES 8
#define MAX_ITEMS (MAX_HEROES + 4)
/* ql-menu/app.js syncArena polls getarena every 120 ms */
#define ARENA_POLL_S 0.12
#define CLIENT_POLL_S 0.5
/* the Panorama port's toast: the status line shows FOCUS for 1.65 s */
#define FLASH_S 1.65

/* the roster when nothing names one: rules/shared.qc Hero_RegisterRoster */
static const char *const default_heroes[MAX_HEROES] = {
	"hero_collier", "hero_beaumont", "hero_vagrant", "hero_skipstream",
	"hero_angel", "hero_motionblue", "hero_archstiletto", "hero_juliet"};

/* colours of drawArenaSelector */
#define C_WHITE 0xffffffffu
#define C_LIVE 0x7dff9affu
#define C_P1 0x9ecfffffu
#define C_P2 0xff8a78ffu
#define C_GREY_AA 0xaaaaaaffu
#define C_GREY_BB 0xbbbbbbffu
#define C_GREY_DD 0xddddddffu
#define C_GREY_77 0x777777ffu
#define C_GREY_99 0x999999ffu
#define C_STATUS 0xaeb8c6ffu

/* what an item does */
enum {
	ACT_HERO,      /* pick heroes[hero] */
	ACT_JOIN,      /* arena_action:join */
	ACT_PLAY,      /* arena_action:play */
	ACT_READY,     /* arena_action:ready */
	ACT_CHOOSE,    /* arena_action:choosehero */
	ACT_SPECTATE,  /* arena_action:spectate (also LEAVE QUEUE) */
	ACT_TEAM       /* the team to join with the pick: hero is the team, 1.. */
};

#define MAX_TEAMS 4

typedef struct {
	int act, hero;
	float x, y;
	bool join;      /* bigred 103x24, else a navright chip 80x20 */
	bool yellow;    /* navrighty art (app.js opts.img) */
	const char *label;
} item_t;

typedef struct {
	/* getclient */
	char mode[32], hero[40];
	bool inGame;
	int team;           /* the local player's, 0 for none (spectating) */
	char teams[MAX_TEAMS][24];
	int nteams;
	int join_team;      /* the team a pick joins first, when team is 0 */
	char roster[384];   /* the heroes the rules let the team play (getclient) */
	/* getarena (webcore.md 8.2) */
	bool active;
	char phase[24], round[32], map[40];
	int readyCount;
	char players[2][48];
	int nplayers;
	int localState;
	char localHero[40];
	bool needsHero;
	/* heroes: the arena's list in duel, else webcore_arena_hero_ids or
	 * the default roster */
	char heroes[MAX_HEROES][40];
	int nheroes;

	bool duel;          /* drawn as drawArenaSelector */
	item_t items[MAX_ITEMS];
	int nitems;
	int focus_act, focus_hero; /* what has the focus, kept across rebuilds */
	double next_arena, next_client;
	char status[64];
	double flash_until;
} hero_t;

/* --- data ----------------------------------------------------------------------------- */

static bool valid_id(const char *id)
{
	if (!*id)
		return false;
	for (; *id; id++)
		if (!isalnum((unsigned char)*id) && *id != '_')
			return false;
	return true;
}

/* a name for the screen: colour codes (^1, ^x123) out */
static void plain_name(char *out, size_t size, const char *s)
{
	size_t n = 0;
	while (*s && n + 1 < size) {
		if (s[0] == '^' && isdigit((unsigned char)s[1])) {
			s += 2;
			continue;
		}
		if (s[0] == '^' && s[1] == 'x' && s[2] && s[3] && s[4]) {
			s += 5;
			continue;
		}
		out[n++] = *s++;
	}
	out[n] = 0;
}

static void set_heroes_from_list(hero_t *h, const char *list)
{
	h->nheroes = 0;
	while (*list && h->nheroes < MAX_HEROES) {
		char id[40];
		size_t n = 0;
		while (*list == ' ')
			list++;
		while (*list && *list != ' ' && n + 1 < sizeof(id))
			id[n++] = *list++;
		while (*list && *list != ' ')
			list++;
		id[n] = 0;
		if (valid_id(id))
			Str_Copy(h->heroes[h->nheroes++], id, sizeof(h->heroes[0]));
	}
}

static void parse_arena(hero_t *h, const char *reply)
{
	json_t *j = JSON_Parse(reply, NULL, 0), *a, *local;
	if (!j || !JSON_GetBool(j, "active", false)) {
		h->active = false;
		JSON_Free(j);
		return;
	}
	h->active = true;
	Str_Copy(h->phase, JSON_GetString(j, "phase", "WARMUP"), sizeof(h->phase));
	Str_Copy(h->round, JSON_GetString(j, "round", "Warmup"), sizeof(h->round));
	Str_Copy(h->map, JSON_GetString(j, "map", "unknown"), sizeof(h->map));
	for (char *c = h->map; *c; c++)
		*c = (char)toupper((unsigned char)*c);
	h->readyCount = (int)JSON_GetNumber(j, "readyCount", 0);
	/* players.filter(Boolean).slice(0, 2) */
	h->nplayers = 0;
	JSON_FOREACH(it, JSON_Get(j, "players"))
		if (h->nplayers < 2 && JSON_IsString(it) && it->str[0])
			plain_name(h->players[h->nplayers++], sizeof(h->players[0]), it->str);
	local = JSON_Get(j, "local");
	h->localState = (int)JSON_GetNumber(local, "state", 0);
	Str_Copy(h->localHero, JSON_GetString(local, "hero", ""), sizeof(h->localHero));
	h->needsHero = JSON_GetBool(local, "needsHero", false);
	h->nheroes = 0;
	if ((a = JSON_Get(j, "heroes")))
		JSON_FOREACH(it, a) {
			const char *id = JSON_GetString(it, "id", "");
			if (h->nheroes < MAX_HEROES && valid_id(id))
				Str_Copy(h->heroes[h->nheroes++], id, sizeof(h->heroes[0]));
		}
	JSON_Free(j);
}

static void parse_client(hero_t *h, const char *reply)
{
	json_t *j = JSON_Parse(reply, NULL, 0);
	if (!j)
		return;
	Str_Copy(h->mode, JSON_GetString(j, "mode", ""), sizeof(h->mode));
	Str_Copy(h->hero, JSON_GetString(j, "hero", ""), sizeof(h->hero));
	h->inGame = JSON_GetBool(j, "inGame", false);
	h->team = (int)JSON_GetNumber(j, "team", 0);
	Str_Copy(h->roster, JSON_GetString(j, "roster", ""), sizeof(h->roster));
	h->nteams = 0;
	JSON_FOREACH(it, JSON_Get(j, "teams"))
		if (h->nteams < MAX_TEAMS && JSON_IsString(it))
			Str_Copy(h->teams[h->nteams++], it->str, sizeof(h->teams[0]));
	JSON_Free(j);
}

/* the team rules (teamdm, domination) start a player spectating with no
 * team, and take no hero until they join one (Hero_TrySelectEx): the
 * picker joins the chosen team first */
static bool needs_team(const hero_t *h)
{
	return !h->duel && h->team == 0 && h->nteams >= 2;
}

/* the focusable items in focus order: heroes row-major, then buttons,
 * where drawArenaSelector (or the generic picker) puts them */
static void add_item(hero_t *h, int act, int hero, float x, float y, bool join, bool yellow, const char *label)
{
	item_t *it = &h->items[h->nitems++];
	it->act = act;
	it->hero = hero;
	it->x = x;
	it->y = y;
	it->join = join;
	it->yellow = yellow;
	it->label = label;
}

static void rebuild_items(hero_t *h)
{
	const float gridX = 300, colStep = 88, chipX = 468, joinX = 445;
	int focus = -1;
	h->nitems = 0;
	if (!h->duel || (h->active && h->needsHero)) {
		const char *picked = h->duel ? h->localHero : h->hero;
		for (int i = 0; i < h->nheroes; i++)
			add_item(h, ACT_HERO, i, gridX + (i % 2) * colStep, 160 + (float)(i / 2) * 28, false,
			         !strcmp(picked, h->heroes[i]), NULL);
		if (h->duel)
			add_item(h, ACT_SPECTATE, -1, chipX, 160 + (float)((h->nheroes + 1) / 2) * 28 + 8, false, true, "SPECTATE");
		else if (needs_team(h))
			/* beside the third hero row, so left and right reach them */
			for (int i = 0; i < h->nteams && i < 2; i++)
				add_item(h, ACT_TEAM, i + 1, 92 + i * colStep, 216, false, h->join_team == i + 1, h->teams[i]);
	} else if (h->active) {
		bool canReady = h->localState == 0 && h->localHero[0] && !h->needsHero;
		float by = 206;
		if (h->localState == 2) {
			/* a spectator: back in through the join and hero path */
			add_item(h, ACT_PLAY, -1, joinX, by, true, false, "JOIN QUEUE");
		} else if (h->localState == 1 || h->localState == 3) {
			add_item(h, ACT_CHOOSE, -1, chipX, by, false, false, "CHOOSE HERO");
			add_item(h, ACT_SPECTATE, -1, chipX, by + 30, false, true, "LEAVE QUEUE");
		} else if (canReady) {
			add_item(h, ACT_READY, -1, joinX, by, true, false, "READY UP");
			add_item(h, ACT_CHOOSE, -1, chipX, by + 30, false, false, "CHOOSE HERO");
			add_item(h, ACT_SPECTATE, -1, chipX, by + 60, false, true, "LEAVE QUEUE");
		} else {
			add_item(h, ACT_JOIN, -1, joinX, by, true, false, "JOIN MATCH");
			add_item(h, ACT_SPECTATE, -1, chipX, by + 30, false, true, "SPECTATE");
		}
	}
	/* the focus stays on what it was on, else the picked hero, else the first */
	for (int i = 0; i < h->nitems && focus < 0; i++)
		if (h->items[i].act == h->focus_act && h->items[i].hero == h->focus_hero)
			focus = i;
	for (int i = 0; i < h->nitems && focus < 0; i++)
		if (h->items[i].act == ACT_HERO && h->items[i].yellow)
			focus = i;
	if (focus < 0)
		focus = 0;
	if (h->nitems) {
		h->focus_act = h->items[focus].act;
		h->focus_hero = h->items[focus].hero;
	}
}

static int focus_index(const hero_t *h)
{
	for (int i = 0; i < h->nitems; i++)
		if (h->items[i].act == h->focus_act && h->items[i].hero == h->focus_hero)
			return i;
	return 0;
}

/* polls what is due; false when a query closed v */
static bool poll(uin_view_t *v, hero_t *h, bool force)
{
	const char *r;
	double now = uin_now();
	bool changed = false;
	if (force || now >= h->next_client) {
		if (!(r = uin_query(v, "getclient")))
			return false;
		parse_client(h, r);
		h->next_client = now + CLIENT_POLL_S;
		changed = true;
	}
	if (force || now >= h->next_arena) {
		if (!(r = uin_query(v, "getarena")))
			return false;
		parse_arena(h, r);
		h->next_arena = now + ARENA_POLL_S;
		changed = true;
	}
	if (!changed)
		return true;
	h->duel = !Str_ICmp(h->mode, "duel") || h->active;
	if (!h->duel) {
		/* the rules' roster for the team (serverinfo teamclass_<n>); rules
		 * that name teams but no roster (domination) take no hero at all.
		 * Without either, the arena's list, else shared.qc's eight. */
		set_heroes_from_list(h, h->roster);
		if (!h->nheroes && !h->nteams) {
			if (!(r = uin_query(v, "cvar:webcore_arena_hero_ids")))
				return false;
			set_heroes_from_list(h, r);
			if (!h->nheroes)
				for (int i = 0; i < MAX_HEROES; i++)
					Str_Copy(h->heroes[h->nheroes++], default_heroes[i], sizeof(h->heroes[0]));
		}
		/* lastmanstanding.qc has no client command handler at all */
		if (!Str_ICmp(h->mode, "lastmanstanding"))
			h->nheroes = 0;
	}
	rebuild_items(h);
	return true;
}

/* --- actions --------------------------------------------------------------------------- */

static void toast(hero_t *h, const char *msg)
{
	Str_Copy(h->status, msg, sizeof(h->status));
	h->flash_until = uin_now() + FLASH_S;
}

/* an arena_action as ql-menu's arenaAction sends it; false when the
 * query closed v (close, spectate) */
static bool arena_action(uin_view_t *v, hero_t *h, const char *action)
{
	char req[80];
	const char *r;
	snprintf(req, sizeof(req), "arena_action:%s", action);
	if (!(r = uin_query(v, req)))
		return false;
	if (strstr(r, "\"ok\":true")) {
		if (!strcmp(action, "spectate") && !uin_query(v, "arena_action:close"))
			return false;
		if (!strncmp(action, "hero:", 5)) {
			snprintf(req, sizeof(req), "SELECT %s", action + 5);
			toast(h, req);
		} else {
			toast(h, action);
		}
	} else {
		toast(h, "Action unavailable");
	}
	return true;
}

/* a pick outside the arena, with the command the rules take (design 6.4):
 * deathmatch and teamdm read selecthero; domination, duel and the rest
 * only the shared changehero (rules/shared.qc Hero_HandleClientCommand) */
static void generic_pick(hero_t *h, const char *id)
{
	const char *verb = !Str_ICmp(h->mode, "deathmatch") || !Str_ICmp(h->mode, "teamdm") ? "selecthero" : "changehero";
	char cmd[128];
	if (!valid_id(id))
		return;
	/* joinTeam: the client game's own team menu sends it (csprogs "cmd
	 * joinTeam %i"); the rules spawn the player, then take the hero */
	if (needs_team(h))
		snprintf(cmd, sizeof(cmd), "cmd joinTeam %d\ncmd %s %s\nwebcore_closemenu\n", h->join_team, verb, id);
	else
		snprintf(cmd, sizeof(cmd), "cmd %s %s\nwebcore_closemenu\n", verb, id);
	uin_cmd(cmd);
}

static void activate(uin_view_t *v, hero_t *h)
{
	const item_t *it;
	char action[64];
	if (!h->nitems)
		return;
	it = &h->items[focus_index(h)];
	uin_sound(UIN_SND_ACCEPT);
	if (!h->duel) {
		if (it->act == ACT_HERO) {
			generic_pick(h, h->heroes[it->hero]);
		} else if (it->act == ACT_TEAM && !h->nheroes) {
			/* no hero to pick in these rules: the team is the whole choice */
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "cmd joinTeam %d\nwebcore_closemenu\n", it->hero);
			uin_cmd(cmd);
		} else if (it->act == ACT_TEAM) {
			h->join_team = it->hero;
			rebuild_items(h);
		}
		return;
	}
	switch (it->act) {
	case ACT_HERO:
		snprintf(action, sizeof(action), "hero:%s", h->heroes[it->hero]);
		break;
	case ACT_JOIN: Str_Copy(action, "join", sizeof(action)); break;
	case ACT_PLAY: Str_Copy(action, "play", sizeof(action)); break;
	case ACT_READY: Str_Copy(action, "ready", sizeof(action)); break;
	case ACT_CHOOSE: Str_Copy(action, "choosehero", sizeof(action)); break;
	default: Str_Copy(action, "spectate", sizeof(action)); break;
	}
	if (!arena_action(v, h, action))
		return; /* closed us */
	/* the snapshot changes a frame later: read it again soon */
	h->next_arena = uin_now() + 0.05;
}

/* spatial: left and right to the neighbour in the same row, up and down
 * to the nearest row above or below, the closest item across in it */
static void move(hero_t *h, int action)
{
	int i = focus_index(h), to = -1;
	float best = 1e9f;
	const item_t *cur;
	if (!h->nitems)
		return;
	cur = &h->items[i];
	for (int j = 0; j < h->nitems; j++) {
		const item_t *it = &h->items[j];
		float dx = it->x - cur->x, dy = it->y - cur->y, d;
		if (action == UIN_A_LEFT || action == UIN_A_RIGHT) {
			if (dy != 0 || (action == UIN_A_LEFT ? dx >= 0 : dx <= 0))
				continue;
			d = dx < 0 ? -dx : dx;
		} else {
			if (action == UIN_A_UP ? dy >= 0 : dy <= 0)
				continue;
			/* rows first, then across */
			d = (dy < 0 ? -dy : dy) * 1000 + (dx < 0 ? -dx : dx);
		}
		if (d < best) {
			best = d;
			to = j;
		}
	}
	if (to < 0)
		return;
	h->focus_act = h->items[to].act;
	h->focus_hero = h->items[to].hero;
	uin_sound(UIN_SND_MOVE);
}

static void hero_key(uin_view_t *v, int action, int key, bool repeat)
{
	hero_t *h = UIN_STATE(v, hero_t);
	(void)key;
	switch (action) {
	case UIN_A_UP: case UIN_A_DOWN: case UIN_A_LEFT: case UIN_A_RIGHT:
		move(h, action);
		break;
	case UIN_A_ACCEPT:
		if (!repeat)
			activate(v, h);
		break;
	case UIN_A_BACK:
	case UIN_A_START:
		/* ql-menu's Escape: arena_action:close, which the router answers
		 * for any rules by closing this page (inside the query) */
		uin_sound(UIN_SND_BACK);
		uin_query(v, "arena_action:close");
		break;
	}
}

/* --- frame and drawing ----------------------------------------------------------------- */

static void hero_open(uin_view_t *v)
{
	hero_t *h = UIN_STATE(v, hero_t);
	h->focus_act = -1;
	h->join_team = 1;
	poll(v, h, true);
}

static void hero_frame(uin_view_t *v, double dt)
{
	(void)dt;
	poll(v, UIN_STATE(v, hero_t), false);
}

static float g_fade = 1;

static uint32_t col(uint32_t rgba) { return uin_col_alpha(rgba, g_fade); }

/* app.js text(): the 8 px grid of standard_07_57 (design 6.4 sizes) */
static void text(int face, float x, float y, const char *s, int align, uint32_t rgba)
{
	d_text(face, x, y, s, align | UIN_BASELINE, col(rgba));
}

/* scoreInset(x, y, w, h): app.js:389-401, corners 20, sides 10, bottom 10 */
static void score_inset(float x, float y, float w, float h)
{
	const float tl = 20, side = 10, bot = 10;
	uint32_t c = col(C_WHITE);
	d_img(UIN_R_SCORE_TL, x, y, tl, tl, c);
	d_img(UIN_R_SCORE_T, x + tl, y, w - tl * 2, tl, c);
	d_img(UIN_R_SCORE_TR, x + w - tl, y, tl, tl, c);
	d_img(UIN_R_SCORE_L, x, y + tl, side, h - tl - bot, c);
	d_img(UIN_R_SCORE_M, x + side, y + tl, w - side * 2, h - tl - bot, c);
	d_img(UIN_R_SCORE_R, x + w - side, y + tl, side, h - tl - bot, c);
	d_img(UIN_R_SCORE_BL, x, y + h - bot, side, bot, c);
	d_img(UIN_R_SCORE_B, x + side, y + h - bot, w - side * 2, bot, c);
	d_img(UIN_R_SCORE_BR, x + w - side, y + h - bot, side, bot, c);
}

/* the hero's label: the id without hero_ (arenaHeroLabel) */
static const char *hero_label(const char *id, char *out, size_t size, bool upper)
{
	Str_Copy(out, !Str_NICmp(id, "hero_", 5) ? id + 5 : id, size);
	if (upper)
		for (char *c = out; *c; c++)
			*c = (char)toupper((unsigned char)*c);
	return out;
}

/* drawArenaButton: a chip (navright 80x20, label right-aligned at x+54)
 * or the join button (bigred 103x24, label at x+86). The page lit the
 * hovered one; here the focused one is lit the same way, and the join
 * button, whose art doesn't change, gets a FOCUS outline too. */
static void draw_button(const hero_t *h, const item_t *it, bool focused)
{
	char label[40];
	float w = it->join ? 103 : 80, bh = it->join ? 24 : 20;
	int art = it->join ? UIN_R_BIGRED : (it->yellow || focused) ? UIN_R_NAVRIGHTY : UIN_R_NAVRIGHT;
	const char *s = it->label;
	if (it->act == ACT_HERO)
		s = hero_label(h->heroes[it->hero], label, sizeof(label), !h->duel);
	d_img(art, it->x, it->y, w, bh, col(C_WHITE));
	if (focused && it->join)
		uin_debug_box(it->x - 1, it->y - 1, w + 2, bh + 2, NULL, col(UIN_COL_FOCUS));
	text(UIN_F_STD8, it->x + (it->join ? 86 : 54), it->y + 18, s, UIN_RIGHT,
	     focused || (it->act != ACT_SPECTATE && it->yellow) ? UIN_COL_FOCUS : C_WHITE);
}

static void draw_items(const hero_t *h)
{
	int f = focus_index(h);
	for (int i = 0; i < h->nitems; i++)
		draw_button(h, &h->items[i], i == f);
	/* the hero in play, marked as design 6.4 has it: a FOCUS bar beside it */
	for (int i = 0; i < h->nitems; i++)
		if ((h->items[i].act == ACT_HERO || h->items[i].act == ACT_TEAM) && h->items[i].yellow)
			d_fill(h->items[i].x - 5, h->items[i].y + 2, 2, 16, col(UIN_COL_FOCUS));
}

static void draw_status(const hero_t *h)
{
	/* status line and hint (app.js drawArenaSelector, last lines) */
	if (h->status[0])
		text(UIN_F_STD8, 320, 342, h->status, UIN_CENTER,
		     uin_col_alpha(uin_now() < h->flash_until ? UIN_COL_FOCUS : C_STATUS, 0.9f));
	text(UIN_F_STD8, 532, 368, "(B)  BACK", UIN_RIGHT, C_GREY_99);
}

/* drawArenaSelector, app.js:1507-1600 */
static void draw_duel(const hero_t *h)
{
	char line[96], name[48];
	bool canReady = h->localState == 0 && h->localHero[0] && !h->needsHero;
	const char *status;
	if (!h->active) {
		text(UIN_F_STD16, 320, 235, "Waiting for arena state...", UIN_CENTER, C_GREY_BB);
		draw_status(h);
		return;
	}
	text(UIN_F_STD16, 84, 88, "DUEL ARENA", UIN_LEFT, C_WHITE);
	text(UIN_F_STD8, 84, 108, h->map, UIN_LEFT, UIN_COL_FOCUS);
	text(UIN_F_STD16, 550, 88, h->phase, UIN_RIGHT, !strcmp(h->phase, "LIVE") ? C_LIVE : UIN_COL_FOCUS);
	text(UIN_F_STD8, 550, 108, h->round, UIN_RIGHT, C_GREY_AA);

	text(UIN_F_STD8, 92, 145, "CURRENT DUEL", UIN_LEFT, C_GREY_AA);
	if (h->nplayers)
		for (int i = 0; i < h->nplayers; i++)
			text(UIN_F_STD16, 105, 170 + i * 24.0f, d_text_fit(UIN_F_STD16, h->players[i], 190, name, sizeof(name)),
			     UIN_LEFT, i ? C_P2 : C_P1);
	else
		text(UIN_F_STD8, 105, 170, "Waiting for competitors", UIN_LEFT, C_GREY_77);
	snprintf(line, sizeof(line), "%d queued and ready", h->readyCount);
	text(UIN_F_STD8, 92, 232, line, UIN_LEFT, C_GREY_BB);
	status = h->needsHero ? "CHOOSE A HERO" : h->localState == 1 ? "READY" : h->localState == 2 ? "SPECTATING" :
	         h->localState == 3 ? "COMPETING" : canReady ? "PRESS READY UP" : "WAITING";
	snprintf(line, sizeof(line), "YOUR STATUS: %s", status);
	text(UIN_F_STD8, 92, 258, line, UIN_LEFT, h->needsHero || canReady ? UIN_COL_FOCUS : C_GREY_DD);
	snprintf(line, sizeof(line), "HERO: %s", h->localHero[0] ? h->localHero : "No hero selected");
	text(UIN_F_STD8, 92, 280, line, UIN_LEFT, C_GREY_AA);
	if (h->needsHero)
		text(UIN_F_STD8, 300, 145, "SELECT YOUR HERO", UIN_LEFT, UIN_COL_FOCUS);
	draw_items(h);
	draw_status(h);
}

/* the other rules: the same panel, chips and status line (design 6.4) */
static void draw_generic(const hero_t *h)
{
	char label[40], line[64];
	text(UIN_F_STD16, 84, 88, h->nheroes || !needs_team(h) ? "SELECT YOUR HERO" : "SELECT YOUR TEAM", UIN_LEFT,
	     C_WHITE);
	Str_Copy(line, h->mode[0] ? h->mode : "game", sizeof(line));
	for (char *c = line; *c; c++)
		*c = (char)toupper((unsigned char)*c);
	text(UIN_F_STD8, 84, 108, line, UIN_LEFT, UIN_COL_FOCUS);
	text(UIN_F_STD8, 92, 145, "CURRENT HERO", UIN_LEFT, C_GREY_AA);
	text(UIN_F_STD16, 105, 170, h->hero[0] ? hero_label(h->hero, label, sizeof(label), true) : "NONE", UIN_LEFT,
	     h->hero[0] ? C_P1 : C_GREY_77);
	if (h->nheroes)
		text(UIN_F_STD8, 300, 145, "HEROES", UIN_LEFT, UIN_COL_FOCUS);
	else
		text(UIN_F_STD8, 300, 145, "NO HERO CHOICE IN THIS MODE", UIN_LEFT, C_GREY_77);
	if (needs_team(h))
		text(UIN_F_STD8, 92, 206, "JOIN TEAM", UIN_LEFT, UIN_COL_FOCUS);
	draw_items(h);
	draw_status(h);
}

static void hero_draw(uin_view_t *v, float x, float y, float w, float h)
{
	hero_t *st = UIN_STATE(v, hero_t);
	uin_xf_t xf;
	uin_xf_fit(&xf, UIN_FRAME_HERO_W, UIN_FRAME_HERO_H, x, y, w, h);
	d_xf(&xf);
	g_fade = v->fade;
	/* scoreInset(68,58,504,332), app.js:1526 */
	score_inset(68, 58, 504, 332);
	if (st->duel)
		draw_duel(st);
	else
		draw_generic(st);
	d_xf(NULL);
	g_fade = 1;
}

const uin_screen_t uin_hero_screen = {"hero", hero_open, NULL, hero_key, hero_frame, hero_draw};

/* --- E7: the rules' hero menu request --------------------------------------------------- */

/* The rules stuff showClassSelectionMenu on a first deathmatch spawn
 * without a pick (rules/deathmatch.qc DM_PlayerSpawn) and through duel.
 * The client game routes it to "menu_webcore arena class" only while its
 * arena adapter runs (a live webcore_arena_snapshot), and otherwise opens
 * its own selector, which needs a mouse. So, outside the arena, the
 * request opens this picker through the router instead. With an argument
 * ("vgui": the router's own fallback, cl_webcore.c open_arena) or in the
 * arena it goes on to the client game as before. */
bool UI_PreCommand(int argc, const char **argv)
{
	cvar_t *snap;
	if (argc != 1 || (Str_ICmp(argv[0], "showClassSelectionMenu") && Str_ICmp(argv[0], "showHeroSelectionMenu")))
		return false;
	snap = Cvar_Find("webcore_arena_snapshot");
	if (snap && strstr(snap->string, "\"active\":true"))
		return false;
	Cbuf_AddText("menu_webcore arena class\n");
	return true;
}
