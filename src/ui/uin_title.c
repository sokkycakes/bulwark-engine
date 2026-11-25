/* Bulwark engine - native UI: the title (title-menu-v2), its Create and
 * Join dialogs and the quit confirmation (design 6.1).
 *
 * The look is the PC Panorama port's (data/panorama/{layout,styles,
 * scripts}/web/title-menu-v2.*), which draws the page's own 1024 x 768
 * artboard stretched over the view: a red wash, a #333 frame faded by the
 * page's radial mask, the tiled wordmark under the same mask, four rows
 * with a sliding grey bar, the copyright and the logo. The backdrop map
 * (webcore_menumap) is the engine's, drawn under all of it.
 *
 * The Xbox has no keyboard, so the dialogs differ from the page's: Create
 * offers system link (LAN) or offline, Join lists the lobbies found on the
 * network, and Quit asks first. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../common/json.h"

#include <math.h>
#include <stdio.h>

/* --- the artboard (title-menu-v2.css, % of 1024 x 768) --------------------------------- */

#define ROW_X 58.0f      /* .menu-item x 5.6641% */
#define ROW_W 399.0f     /* width 38.9648% */
#define ROW_H 24.0f      /* height 3.125% */
#define LABEL_X 81.46f   /* + Label x 5.8797% of the item */
#define FOCUS_S 0.1f     /* .focus-bar transition 0.1 s */
#define COL_BAR 0xbfbfbfffu
#define COL_ROW_TEXT 0xffffffffu
#define COL_ROW_FOCUS 0x404040ffu
#define COL_COPY 0x9d9d9dffu  /* .copy-text */
#define COL_RING 0x404040ffu  /* .copy-ring */

/* .item-0 .. .item-3 y: 68.8802%, 72.0052%, 75.1302%, 78.125% */
static const float row_y[] = {529.0f, 553.0f, 577.0f, 600.0f};

enum { ITEM_CREATE, ITEM_JOIN, ITEM_SETTINGS, ITEM_QUIT, NUM_ITEMS };
static const char *const item_label[NUM_ITEMS] = {"CREATE GAME", "JOIN SYSTEM LINK", "SETTINGS", "QUIT"};

/* --- dialogs (design 6.1, at 640 x 480) --------------------------------------------------- */

enum { DLG_NONE, DLG_CREATE, DLG_JOIN, DLG_QUIT };

#define MAX_HOSTS 8
#define HOST_ROW_H 15.0f

typedef struct {
	char addr[64];
	char name[40];
	char map[32];
	int players, max;
} host_t;

typedef struct {
	int focus;
	float bar_from;     /* the bar's y when it started moving */
	double bar_moved;   /* when it started */
	int dialog;
	int dfocus;
	double dialog_opened;
	int numhosts;
	bool searched;      /* a refresh has answered since Join opened */
	host_t hosts[MAX_HOSTS];
} title_t;

/* --- the backdrop's mask ------------------------------------------------------------------
 * title-menu-v2.js maskAt: the page's viewport-mask.svg, a rotated radial
 * gradient over the 1365 x 768 frame (which starts 171 px left of the
 * artboard), transparent in the middle and opaque at the edges. */

#define FRAME_W 1365.0f
#define FRAME_H 768.0f
#define FRAME_X0 -0.166992f /* .bg-frame x -16.6992% */

static float mask_at(float x, float y)
{
	static const float ct = 0.482244f, st = 0.876037f; /* cos, sin of 61.1842 deg */
	float px = x - 339.5f, py = y - 9;
	float u = (ct * px + st * py) / 745.857f, v = (-st * px + ct * py) / 1325.64f;
	float r = sqrtf(u * u + v * v);
	if (r <= 0.4375f)
		return 0;
	if (r <= 0.774038f)
		return 0.452632f * (r - 0.4375f) / (0.774038f - 0.4375f);
	if (r <= 1)
		return 0.452632f + (1 - 0.452632f) * (r - 0.774038f) / (1 - 0.774038f);
	return 1;
}

/* the #333 frame: one quad per cell of a grid over the visible artboard,
 * the mask at its corners (the port does 48 rows of 24-stop gradients) */
#define GRID_COLS 16
#define GRID_ROWS 12

static void draw_frame(float fade)
{
	static float grid[GRID_ROWS + 1][GRID_COLS + 1];
	static bool built;
	if (!built) {
		for (int j = 0; j <= GRID_ROWS; j++)
			for (int i = 0; i <= GRID_COLS; i++)
				grid[j][i] = mask_at(171 + 1024.0f * i / GRID_COLS, 768.0f * j / GRID_ROWS);
		built = true;
	}
	for (int j = 0; j < GRID_ROWS; j++)
		for (int i = 0; i < GRID_COLS; i++) {
			float x0 = 1024.0f * i / GRID_COLS, x1 = 1024.0f * (i + 1) / GRID_COLS;
			float y0 = 768.0f * j / GRID_ROWS, y1 = 768.0f * (j + 1) / GRID_ROWS;
			float xy[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
			uint32_t c[4] = {UIN_HEXA(0x333333, grid[j][i] * fade), UIN_HEXA(0x333333, grid[j][i + 1] * fade),
			                 UIN_HEXA(0x333333, grid[j + 1][i + 1] * fade), UIN_HEXA(0x333333, grid[j + 1][i] * fade)};
			if (!(c[0] & 255) && !(c[1] & 255) && !(c[2] & 255) && !(c[3] & 255))
				continue;
			d_quad4(xy, c);
		}
}

/* the tiled wordmark, as title-menu-v2.js buildPattern lays it out in view
 * pixels: tiles of 428.224 x 70.272 frame units with the 429 x 71 SVG kept
 * to its aspect inside, odd bands shifted, each tile cut in six slices
 * whose opacity is 0.11 x the mask at the slice's middle */
#define TILE_W 428.224f
#define TILE_H 70.272f
#define TILE_SLICES 6

static void draw_pattern(float vx, float vy, float vw, float vh, float fade)
{
	float fw = vw * FRAME_W / 1024, fh = vh, ox0 = vx + FRAME_X0 * vw;
	float tw = fw * TILE_W / FRAME_W, th = fh * TILE_H / FRAME_H;
	float sc = BW_MIN(tw / 429, th / 71), mw = 429 * sc, mh = 71 * sc;
	float mx = (tw - mw) / 2, my = (th - mh) / 2;
	float oddx = (TILE_W / FRAME_W / 2) * (fw - tw), oddy = (TILE_H / FRAME_H) * (fh - th);
	for (int b = 0; b * th < fh; b++) {
		float by0 = b * th, by1 = BW_MIN(fh, by0 + th);
		float ox = b % 2 ? oddx : 0, oy = b % 2 ? oddy : 0;
		for (int k = (int)floorf((by0 - oy) / th); k <= (int)floorf((by1 - oy) / th); k++) {
			float ty = oy + k * th;
			float y0 = BW_MAX(by0, ty + my), y1 = BW_MIN(by1, ty + my + mh);
			if (y1 - y0 < 0.5f)
				continue;
			for (float tx = ox - tw * ceilf(ox / tw); tx < fw; tx += tw)
				for (int s = 0; s < TILE_SLICES; s++) {
					float x0 = BW_MAX(0, tx + mx + mw * s / TILE_SLICES);
					float x1 = BW_MIN(fw, tx + mx + mw * (s + 1) / TILE_SLICES);
					float a;
					/* only what lands on the view */
					if (x1 - x0 < 0.5f || ox0 + x1 <= vx || ox0 + x0 >= vx + vw)
						continue;
					a = 0.11f * mask_at((x0 + x1) / 2 * FRAME_W / fw, (y0 + y1) / 2 * FRAME_H / fh);
					if (a < 0.004f)
						continue;
					d_img_uv(UIN_R_LOGOTILE, ox0 + x0, vy + y0, x1 - x0, y1 - y0, (x0 - tx - mx) / mw,
					         (y0 - ty - my) / mh, (x1 - tx - mx) / mw, (y1 - ty - my) / mh,
					         UIN_HEXA(0xffffff, a * fade));
				}
		}
	}
}

/* --- the rows ---------------------------------------------------------------------------- */

static float bar_y(const title_t *t)
{
	float k = uin_ease_focus((float)((uin_now() - t->bar_moved) / FOCUS_S));
	return t->bar_from + (row_y[t->focus] - t->bar_from) * k;
}

static void move_focus(title_t *t, int to)
{
	t->bar_from = bar_y(t);
	t->bar_moved = uin_now();
	t->focus = to;
	uin_sound(UIN_SND_MOVE);
}

/* a row of the page's look: label left, the grey bar under the focus */
static void draw_rows(const title_t *t, float fade)
{
	d_fill(ROW_X, bar_y(t), ROW_W, ROW_H, uin_col_alpha(COL_BAR, fade));
	for (int i = 0; i < NUM_ITEMS; i++)
		d_text(UIN_F_STD8, LABEL_X, row_y[i] + ROW_H / 2, item_label[i], UIN_LEFT | UIN_MIDDLE,
		       uin_col_alpha(i == t->focus && !t->dialog ? COL_ROW_FOCUS : COL_ROW_TEXT, fade));
}

/* the copyright with its #404040 ring (the port's eight offset copies),
 * then the logo, contained and left-aligned (title-menu-v2.css .tp-logo) */
static void draw_credits(const uin_xf_t *xf, float fade)
{
	static const char copy[] = "copyright 2025-2026 tomboy princess";
	float x = floorf(xf->ox + 65 * xf->sx + 0.5f), y = floorf(xf->oy + 643 * xf->sy + 0.5f);
	d_xf(NULL);
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
			if (dx || dy)
				d_text(UIN_F_HOOGE8, x + dx, y + dy, copy, UIN_LEFT | UIN_TOP, uin_col_alpha(COL_RING, fade));
	d_text(UIN_F_HOOGE8, x, y, copy, UIN_LEFT | UIN_TOP, uin_col_alpha(COL_COPY, fade));
	d_xf(xf);
	d_img(UIN_R_LOGO, 265, 643, 115, 42, uin_col_alpha(0xffffffffu, fade));
}

/* --- dialogs ----------------------------------------------------------------------------- */

static void open_dialog(uin_view_t *v, int dialog, int focus)
{
	title_t *t = UIN_STATE(v, title_t);
	t->dialog = dialog;
	t->dfocus = focus;
	t->dialog_opened = uin_now();
}

/* the lobbies a broadcast found (gethostcache), lobbies first: when any
 * answer is a lobby, plain servers are left out, as the page does */
static void read_hosts(uin_view_t *v)
{
	title_t *t = UIN_STATE(v, title_t);
	const char *r = uin_query(v, "gethostcache");
	json_t *j;
	bool any_lobby = false;
	if (!r || !(j = JSON_Parse(r, NULL, 0)))
		return;
	t->numhosts = 0;
	for (const json_t *e = j->child; e; e = e->next)
		any_lobby |= JSON_GetBool(e, "lobby", false);
	for (const json_t *e = j->child; e && t->numhosts < MAX_HOSTS; e = e->next) {
		host_t *h = &t->hosts[t->numhosts];
		const char *addr = JSON_GetString(e, "addr", "");
		const char *port = strrchr(addr, ':');
		bool dup = false;
		if (!*addr || (any_lobby && !JSON_GetBool(e, "lobby", false)))
			continue;
		/* a host on this machine answers both the broadcast and the
		 * loopback probe: one row for the pair (two consoles both called
		 * "player" on one map are both kept: neither is a loopback) */
		for (int i = 0; i < t->numhosts && !dup; i++) {
			const char *p = strrchr(t->hosts[i].addr, ':');
			dup = (!strncmp(addr, "127.", 4) || !strncmp(t->hosts[i].addr, "127.", 4)) &&
			      !strncmp(t->hosts[i].name, JSON_GetString(e, "name", addr), sizeof(t->hosts[i].name) - 1) &&
			      !strcmp(t->hosts[i].map, JSON_GetString(e, "map", "")) && !strcmp(p ? p : "", port ? port : "");
		}
		if (dup)
			continue;
		Str_Copy(h->addr, addr, sizeof(h->addr));
		Str_Copy(h->name, JSON_GetString(e, "name", addr), sizeof(h->name));
		Str_Copy(h->map, JSON_GetString(e, "map", ""), sizeof(h->map));
		h->players = (int)JSON_GetNumber(e, "players", 0);
		h->max = (int)JSON_GetNumber(e, "max", 0);
		t->numhosts++;
	}
	JSON_Free(j);
	t->searched = true;
	if (t->dfocus >= t->numhosts)
		t->dfocus = BW_MAX(0, t->numhosts - 1);
}

/* a broadcast and a short wait for answers (cl_lobby.c discover, 0.25 s) */
static void refresh_hosts(uin_view_t *v)
{
	int serial = v->serial;
	if (!uin_query(v, "hostcache_refresh") || !v->open || v->serial != serial)
		return;
	read_hosts(v);
}

/* a dialog's panel and title in the 640 x 480 frame */
static void draw_panel(float x, float y, float w, float h, const char *title, float a)
{
	d_fill(x, y, w, h, UIN_HEXA(0x2a2a2a, 0.92f * a));
	d_fill(x, y, w, 1, uin_col_alpha(0x666666ffu, a));
	d_fill(x, y + h - 1, w, 1, uin_col_alpha(0x666666ffu, a));
	d_fill(x, y + 1, 1, h - 2, uin_col_alpha(0x666666ffu, a));
	d_fill(x + w - 1, y + 1, 1, h - 2, uin_col_alpha(0x666666ffu, a));
	d_text(UIN_F_INTERB10, x + 10, y + 6, title, UIN_LEFT | UIN_TOP, uin_col_alpha(0xccccccffu, a));
	d_fill(x + 1, y + 21, w - 2, 1, uin_col_alpha(0x666666ffu, a));
}

/* a choice row: the title rows' grey bar when focused */
static void draw_choice(float x, float y, float w, const char *label, bool focused, float a)
{
	if (focused)
		d_fill(x, y, w, HOST_ROW_H, uin_col_alpha(COL_BAR, a));
	d_text(UIN_F_STD8, x + 8, y + HOST_ROW_H / 2, label, UIN_LEFT | UIN_MIDDLE,
	       uin_col_alpha(focused ? COL_ROW_FOCUS : COL_ROW_TEXT, a));
}

static void draw_join(const title_t *t, float a)
{
	const float x = 296, y = 190, w = 300, h = 160;
	draw_panel(x, y, w, h, "JOIN SYSTEM LINK", a);
	if (!t->numhosts) {
		d_text(UIN_F_STD8, x + w / 2, y + 80, t->searched ? "NO GAMES FOUND" : "SEARCHING...", UIN_CENTER | UIN_MIDDLE,
		       uin_col_alpha(0x888888ffu, a));
		return;
	}
	for (int i = 0; i < t->numhosts; i++) {
		const host_t *hs = &t->hosts[i];
		float ry = y + 26 + HOST_ROW_H * i;
		bool f = i == t->dfocus;
		uint32_t c = uin_col_alpha(f ? COL_ROW_FOCUS : COL_ROW_TEXT, a);
		char name[40], count[16];
		if (f)
			d_fill(x + 4, ry, w - 8, HOST_ROW_H, uin_col_alpha(COL_BAR, a));
		d_text(UIN_F_STD8, x + 10, ry + HOST_ROW_H / 2, d_text_fit(UIN_F_STD8, hs->name, 130, name, sizeof(name)),
		       UIN_LEFT | UIN_MIDDLE, c);
		d_text(UIN_F_STD8, x + 150, ry + HOST_ROW_H / 2, d_text_fit(UIN_F_STD8, hs->map, 90, name, sizeof(name)),
		       UIN_LEFT | UIN_MIDDLE, f ? c : uin_col_alpha(0x888888ffu, a));
		snprintf(count, sizeof(count), "%d/%d", hs->players, hs->max);
		d_text(UIN_F_STD8, x + w - 10, ry + HOST_ROW_H / 2, count, UIN_RIGHT | UIN_MIDDLE, c);
	}
}

static void draw_dialog(const title_t *t, float w, float h)
{
	uin_xf_t xf;
	float a = BW_CLAMP((float)((uin_now() - t->dialog_opened) / UIN_FADE_S), 0.0f, 1.0f);
	uin_xf_stretch(&xf, 640, 480, 0, 0, w, h);
	d_xf(&xf);
	/* .join-backdrop #0000008c */
	d_fill(0, 0, 640, 480, UIN_HEXA(0x000000, 0.55f * a));
	if (t->dialog == DLG_CREATE) {
		draw_panel(300, 250, 220, 72, "CREATE GAME", a);
		draw_choice(304, 28 + 250, 212, "SYSTEM LINK", t->dfocus == 0, a);
		draw_choice(304, 45 + 250, 212, "OFFLINE", t->dfocus == 1, a);
	} else if (t->dialog == DLG_QUIT) {
#ifdef BW_XBOX
		draw_panel(300, 250, 220, 72, "QUIT TO DASHBOARD?", a);
#else
		draw_panel(300, 250, 220, 72, "QUIT GAME?", a);
#endif
		draw_choice(304, 28 + 250, 212, "YES", t->dfocus == 0, a);
		draw_choice(304, 45 + 250, 212, "NO", t->dfocus == 1, a);
	} else if (t->dialog == DLG_JOIN) {
		draw_join(t, a);
	}
	d_xf(NULL);
}

static void dialog_key(uin_view_t *v, int action)
{
	title_t *t = UIN_STATE(v, title_t);
	int rows = t->dialog == DLG_JOIN ? t->numhosts : 2;
	if (uin_now() - t->dialog_opened < UIN_FADE_S)
		return; /* the page locks input while a dialog fades in */
	switch (action) {
	case UIN_A_UP:
	case UIN_A_DOWN:
		if (rows > 1) {
			t->dfocus = (t->dfocus + (action == UIN_A_UP ? rows - 1 : 1)) % rows;
			uin_sound(UIN_SND_MOVE);
		}
		break;
	case UIN_A_BACK:
		uin_sound(UIN_SND_BACK);
		t->dialog = DLG_NONE;
		break;
	case UIN_A_X:
		if (t->dialog == DLG_JOIN) {
			uin_sound(UIN_SND_MOVE);
			refresh_hosts(v);
		}
		break;
	case UIN_A_ACCEPT:
		if (t->dialog == DLG_CREATE) {
			uin_sound(UIN_SND_ACCEPT);
			t->dialog = DLG_NONE;
			/* trusted C: straight to the console, no page allow list */
			uin_cmd(t->dfocus == 0 ? "lobby_create_lan\nmenu_webcore_lobby\n" : "lobby_create_offline\nmenu_webcore_lobby\n");
		} else if (t->dialog == DLG_QUIT) {
			if (t->dfocus == 0) {
				uin_sound(UIN_SND_ACCEPT);
				uin_cmd("quit\n");
			} else {
				uin_sound(UIN_SND_BACK);
			}
			t->dialog = DLG_NONE;
		} else if (t->dialog == DLG_JOIN && t->dfocus < t->numhosts) {
			char req[96];
			const char *r;
			int serial = v->serial;
			snprintf(req, sizeof(req), "lobby_join:%s", t->hosts[t->dfocus].addr);
			r = uin_query(v, req);
			if (!r || !v->open || v->serial != serial)
				return;
			if (!strcmp(r, "ok")) {
				uin_sound(UIN_SND_ACCEPT);
				t->dialog = DLG_NONE;
				uin_cmd("menu_webcore_lobby\n");
			}
		}
		break;
	default:
		break;
	}
}

/* --- the screen ---------------------------------------------------------------------------- */

static void title_open(uin_view_t *v)
{
	title_t *t = UIN_STATE(v, title_t);
	const char *r;
	cvar_t *bots;
	t->bar_from = row_y[0];
	t->bar_moved = -1;
	/* the lobby's BOTS set bot_minClients for its game; the title's backdrop
	 * map would fill up with them too. Only when nothing is connected: that
	 * is when the router starts the backdrop, just after this open, and the
	 * title also flashes up while a lobby game's client reconnects to the
	 * new map (with its bots still wanted). */
	if ((r = uin_query(v, "getclient")) && strstr(r, "\"state\":0,") && (bots = Cvar_Find("bot_minClients")) &&
	    strcmp(bots->string, "-1"))
		Cvar_Set("bot_minClients", "-1");
	/* why the last game ended, once (design 6.5) */
	if ((r = uin_query(v, "takedisconnect")) && *r)
		uin_error_open("DISCONNECTED", r);
}

static void title_key(uin_view_t *v, int action, int key, bool repeat)
{
	title_t *t = UIN_STATE(v, title_t);
	(void)key, (void)repeat;
	if (t->dialog) {
		dialog_key(v, action);
		return;
	}
	switch (action) {
	case UIN_A_UP:
		move_focus(t, (t->focus + NUM_ITEMS - 1) % NUM_ITEMS);
		break;
	case UIN_A_DOWN:
		move_focus(t, (t->focus + 1) % NUM_ITEMS);
		break;
	case UIN_A_Y:
		uin_sound(UIN_SND_ACCEPT);
		uin_cmd("menu_options\n");
		break;
	case UIN_A_ACCEPT:
		uin_sound(UIN_SND_ACCEPT);
		if (t->focus == ITEM_CREATE) {
			open_dialog(v, DLG_CREATE, 0);
		} else if (t->focus == ITEM_JOIN) {
			int serial = v->serial;
			open_dialog(v, DLG_JOIN, 0);
			t->numhosts = 0;
			t->searched = false;
			refresh_hosts(v);
			if (!v->open || v->serial != serial)
				return;
		} else if (t->focus == ITEM_SETTINGS) {
			uin_cmd("menu_options\n");
		} else {
			open_dialog(v, DLG_QUIT, 1); /* NO first: a second A shouldn't quit */
		}
		break;
	default:
		break;
	}
}

static void title_draw(uin_view_t *v, float x, float y, float w, float h)
{
	title_t *t = UIN_STATE(v, title_t);
	uin_xf_t xf;
	float fade = v->fade;
	uin_xf_stretch(&xf, 1024, 768, x, y, w, h);
	d_xf(&xf);
	/* .bg-red: the page's mix-blend-mode: color layer composites as a plain
	 * wash (title-menu-v2.css:34-42), the full width */
	d_fill(0, 0, 1024, 768, UIN_HEXA(0x690002, 0.67f * fade));
	draw_frame(fade);
	d_xf(NULL);
	draw_pattern(x, y, w, h, fade);
	d_xf(&xf);
	draw_rows(t, fade);
	draw_credits(&xf, fade);
	d_xf(NULL);
	if (t->dialog)
		draw_dialog(t, w, h);
	if (t->dialog == DLG_JOIN)
		uin_footer(w, h, t->numhosts ? "A=JOIN X=REFRESH B=BACK" : "X=REFRESH B=BACK");
	else if (t->dialog)
		uin_footer(w, h, "A=SELECT B=BACK");
	else
		uin_footer(w, h, "A=SELECT Y=SETTINGS");
}

const uin_screen_t uin_title_screen = {"title", title_open, NULL, title_key, NULL, title_draw};
