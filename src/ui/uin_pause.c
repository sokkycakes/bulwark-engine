/* Bulwark engine - native UI: the in-game pause page (pause-menu, design
 * 6.3), drawn as Bulwark's Panorama port draws it
 * (data/panorama/{layout,styles,scripts}/web/pause-menu.*), whose geometry
 * is Stiletto's base/data/web/pause-menu/style.css in its 1280x720 frame,
 * stretched over the screen.
 *
 * The page's Host Menu, Change Team and Call a Vote rows have no target
 * (nothing registers menu_hostmenu, menu_changeteam or menu_callvote), so
 * they give way to rows that work: Change Hero, and Vote Yes / Vote No
 * while a vote runs. Leave Game asks first. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../client/keys.h"
#include "../common/json.h"

#include <ctype.h>

/* --- geometry: style.css, 1280x720 frame ------------------------------------------------- */

#define P_PANEL_X 149 /* .panel left, top, width, height */
#define P_PANEL_Y 53
#define P_PANEL_W 1131
#define P_PANEL_H 666
#define P_BANNER_W 146 /* .banner: left 0, top 53, 146 x 667 */
#define P_BANNER_H 667
#define P_ROW_X 155 /* .menu-item and .focus-bar: left 155, 452 x 32 */
#define P_ROW_W 452
#define P_ROW_H 32
#define P_LABEL_X 9 /* .menu-item padding-left 9, padding-top 4 */
/* padding-top 4, plus the half-leading of the label's line box that the
 * page's glyphs sit under (3 screen px at 480 lines, measured against the
 * Panorama build) */
#define P_LABEL_Y 8.5f
/* .menu-item:nth-child(n) top */
static const float row_y[] = {67, 98, 130, 162, 193, 225};
#define MAX_ROWS ((int)BW_ARRAY_COUNT(row_y))

/* the focus bar slides to its row over 120 ms (.focus-bar transition) */
#define FOCUS_SLIDE_S 0.12
/* getclient is read twice a second: enough for a vote appearing */
#define CLIENT_POLL_S 0.5

enum { ITEM_RETURN, ITEM_HERO, ITEM_VOTE_YES, ITEM_VOTE_NO, ITEM_SETTINGS, ITEM_LEAVE };
static const char *const item_label[] = {"RETURN TO GAME", "CHANGE HERO", "VOTE YES", "VOTE NO", "SETTINGS",
                                         "LEAVE GAME"};

typedef struct {
	int items[MAX_ROWS], nitems;
	int focus;            /* index into items */
	float bar_from, bar_to;
	double bar_moved;     /* uin_now() when the bar left bar_from */
	double next_poll;
	bool polled;
	char mode[32];        /* serverinfo mode: deathmatch, duel, lastmanstanding, ... */
	bool voting;
	bool join;            /* no team yet in team rules: the hero row joins one */
	bool no_heroes;       /* the rules name teams but let them play no hero */
	char votes_y[8], votes_n[8];
	bool confirm;         /* the Leave Game question is up */
	int confirm_focus;    /* 0 LEAVE, 1 STAY */
} pause_t;

/* --- data ----------------------------------------------------------------------------- */

static void rebuild_items(pause_t *p)
{
	int keep = p->nitems ? p->items[p->focus] : ITEM_RETURN;
	p->nitems = 0;
	p->items[p->nitems++] = ITEM_RETURN;
	/* lastmanstanding's rules take no hero command (no ClientCommand
	 * handler), and domination registers no roster, so the picker would
	 * do nothing there; it stays while there is a team to join */
	if (p->join || (Str_ICmp(p->mode, "lastmanstanding") && !p->no_heroes))
		p->items[p->nitems++] = ITEM_HERO;
	if (p->voting) {
		p->items[p->nitems++] = ITEM_VOTE_YES;
		p->items[p->nitems++] = ITEM_VOTE_NO;
	}
	p->items[p->nitems++] = ITEM_SETTINGS;
	p->items[p->nitems++] = ITEM_LEAVE;
	/* the focus stays on its item when rows come and go */
	p->focus = 0;
	for (int i = 0; i < p->nitems; i++)
		if (p->items[i] == keep)
			p->focus = i;
	p->bar_to = row_y[p->focus];
	if (!p->polled)
		p->bar_from = p->bar_to;
}

/* returns false when the query closed v */
static bool poll_client(uin_view_t *v, pause_t *p)
{
	const char *reply = uin_query(v, "getclient");
	json_t *j;
	if (!reply)
		return false;
	p->next_poll = uin_now() + CLIENT_POLL_S;
	if ((j = JSON_Parse(reply, NULL, 0))) {
		const char *vote = JSON_GetString(j, "vote", "");
		Str_Copy(p->mode, JSON_GetString(j, "mode", ""), sizeof(p->mode));
		json_t *teams = JSON_Get(j, "teams");
		int nteams = teams && teams->type == JSON_ARRAY ? teams->count : 0;
		p->voting = vote[0] != 0;
		p->join = JSON_GetNumber(j, "team", 0) == 0 && nteams >= 2;
		p->no_heroes = nteams > 0 && !JSON_GetString(j, "roster", "")[0];
		Str_Copy(p->votes_y, JSON_GetString(j, "votesY", ""), sizeof(p->votes_y));
		Str_Copy(p->votes_n, JSON_GetString(j, "votesN", ""), sizeof(p->votes_n));
		JSON_Free(j);
	}
	rebuild_items(p);
	p->polled = true;
	return true;
}

/* --- keys ----------------------------------------------------------------------------- */

static void move_focus(pause_t *p, int to)
{
	float y = row_y[p->focus];
	float t = (float)((uin_now() - p->bar_moved) / FOCUS_SLIDE_S);
	/* from wherever the bar is now, so a quick second press doesn't jump */
	if (t < 1)
		y = p->bar_from + (p->bar_to - p->bar_from) * uin_ease_focus(t);
	p->focus = to;
	p->bar_from = y;
	p->bar_to = row_y[to];
	p->bar_moved = uin_now();
	uin_sound(UIN_SND_MOVE);
}

static void activate(pause_t *p)
{
	uin_sound(UIN_SND_ACCEPT);
	switch (p->items[p->focus]) {
	case ITEM_RETURN:
		uin_cmd("webcore_closemenu\n");
		break;
	case ITEM_HERO:
		/* the router opens the picker in place of this page (open_arena) */
		uin_cmd("menu_webcore arena class\n");
		break;
	case ITEM_VOTE_YES:
	case ITEM_VOTE_NO:
		/* the vote line is the client game's; back to the game to see it */
		uin_cmd(p->items[p->focus] == ITEM_VOTE_YES ? "vote yes\nwebcore_closemenu\n" : "vote no\nwebcore_closemenu\n");
		break;
	case ITEM_SETTINGS:
		/* the engine's options menu draws over this page and goes back to it */
		uin_cmd("menu_options\n");
		break;
	case ITEM_LEAVE:
		p->confirm = true;
		p->confirm_focus = 1; /* STAY: a stray A doesn't end the game */
		break;
	}
}

static void pause_key(uin_view_t *v, int action, int key, bool repeat)
{
	pause_t *p = UIN_STATE(v, pause_t);
	/* Esc in a game is closed by the router after it reaches us
	 * (WC_KeyEvent): closing here too would close twice */
	if (key == K_ESCAPE)
		return;
	if (p->confirm) {
		switch (action) {
		case UIN_A_UP: case UIN_A_DOWN: case UIN_A_LEFT: case UIN_A_RIGHT:
			p->confirm_focus ^= 1;
			uin_sound(UIN_SND_MOVE);
			break;
		case UIN_A_ACCEPT:
			if (p->confirm_focus == 0) {
				uin_sound(UIN_SND_ACCEPT);
				/* pause-menu/index.html's Leave Game, in its order */
				uin_cmd("lobby_close\ndisconnect\nwebcore_closemenu\n");
			} else {
				uin_sound(UIN_SND_BACK);
			}
			p->confirm = false;
			break;
		case UIN_A_BACK:
		case UIN_A_START:
			uin_sound(UIN_SND_BACK);
			p->confirm = false;
			break;
		}
		return;
	}
	switch (action) {
	case UIN_A_UP:
		/* six rows at most: the list wraps (design 6.0) */
		move_focus(p, (p->focus + p->nitems - 1) % p->nitems);
		break;
	case UIN_A_DOWN:
		move_focus(p, (p->focus + 1) % p->nitems);
		break;
	case UIN_A_ACCEPT:
		if (!repeat)
			activate(p);
		break;
	case UIN_A_BACK:
	case UIN_A_START:
		uin_sound(UIN_SND_BACK);
		uin_cmd("webcore_closemenu\n");
		break;
	}
}

/* --- frame and drawing ------------------------------------------------------------------ */

static void pause_open(uin_view_t *v)
{
	pause_t *p = UIN_STATE(v, pause_t);
	rebuild_items(p);
	poll_client(v, p);
}

static void pause_frame(uin_view_t *v, double dt)
{
	pause_t *p = UIN_STATE(v, pause_t);
	(void)dt;
	if (uin_now() >= p->next_poll)
		poll_client(v, p);
}

/* the focus bar of .focus-bar: #b70043 to 32%, then fading out, with
 * 2 px (at 720 lines) edges of #ff8c8c the same way */
static void draw_focus_bar(float x, float y, float w, float h, float edge, float fade)
{
	float solid = w * 0.32f;
	d_fill(x, y, solid, h, uin_col_alpha(0xb70043ffu, fade));
	d_grad_h(x + solid, y, w - solid, h, uin_col_alpha(0xb70043ffu, fade), 0xb7004300u);
	for (int i = 0; i < 2; i++) {
		float ey = i ? y + h - edge : y;
		d_fill(x, ey, solid, edge, uin_col_alpha(0xff8c8cffu, fade));
		d_grad_h(x + solid, ey, w - solid, edge, uin_col_alpha(0xff8c8cffu, fade), 0xff8c8c00u);
	}
}

static void draw_confirm(pause_t *p, float fade)
{
	/* a small box over the panel, in the error dialog's style (design 6.5) */
	static const char *const choice[] = {"LEAVE", "STAY"};
	const float bx = 440, by = 250, bw = 400, bh = 190;
	d_fill(bx, by, bw, bh, uin_col_alpha(UIN_HEXA(0x1a1a1a, 0.92f), fade));
	d_fill(bx, by, bw, 2, uin_col_alpha(UIN_COL_ACCENT, fade));
	d_text(UIN_F_TG15, bx + 24, by + 22, "LEAVE GAME?", UIN_LEFT | UIN_TOP, uin_col_alpha(0xffffffffu, fade));
	d_text(UIN_F_INTER10, bx + 24, by + 58, "You will leave the match.", UIN_LEFT | UIN_TOP,
	       uin_col_alpha(0xd0d0d0ffu, fade));
	for (int i = 0; i < 2; i++) {
		float ry = by + 96 + i * 40;
		if (p->confirm_focus == i)
			draw_focus_bar(bx + 16, ry, bw - 32, P_ROW_H, 2, fade);
		d_text_ex(UIN_F_TG15, bx + 16 + P_LABEL_X, ry + P_LABEL_Y, choice[i], UIN_LEFT | UIN_TOP,
		          uin_col_alpha(0xffffffffu, fade), 0.1f, 0);
	}
}

static void pause_draw(uin_view_t *v, float x, float y, float w, float h)
{
	pause_t *p = UIN_STATE(v, pause_t);
	uin_xf_t xf;
	float fade = v->fade, edge, bar_y, t;
	uin_xf_stretch(&xf, UIN_FRAME_PAUSE_W, UIN_FRAME_PAUSE_H, x, y, w, h);
	d_xf(&xf);
	/* one screen pixel tall, whatever the scale (max(1px, 100vh / 720)) */
	edge = 1 / xf.sy;

	/* .panel__base: #272727c9 to #494949c9 at 90%, then flat; 1 px edges */
	d_grad_v(P_PANEL_X, P_PANEL_Y, P_PANEL_W, P_PANEL_H * 0.9f, uin_col_alpha(0x272727c9u, fade),
	         uin_col_alpha(0x494949c9u, fade));
	d_fill(P_PANEL_X, P_PANEL_Y + P_PANEL_H * 0.9f, P_PANEL_W, P_PANEL_H * 0.1f, uin_col_alpha(0x494949c9u, fade));
	d_fill(P_PANEL_X, P_PANEL_Y, P_PANEL_W, edge, uin_col_alpha(0xc9c9c9ffu, fade));
	d_fill(P_PANEL_X, P_PANEL_Y + P_PANEL_H - edge, P_PANEL_W, edge, uin_col_alpha(0xc9c9c9ffu, fade));

	/* .banner and its label: 48 px at 720 lines, letter-spacing 0.2em,
	 * turned -90 degrees about the banner's centre */
	d_fill(0, P_PANEL_Y, P_BANNER_W, P_BANNER_H, uin_col_alpha(0x1a1a1affu, fade));
	{
		const uin_face_t *f = uin_face(UIN_F_PAUSE);
		float track = f ? f->px * 0.2f : 6, cx = P_BANNER_W * 0.5f;
		float sx, sy, sw, sh, half;
		/* The page centres it in the banner, which on a 640-wide TV puts its
		 * ink across the action-safe edge (PC monitors don't overscan). The
		 * band stays at the screen edge; the label moves in to clear
		 * ui_native_safe, staying in the band. Glyphs aren't scaled by the
		 * frame, so its half-height is in screen pixels. */
		uin_safe_rect(&sx, &sy, &sw, &sh);
		half = f ? (f->ascent + f->descent) * 0.5f : 12;
		cx = BW_MAX(cx, (sx + half - xf.ox) / xf.sx);
		cx = BW_MIN(cx, P_BANNER_W - half / xf.sx);
		d_text_ex(UIN_F_PAUSE, cx, P_PANEL_Y + P_BANNER_H * 0.5f, "PAUSE", UIN_CENTER | UIN_MIDDLE,
		          uin_col_alpha(UIN_COL_ACCENT, fade), track, -90);
	}

	/* the focus bar, eased to its row */
	t = (float)((uin_now() - p->bar_moved) / FOCUS_SLIDE_S);
	bar_y = t >= 1 ? p->bar_to : p->bar_from + (p->bar_to - p->bar_from) * uin_ease_focus(t);
	if (!p->confirm)
		draw_focus_bar(P_ROW_X, bar_y, P_ROW_W, P_ROW_H, 2, fade);

	/* .menu-item labels: 23 px at 720 lines, bold, letter-spacing -0.5px;
	 * drawn 0.1 px apart, which matches the width the Panorama port draws
	 * (its bold over the bold face comes out wider than -0.5 px) */
	for (int i = 0; i < p->nitems; i++) {
		const char *label = p->items[i] == ITEM_HERO && p->join ? "JOIN A TEAM" : item_label[p->items[i]];
		char buf[48];
		if (p->items[i] == ITEM_VOTE_YES || p->items[i] == ITEM_VOTE_NO) {
			/* the count as the client game's vote line shows it */
			const char *n = p->items[i] == ITEM_VOTE_YES ? p->votes_y : p->votes_n;
			if (n[0]) {
				Str_Copy(buf, label, sizeof(buf));
				Str_Cat(buf, " (", sizeof(buf));
				Str_Cat(buf, n, sizeof(buf));
				Str_Cat(buf, ")", sizeof(buf));
				label = buf;
			}
		}
		d_text_ex(UIN_F_TG15, P_ROW_X + P_LABEL_X, row_y[i] + P_LABEL_Y, label, UIN_LEFT | UIN_TOP,
		          uin_col_alpha(p->confirm ? 0x808080ffu : 0xffffffffu, fade), 0.1f, 0);
	}
	if (p->confirm)
		draw_confirm(p, fade);
	d_xf(NULL);
	uin_footer(w, h, p->confirm ? "A=SELECT B=BACK" : "A=SELECT B=RETURN");
}

const uin_screen_t uin_pause_screen = {"pause", pause_open, NULL, pause_key, pause_frame, pause_draw};
