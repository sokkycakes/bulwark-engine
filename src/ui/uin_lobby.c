/* Bulwark engine - native UI: the lobby (lobby-menu), its Leave
 * confirmation and read-only chat (design 6.2).
 *
 * The page's 960 x 720 frame, scaled to fit and centred: the panel with
 * the map preview and its summary, then the settings as rows a pad steps
 * through (the page's dialogs and number fields need a mouse and a
 * keyboard), the player list on the right, the chat under the rows and
 * the status line at the bottom. Colours and type follow lobby-menu.css as
 * the PC port draws it.
 *
 * All state comes from getlobby (every 250 ms) and getlobbychat; changes
 * go back as lobby_action requests, sent 250 ms after the last step so a
 * held direction doesn't flood the guests with snapshots. BOTS and BOT
 * SKILL are the host's own: they become bot_minClients and bot_skill,
 * set at once before the start request. begin_start (cl_lobby.c) puts the
 * map command at the front of the console buffer, so cvar text queued
 * before it would run after the map had loaded. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../common/fs.h"
#include "../common/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* --- the frame (lobby-menu.css values / 1.5: the port draws the 960 x 720
 * page at 1.5 layout px per page px) ------------------------------------------------------ */

#define MENU_X 53.0f
#define MENU_Y 252.2f
#define MENU_W 518.4f
#define ROW_H 22.0f      /* the page's rows are 24; seven rows must clear the chat */
#define START_GAP 8.0f
#define START_H 40.0f
#define CHAT_Y 440.0f
#define CHAT_H 150.0f
#define CHAT_LINES 6
#define ROSTER_X 615.0f
#define ROSTER_Y 29.0f
#define ROSTER_W 282.0f
#define PLATE_H 44.0f
#define PLATE_GAP 3.0f
#define MAX_PLATES 6

#define COL_BAR 0xbfbfbfffu
#define COL_TEXT 0xffffffffu
#define COL_FOCUS_TEXT 0x404040ffu
#define COL_DISABLED 0x888888ffu
#define COL_SOFT 0xeaeef8ffu
#define COL_BLUE 0xadc4dfffu
#define COL_READY 0x8ae234ffu
#define COL_ERROR 0xff8c8cffu

#define DEBOUNCE_S 0.25
#define POLL_S 0.25 /* the page polls getlobby every 250 ms */

/* --- choices -------------------------------------------------------------------------------- */

static const struct {
	const char *id, *label;
} rulesets[] = {
	{"DEATHMATCH", "DEATHMATCH"}, {"TEAMDM", "TEAM DEATHMATCH"}, {"DUEL", "DUEL"},
	{"DOMINATION", "DOMINATION"}, {"LASTMANSTANDING", "LAST MAN STANDING"},
};
#define NUM_RULESETS ((int)BW_ARRAY_COUNT(rulesets))

/* the page's four (lobby-menu model.js) and the maps with bot waypoints
 * (data/<map>.way, the deathmatch cycle), filtered at open to what is
 * installed, as the lobby's own map check (map_id_ok) does */
static const char *const map_ids[] = {
	"envtest", "lean", "arch_testbed", "arch_crux_test", "lqdm1", "lqdm3", "lqdm4",
	"lqdm6", "lqdm7", "lqdm8", "lqdm9", "lqdm10", "duel_alley",
};
#define MAX_MAPS ((int)BW_ARRAY_COUNT(map_ids))

static const char *const skill_label[] = {"EASY", "NORMAL", "HARD"};

enum { ROW_RULESET, ROW_MAP, ROW_TIME, ROW_FRAGS, ROW_BOTS, ROW_SKILL, ROW_START, NUM_ROWS };
static const char *const row_label[ROW_START] = {"RULESET", "MAP", "TIME LIMIT", "FRAG LIMIT", "BOTS", "BOT SKILL"};

#define MAX_PLAYERS 16

typedef struct {
	/* the last getlobby, swapped in only after a whole parse */
	bool have;
	bool seen_active;   /* this view saw a live lobby: an error later is news */
	bool host, can_ready, can_start, can_cancel, can_change;
	int local_seat, max;
	char phase[16], network[12];
	char status[96], error[160];
	char map[40], ruleset[24];
	int timelimit, fraglimit;
	double remaining;   /* countdown seconds, or < 0 */
	int numplayers;
	struct {
		char name[32];
		int seat;
		bool host, ready, connected;
	} players[MAX_PLAYERS];
	int numchat;
	struct {
		char name[24];
		char text[96];
	} chat[CHAT_LINES];

	/* the installed maps, and which have waypoints */
	int nummaps;
	uint8_t maps[MAX_MAPS];
	bool waypoints[MAX_MAPS];

	/* the host's choices; while dirty they win over the snapshot */
	int rs, mapi, tl, fl;
	double dirty_rs, dirty_map, dirty_lim;
	int bots, skill;    /* 0 = off; 1-3 */

	int focus;
	float bar_from;
	double bar_moved;
	bool leave;         /* the Leave confirmation is up */
	int lfocus;
	double leave_opened;
	bool error_shown;
	double next_poll;
} lobby_t;

/* --- the model ------------------------------------------------------------------------------- */

static int ruleset_index(const char *id)
{
	for (int i = 0; i < NUM_RULESETS; i++)
		if (!Str_ICmp(rulesets[i].id, id))
			return i;
	return -1;
}

/* index into l->maps of a map id, -1 if it isn't offered */
static int map_index(const lobby_t *l, const char *id)
{
	for (int i = 0; i < l->nummaps; i++)
		if (!Str_ICmp(map_ids[l->maps[i]], id))
			return i;
	return -1;
}

static int humans(const lobby_t *l) { return BW_MAX(1, l->numplayers); }
static int max_bots(const lobby_t *l) { return BW_CLAMP((l->max ? l->max : 8) - humans(l), 0, 7); }

static bool local_ready(const lobby_t *l)
{
	for (int i = 0; i < l->numplayers; i++)
		if (l->players[i].seat == l->local_seat)
			return l->players[i].ready;
	return false;
}

/* a map bots can play on, when bots are on: the first with waypoints */
static void fit_map_to_bots(lobby_t *l)
{
	if (!l->bots || l->mapi < 0 || l->waypoints[l->mapi])
		return;
	for (int i = 0; i < l->nummaps; i++)
		if (l->waypoints[i]) {
			l->mapi = i;
			l->dirty_map = uin_now();
			return;
		}
}

static void parse_lobby(lobby_t *l, const char *text)
{
	json_t *j = JSON_Parse(text, NULL, 0);
	const json_t *s, *players;
	double server_time;
	if (!j)
		return;
	l->have = true;
	l->host = JSON_GetBool(j, "isHost", false);
	l->local_seat = (int)JSON_GetNumber(j, "localSeat", -1);
	l->max = (int)JSON_GetNumber(j, "max", 8);
	l->can_ready = JSON_GetBool(j, "canReady", false);
	l->can_start = JSON_GetBool(j, "canStart", false);
	l->can_cancel = JSON_GetBool(j, "canCancel", false);
	l->can_change = JSON_GetBool(j, "canChangeMap", false);
	Str_Copy(l->phase, JSON_GetString(j, "phase", ""), sizeof(l->phase));
	Str_Copy(l->network, JSON_GetString(j, "network", ""), sizeof(l->network));
	Str_Copy(l->status, JSON_GetString(j, "status", ""), sizeof(l->status));
	Str_Copy(l->error, JSON_GetString(j, "error", ""), sizeof(l->error));
	Str_Copy(l->map, JSON_GetString(j, "map", ""), sizeof(l->map));
	server_time = JSON_GetNumber(j, "serverTime", 0);
	l->remaining = JSON_GetNumber(j, "countdownEnd", 0) > 0 ? JSON_GetNumber(j, "countdownEnd", 0) - server_time : -1;
	if ((s = JSON_Get(j, "settings"))) {
		Str_Copy(l->ruleset, JSON_GetString(s, "ruleset", ""), sizeof(l->ruleset));
		l->timelimit = atoi(JSON_GetString(s, "timelimit", "0"));
		l->fraglimit = atoi(JSON_GetString(s, "fraglimit", "0"));
	}
	l->numplayers = 0;
	if ((players = JSON_Get(j, "players")))
		JSON_FOREACH(p, players) {
			if (l->numplayers == MAX_PLAYERS)
				break;
			Str_Copy(l->players[l->numplayers].name, JSON_GetString(p, "name", ""), sizeof(l->players[0].name));
			l->players[l->numplayers].seat = (int)JSON_GetNumber(p, "seat", -1);
			l->players[l->numplayers].host = JSON_GetBool(p, "host", false);
			l->players[l->numplayers].ready = JSON_GetBool(p, "ready", false);
			l->players[l->numplayers].connected = JSON_GetBool(p, "connected", true);
			l->numplayers++;
		}
	JSON_Free(j);
	if (strcmp(l->phase, "IDLE") && strcmp(l->phase, "CLOSING"))
		l->seen_active = true;
	/* the host's settings, unless a change of ours is still on its way */
	if (!l->dirty_rs)
		l->rs = ruleset_index(l->ruleset);
	if (!l->dirty_map) {
		l->mapi = map_index(l, l->map);
		/* a default map that isn't installed (the disc carries a few): the
		 * host moves to one that is */
		if (l->mapi < 0 && l->host && l->can_change && l->nummaps > 0) {
			l->mapi = 0;
			l->dirty_map = uin_now();
		}
	}
	if (!l->dirty_lim) {
		l->tl = l->timelimit;
		l->fl = l->fraglimit;
	}
	l->bots = BW_MIN(l->bots, max_bots(l));
}

static void parse_chat(lobby_t *l, const char *text)
{
	json_t *j = JSON_Parse(text, NULL, 0);
	int n = 0, skip;
	if (!j)
		return;
	skip = BW_MAX(0, j->count - CHAT_LINES);
	JSON_FOREACH(c, j) {
		if (skip-- > 0)
			continue;
		Str_Copy(l->chat[n].name, JSON_GetString(c, "name", ""), sizeof(l->chat[0].name));
		Str_Copy(l->chat[n].text, JSON_GetString(c, "text", ""), sizeof(l->chat[0].text));
		if (++n == CHAT_LINES)
			break;
	}
	l->numchat = n;
	JSON_Free(j);
}

/* sends what changed; false if the request closed the view */
static bool send_changes(uin_view_t *v, bool now)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	char req[64];
	double t = uin_now();
	if (l->dirty_rs && (now || t - l->dirty_rs >= DEBOUNCE_S)) {
		l->dirty_rs = 0;
		if (l->rs >= 0) {
			snprintf(req, sizeof(req), "lobby_action:setruleset:%s", rulesets[l->rs].id);
			if (!uin_query(v, req))
				return false;
		}
	}
	if (l->dirty_map && (now || t - l->dirty_map >= DEBOUNCE_S)) {
		l->dirty_map = 0;
		if (l->mapi >= 0) {
			snprintf(req, sizeof(req), "lobby_action:setmap:%s", map_ids[l->maps[l->mapi]]);
			if (!uin_query(v, req))
				return false;
		}
	}
	if (l->dirty_lim && (now || t - l->dirty_lim >= DEBOUNCE_S)) {
		l->dirty_lim = 0;
		snprintf(req, sizeof(req), "lobby_action:setlimits:%d:%d", l->tl, l->fl);
		if (!uin_query(v, req))
			return false;
	}
	return true;
}

static bool poll(uin_view_t *v)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	const char *r;
	if (!(r = uin_query(v, "getlobby")))
		return false;
	parse_lobby(l, r);
	if (!(r = uin_query(v, "getlobbychat")))
		return false;
	parse_chat(l, r);
	/* a lobby that ended with an error says why, once (design 6.5) */
	if (l->seen_active && l->error[0] && !l->error_shown && (!strcmp(l->phase, "CLOSING") || !strcmp(l->phase, "IDLE"))) {
		l->error_shown = true;
		uin_error_open("LOBBY CLOSED", l->error);
	}
	return true;
}

/* --- rows and keys ----------------------------------------------------------------------------- */

static bool row_enabled(const lobby_t *l, int row)
{
	if (row == ROW_START)
		return true;
	return l->host && l->can_change;
}

static float row_top(int row) { return row == ROW_START ? MENU_Y + ROW_START * ROW_H + START_GAP : MENU_Y + row * ROW_H; }

static float bar_y(const lobby_t *l)
{
	float k = uin_ease_focus((float)((uin_now() - l->bar_moved) / 0.1));
	return l->bar_from + (row_top(l->focus) - l->bar_from) * k;
}

static void set_focus(lobby_t *l, int row, bool sound)
{
	if (row == l->focus)
		return;
	l->bar_from = bar_y(l);
	l->bar_moved = uin_now();
	l->focus = row;
	if (sound)
		uin_sound(UIN_SND_MOVE);
}

/* the next enabled row up or down, wrapping (design 6.0) */
static void step_focus(lobby_t *l, int dir)
{
	int row = l->focus;
	for (int i = 0; i < NUM_ROWS; i++) {
		row = (row + dir + NUM_ROWS) % NUM_ROWS;
		if (row_enabled(l, row)) {
			set_focus(l, row, true);
			return;
		}
	}
}

/* left/right on a value row; true if it changed */
static bool step_value(lobby_t *l, int dir)
{
	double t = uin_now();
	switch (l->focus) {
	case ROW_RULESET:
		l->rs = ((l->rs < 0 ? 0 : l->rs) + dir + NUM_RULESETS) % NUM_RULESETS;
		l->dirty_rs = t;
		return true;
	case ROW_MAP:
		if (!l->nummaps)
			return false;
		/* with bots on, only maps they can find their way round */
		for (int i = 0, m = l->mapi < 0 ? (dir > 0 ? -1 : 0) : l->mapi; i < l->nummaps; i++) {
			m = (m + dir + l->nummaps) % l->nummaps;
			if (!l->bots || l->waypoints[m]) {
				if (m == l->mapi)
					return false;
				l->mapi = m;
				l->dirty_map = t;
				return true;
			}
		}
		return false;
	case ROW_TIME:
		l->tl = BW_CLAMP(l->tl + 5 * dir, 0, 180);
		l->dirty_lim = t;
		return true;
	case ROW_FRAGS:
		l->fl = BW_CLAMP(l->fl + 5 * dir, 0, 995);
		l->dirty_lim = t;
		return true;
	case ROW_BOTS: {
		int b = BW_CLAMP(l->bots + dir, 0, max_bots(l));
		if (b == l->bots)
			return false;
		l->bots = b;
		fit_map_to_bots(l);
		return true;
	}
	case ROW_SKILL:
		l->skill = (l->skill - 1 + dir + 3) % 3 + 1;
		return true;
	default:
		return false;
	}
}

/* START GAME, CANCEL, or READY for a guest */
static void press_start(uin_view_t *v)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	if (l->host && l->can_start) {
		char n[16];
		/* the settings as shown, then the bots, then start */
		if (!send_changes(v, true))
			return;
		snprintf(n, sizeof(n), "%d", humans(l) + l->bots);
		Cvar_Set("bot_minClients", l->bots ? n : "-1");
		snprintf(n, sizeof(n), "%d", l->skill);
		Cvar_Set("bot_skill", n);
		uin_sound(UIN_SND_ACCEPT);
		uin_query(v, "lobby_action:start");
	} else if (l->host && l->can_cancel) {
		uin_sound(UIN_SND_BACK);
		uin_query(v, "lobby_action:cancel");
	} else if (l->can_ready) {
		uin_sound(UIN_SND_ACCEPT);
		uin_query(v, local_ready(l) ? "lobby_action:unready" : "lobby_action:ready");
	}
}

static void leave_key(uin_view_t *v, int action)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	if (uin_now() - l->leave_opened < UIN_FADE_S)
		return;
	if (action == UIN_A_UP || action == UIN_A_DOWN || action == UIN_A_LEFT || action == UIN_A_RIGHT) {
		l->lfocus ^= 1;
		uin_sound(UIN_SND_MOVE);
	} else if (action == UIN_A_BACK) {
		l->leave = false;
		uin_sound(UIN_SND_BACK);
	} else if (action == UIN_A_ACCEPT) {
		l->leave = false;
		if (l->lfocus == 0) {
			uin_sound(UIN_SND_BACK);
			/* WC_Frame reopens the title once the lobby is gone */
			uin_query(v, "lobby_action:close");
		} else {
			uin_sound(UIN_SND_ACCEPT);
		}
	}
}

static void lobby_key(uin_view_t *v, int action, int key, bool repeat)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	(void)key;
	if (l->leave) {
		if (!repeat)
			leave_key(v, action);
		return;
	}
	switch (action) {
	case UIN_A_UP:
		step_focus(l, -1);
		break;
	case UIN_A_DOWN:
		step_focus(l, 1);
		break;
	case UIN_A_LEFT:
	case UIN_A_RIGHT:
		if (l->focus != ROW_START && row_enabled(l, l->focus) && step_value(l, action == UIN_A_LEFT ? -1 : 1))
			uin_sound(UIN_SND_MOVE);
		break;
	case UIN_A_ACCEPT:
		if (repeat)
			break;
		if (l->focus == ROW_START)
			press_start(v);
		else if (row_enabled(l, l->focus) && step_value(l, 1))
			uin_sound(UIN_SND_MOVE);
		break;
	case UIN_A_START:
		if (!repeat)
			press_start(v);
		break;
	case UIN_A_X:
		if (!repeat && l->can_ready) {
			uin_sound(UIN_SND_ACCEPT);
			uin_query(v, local_ready(l) ? "lobby_action:unready" : "lobby_action:ready");
		}
		break;
	case UIN_A_BACK:
		if (!repeat) {
			uin_sound(UIN_SND_BACK);
			l->leave = true;
			l->lfocus = 1; /* STAY first */
			l->leave_opened = uin_now();
		}
		break;
	default:
		break;
	}
}

/* --- the screen ------------------------------------------------------------------------------- */

static void lobby_open(uin_view_t *v)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	cvar_t *skill = Cvar_Find("bot_skill");
	for (int i = 0; i < MAX_MAPS; i++)
		if (FS_FileExists(va("maps/%s.bsp", map_ids[i]))) {
			l->waypoints[l->nummaps] = FS_FileExists(va("data/%s.way", map_ids[i]));
			l->maps[l->nummaps++] = (uint8_t)i;
		}
	l->rs = l->mapi = -1;
	l->skill = skill ? BW_CLAMP(skill->ivalue, 1, 3) : 2;
	l->focus = ROW_START;
	l->bar_from = row_top(ROW_START);
	l->bar_moved = -1;
	l->next_poll = 0;
	if (!poll(v))
		return;
	/* the host starts on the first setting, a guest on READY */
	if (l->host && l->can_change) {
		l->focus = ROW_RULESET;
		l->bar_from = row_top(ROW_RULESET);
	}
}

static void lobby_frame(uin_view_t *v, double dt)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	(void)dt;
	if (!send_changes(v, false))
		return;
	if (uin_now() >= l->next_poll) {
		l->next_poll = uin_now() + POLL_S;
		if (!poll(v))
			return;
	}
	/* a row that went away under the focus (the game is starting) */
	if (!row_enabled(l, l->focus))
		set_focus(l, ROW_START, false);
}

/* text widths are screen pixels (faces are baked at screen size), the
 * layout is in the page frame: these convert through the frame's scale */
static float to_frame(float px) { return px / d_get_xf()->sx; }
static float to_px(float f) { return f * d_get_xf()->sx; }

/* a small solid triangle pointing left (dir -1) or right (dir 1) */
static void arrow(float x, float cy, int dir, uint32_t c)
{
	/* a quad with two corners on one point, wound as every other quad is */
	float right[8] = {x, cy - 4, x, cy - 4, x + 4.5f, cy, x, cy + 4};
	float left[8] = {x + 4.5f, cy - 4, x + 4.5f, cy - 4, x + 4.5f, cy + 4, x, cy};
	uint32_t cs[4] = {c, c, c, c};
	d_quad4(dir < 0 ? left : right, cs);
}

static void value_text(const lobby_t *l, int row, char *out, size_t size)
{
	switch (row) {
	case ROW_RULESET:
		Str_Copy(out, l->rs >= 0 ? rulesets[l->rs].label : l->ruleset, size);
		break;
	case ROW_MAP: {
		size_t n = 0;
		const char *m = l->mapi >= 0 ? map_ids[l->maps[l->mapi]] : l->map;
		for (; *m && n + 1 < size; m++)
			out[n++] = (char)toupper((unsigned char)*m);
		out[n] = 0;
		break;
	}
	case ROW_TIME:
		if (l->tl)
			snprintf(out, size, "%d MIN", l->tl);
		else
			Str_Copy(out, "NONE", size);
		break;
	case ROW_FRAGS:
		if (l->fl)
			snprintf(out, size, "%d", l->fl);
		else
			Str_Copy(out, "NONE", size);
		break;
	case ROW_BOTS:
		if (l->bots)
			snprintf(out, size, "%d", l->bots);
		else
			Str_Copy(out, "OFF", size);
		break;
	case ROW_SKILL:
		Str_Copy(out, skill_label[BW_CLAMP(l->skill, 1, 3) - 1], size);
		break;
	default:
		out[0] = 0;
	}
}

static const char *start_label(const lobby_t *l)
{
	if (l->host) {
		if (l->can_cancel)
			return "CANCEL";
		if (!strcmp(l->phase, "STARTING") || !strcmp(l->phase, "INGAME"))
			return "STARTING...";
		return "START GAME";
	}
	if (!strcmp(l->phase, "JOINING"))
		return "JOINING...";
	return local_ready(l) ? "NOT READY" : "READY";
}

static void draw_rows(const lobby_t *l, float a)
{
	bool locked = !(l->host && l->can_change);
	float by = bar_y(l);
	d_fill(MENU_X, by, MENU_W, l->focus == ROW_START ? START_H : ROW_H, uin_col_alpha(COL_BAR, a));
	for (int r = 0; r < ROW_START; r++) {
		float y = row_top(r) + ROW_H / 2;
		bool f = l->focus == r;
		uint32_t c = uin_col_alpha(f ? COL_FOCUS_TEXT : row_enabled(l, r) ? COL_TEXT : COL_DISABLED, a);
		char value[40];
		float vw;
		if (locked && r < ROW_START)
			continue; /* under the guest lock */
		d_text(UIN_F_STD8, MENU_X + 6, y, row_label[r], UIN_LEFT | UIN_MIDDLE, c);
		value_text(l, r, value, sizeof(value));
		vw = d_text(UIN_F_STD8, MENU_X + MENU_W - 22, y, value, UIN_RIGHT | UIN_MIDDLE, c);
		if (f) {
			arrow(MENU_X + MENU_W - 32 - to_frame(vw), y, -1, c);
			arrow(MENU_X + MENU_W - 15, y, 1, c);
		}
	}
	d_text(UIN_F_STD16, MENU_X + 6, row_top(ROW_START) + START_H / 2, start_label(l), UIN_LEFT | UIN_MIDDLE,
	       uin_col_alpha(l->focus == ROW_START ? COL_FOCUS_TEXT : COL_TEXT, a));
}

/* Figma guestLock: the settings rows covered for guests, the lock art
 * turned -28.42 deg and clipped to the panel (lobby-menu.css:187-203) */
static void draw_guest_lock(float a)
{
	float x = MENU_X - 3, y = MENU_Y, w = 522, h = ROW_START * ROW_H;
	d_fill(x, y, w, h, uin_col_alpha(0x3d3d3dffu, a));
	d_clip(x, y, w, h);
	d_img_rot(UIN_R_GUESTLOCK, x + 82.9f, y + 64.4f, 122, 155.4f, -28.42f, uin_col_alpha(0xffffffffu, a));
	d_unclip();
	d_text(UIN_F_STD8, x + w - 49, y + h / 2, "Lobby settings locked to Host.", UIN_RIGHT | UIN_MIDDLE,
	       uin_col_alpha(COL_TEXT, a));
}

static void draw_preview(const lobby_t *l, float a)
{
	char line[64], name[48], fitted[48];
	const char *rs = l->rs >= 0 ? rulesets[l->rs].label : l->ruleset;
	size_t n = 0;
	/* mapPreview: no levelshots ship, so the page's plain panel */
	d_fill(50, 66.75f, 522, 175.25f, UIN_HEXA(0x3e3e3e, 0.65f * a));
	d_text(UIN_F_INTER10, 56, 72.5f, rs[0] ? rs : "Waiting for settings", UIN_LEFT | UIN_TOP, UIN_HEXA(0xffffff, 0.65f * a));
	if (l->tl)
		snprintf(line, sizeof(line), "%d minute time limit", l->tl);
	else
		Str_Copy(line, "No time limit", sizeof(line));
	d_text(UIN_F_INTER10, 56, 87.5f, line, UIN_LEFT | UIN_TOP, UIN_HEXA(0xffffff, 0.65f * a));
	if (l->fl)
		snprintf(line, sizeof(line), "%d frag limit", l->fl);
	else
		Str_Copy(line, "No frag limit", sizeof(line));
	d_text(UIN_F_INTER10, 56, 102.5f, line, UIN_LEFT | UIN_TOP, UIN_HEXA(0xffffff, 0.65f * a));
	/* the map's name, large, bottom left (.map-title) */
	for (const char *m = l->mapi >= 0 ? map_ids[l->maps[l->mapi]] : l->map; *m && n + 1 < sizeof(name); m++)
		name[n++] = (char)toupper((unsigned char)*m);
	name[n] = 0;
	d_text(UIN_F_INTERB26, 57, 215.5f, d_text_fit(UIN_F_INTERB26, name, to_px(508), fitted, sizeof(fitted)), UIN_LEFT | UIN_MIDDLE,
	       UIN_HEXA(0xffffff, 0.65f * a));
}

static void draw_roster(const lobby_t *l, float a)
{
	char count[16], name[40];
	float y = ROSTER_Y + 38;
	const char *net = !strcmp(l->network, "offline") ? "OFFLINE" : !strcmp(l->network, "online") ? "ONLINE" : "SYSTEM LINK";
	/* rosterInfo: the kind of lobby, and players / seats */
	d_fill(ROSTER_X, ROSTER_Y, ROSTER_W, 37, UIN_HEXA(0x242424, 0.72f * a));
	d_text(UIN_F_STD8, ROSTER_X + 6, ROSTER_Y + 18.5f, net, UIN_LEFT | UIN_MIDDLE, uin_col_alpha(COL_TEXT, a));
	/* .player-count: the count, a faint slash and the seats, 1.62 px apart */
	{
		char num[8];
		float right = ROSTER_X + ROSTER_W - 6, cy = ROSTER_Y + 18.5f, gap = 4.74f;
		float wmax, wslash;
		snprintf(count, sizeof(count), "%d", l->max);
		snprintf(num, sizeof(num), "%d", l->numplayers);
		wmax = d_text_width(UIN_F_TG15, count, 1);
		wslash = d_text_width(UIN_F_TG15, "/", 1);
		d_text_ex(UIN_F_TG15, right, cy, count, UIN_RIGHT | UIN_MIDDLE, uin_col_alpha(COL_BLUE, 0.85f * a), 1, 0);
		d_text_ex(UIN_F_TG15, right - to_frame(wmax) - gap, cy, "/", UIN_RIGHT | UIN_MIDDLE, uin_col_alpha(COL_BLUE, 0.6f * a), 1, 0);
		d_text_ex(UIN_F_TG15, right - to_frame(wmax + wslash) - 2 * gap, cy, num, UIN_RIGHT | UIN_MIDDLE, uin_col_alpha(COL_SOFT, a), 1, 0);
	}
	d_fill(ROSTER_X, y, ROSTER_W, 326, UIN_HEXA(0x000000, 0.2f * a));
	for (int i = 0; i < l->numplayers && i < MAX_PLATES; i++) {
		char upper[32];
		size_t n = 0;
		float py = y + i * (PLATE_H + PLATE_GAP);
		for (const char *s = l->players[i].name; *s && n + 1 < sizeof(upper); s++)
			upper[n++] = (char)toupper((unsigned char)*s);
		upper[n] = 0;
		d_fill(ROSTER_X, py, ROSTER_W, PLATE_H, uin_col_alpha(0x515151ffu, a));
		if (l->players[i].host)
			d_fill(ROSTER_X, py, 6, PLATE_H, uin_col_alpha(UIN_COL_FOCUS, a));
		d_text(UIN_F_INTERB10, ROSTER_X + 14, py + PLATE_H / 2, d_text_fit(UIN_F_INTERB10, upper, to_px(190), name, sizeof(name)),
		       UIN_LEFT | UIN_MIDDLE, uin_col_alpha(l->players[i].connected ? COL_TEXT : COL_DISABLED, a));
		if (l->players[i].ready)
			d_text(UIN_F_STD8, ROSTER_X + ROSTER_W - 10, py + PLATE_H / 2, "READY", UIN_RIGHT | UIN_MIDDLE,
			       uin_col_alpha(COL_READY, a));
	}
	if (l->numplayers > MAX_PLATES) {
		snprintf(name, sizeof(name), "+%d MORE", l->numplayers - MAX_PLATES);
		d_text(UIN_F_STD8, ROSTER_X + ROSTER_W / 2, y + 326 - 10, name, UIN_CENTER | UIN_MIDDLE, uin_col_alpha(COL_BLUE, a));
	}
}

static void draw_chat(const lobby_t *l, float a)
{
	d_fill(54, CHAT_Y, 517, CHAT_H, UIN_HEXA(0x242424, 0.72f * a));
	for (int i = 0; i < l->numchat; i++) {
		char who[32], text[96];
		float y = CHAT_Y + 10 + 22 * i, w;
		snprintf(who, sizeof(who), "%s:", l->chat[i].name);
		w = d_text(UIN_F_INTERB10, 62, y, who, UIN_LEFT | UIN_TOP, uin_col_alpha(COL_BLUE, a));
		d_text(UIN_F_INTER10, 62 + to_frame(w) + 6, y, d_text_fit(UIN_F_INTER10, l->chat[i].text, to_px(494 - to_frame(w)), text, sizeof(text)),
		       UIN_LEFT | UIN_TOP, uin_col_alpha(COL_SOFT, a));
	}
}

static void draw_status(const lobby_t *l, float a)
{
	char line[128];
	if (l->error[0]) {
		d_text(UIN_F_INTER10, 54, 675, l->error, UIN_LEFT | UIN_TOP, uin_col_alpha(COL_ERROR, a));
		return;
	}
	if (!strcmp(l->phase, "COUNTDOWN") && l->remaining >= 0)
		snprintf(line, sizeof(line), "%s: %d", l->status, (int)(l->remaining + 0.99));
	else
		Str_Copy(line, l->status, sizeof(line));
	d_text(UIN_F_INTER10, 54, 675, line, UIN_LEFT | UIN_TOP, uin_col_alpha(COL_SOFT, a));
}

static void draw_leave(float a)
{
	/* the title's dialog look, in the middle of the frame */
	const float x = 330, y = 306, w = 300, h = 108;
	d_fill(0, 0, 960, 720, UIN_HEXA(0x000000, 0.5f * a));
	d_fill(x, y, w, h, UIN_HEXA(0x242424, 0.97f * a));
	d_fill(x, y, w, 2, uin_col_alpha(0x898989ffu, a));
	d_fill(x, y + h - 2, w, 2, uin_col_alpha(0x898989ffu, a));
	d_text(UIN_F_TG15, x + 15, y + 12, "LEAVE LOBBY?", UIN_LEFT | UIN_TOP, uin_col_alpha(COL_SOFT, a));
}

static void lobby_draw(uin_view_t *v, float x, float y, float w, float h)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	float a = v->fade;
	uin_xf_t xf;
	uin_xf_fit(&xf, UIN_FRAME_LOBBY_W, UIN_FRAME_LOBBY_H, x, y, w, h);
	d_xf(&xf);
	/* bgPanel: #3d3d3d at 0.7, faded in over its top 4.8% and out over its
	 * last 5% (lobby-menu.css .bg-panel) */
	d_grad_v(39, 14, 544, 33.3f, UIN_HEXA(0x3d3d3d, 0), UIN_HEXA(0x3d3d3d, 0.7f * a));
	d_fill(39, 47.3f, 544, 627, UIN_HEXA(0x3d3d3d, 0.7f * a));
	d_grad_v(39, 674.3f, 544, 34.7f, UIN_HEXA(0x3d3d3d, 0.7f * a), UIN_HEXA(0x3d3d3d, 0));
	if (l->have) {
		draw_preview(l, a);
		if (!(l->host && l->can_change))
			draw_guest_lock(a);
		draw_rows(l, a);
		draw_roster(l, a);
		draw_chat(l, a);
		draw_status(l, a);
	}
	if (l->leave) {
		float la = BW_CLAMP((float)((uin_now() - l->leave_opened) / UIN_FADE_S), 0.0f, 1.0f);
		draw_leave(la);
		for (int i = 0; i < 2; i++) {
			float ry = 306 + 44 + 24.0f * i;
			bool f = l->lfocus == i;
			if (f)
				d_fill(336, ry, 288, 22, uin_col_alpha(COL_BAR, la));
			d_text(UIN_F_STD8, 348, ry + 11, i ? "STAY" : "LEAVE", UIN_LEFT | UIN_MIDDLE,
			       uin_col_alpha(f ? COL_FOCUS_TEXT : COL_TEXT, la));
		}
	}
	d_xf(NULL);
	if (l->leave)
		uin_footer(w, h, "A=SELECT B=BACK");
	else if (l->host)
		uin_footer(w, h, l->focus == ROW_START ? "A=START B=LEAVE" : "A=CHANGE B=LEAVE");
	else
		uin_footer(w, h, local_ready(l) ? "A=NOT_READY B=LEAVE" : "A=READY B=LEAVE");
}

const uin_screen_t uin_lobby_screen = {"lobby", lobby_open, NULL, lobby_key, lobby_frame, lobby_draw};
