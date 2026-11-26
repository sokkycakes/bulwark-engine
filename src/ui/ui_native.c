/* Bulwark engine - the native UI: a ui.h backend that draws Stiletto's
 * WebCore pages (title, lobby, pause, hero picker, HUD) in C, for builds
 * without PanoramaEngine. cl_webcore.c keeps all the routing: it opens and
 * closes layouts by name as on the PC, and this file binds each layout to
 * a screen (uin_local.h), feeds it keys and frames, and draws it.
 * uin_local.h describes the parts. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../client/client.h" /* Key_IsDown, EM_Active; ui_key: Key_NumForName, IN_KeyEvent */
#include "../client/console.h"
#include "../client/keys.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_VIEWS 8

cvar_t *ui_native_safe, *ui_hud_style, *ui_hud_debug, *ui_native_hudtest, *ui_native_stats;
static cvar_t *cl_loadstage;

static uin_view_t views[MAX_VIEWS];
static int next_serial = 1;
static ui_query_fn query_fn;
static strbuf_t reply;
static double now;
static float hud_w = 640, hud_h = 480; /* the last UI_DrawAt size, for UI_ViewSize */
static char shot_pending[128];
static int show_atlas; /* ui_native_atlas: 1 page 0, 2 page 1 */

/* a held direction repeats after 400 ms, then every 100 ms (design 7) */
static struct {
	int key, action;
	double next;
} held;

/* ui_native_stats: microseconds per call, over 2 s */
static struct {
	double hud_sum, hud_max, draw_sum, draw_max, since;
	int hud_n, draw_n, verts_max, calls_max;
} stats;

/* --- layouts -------------------------------------------------------------------------- */

/* the layouts cl_webcore.c maps Stiletto's pages to (page_layout), and the
 * screen each one is. No file check: the .xml files needn't ship. */
static const struct {
	const char *layout;
	int kind;
} layouts[] = {
	{"layout/web/title-menu-v2.xml", UIN_KIND_TITLE},
	{"layout/web/title-menu.xml", UIN_KIND_TITLE}, /* webcore_menu_url's default: shown as v2 */
	{"layout/web/lobby-menu.xml", UIN_KIND_LOBBY},
	{"layout/web/pause-menu.xml", UIN_KIND_PAUSE},
	{"layout/web/ql-menu.xml", UIN_KIND_HERO},
	{"layout/web/hud.xml", UIN_KIND_HUD},
};

static const uin_screen_t *const screens[UIN_NUM_KINDS] = {
	[UIN_KIND_TITLE] = &uin_title_screen, [UIN_KIND_LOBBY] = &uin_lobby_screen,
	[UIN_KIND_PAUSE] = &uin_pause_screen, [UIN_KIND_HERO] = &uin_hero_screen,
	[UIN_KIND_HUD] = &uin_hud_screen,
};

static int layout_kind(const char *layout)
{
	if (!layout)
		return UIN_KIND_NONE;
	for (size_t i = 0; i < BW_ARRAY_COUNT(layouts); i++)
		if (!Str_ICmp(layouts[i].layout, layout))
			return layouts[i].kind;
	return UIN_KIND_NONE;
}

/* open_page (cl_webcore.c) hands the page its location as JavaScript:
 * var location = {href: "...", search: "?mode=arena"}; ... -- the search
 * string is the one thing a screen needs from it */
static void parse_search(const char *js, char *out, size_t outsize)
{
	const char *p = js ? strstr(js, "search: \"") : NULL;
	size_t n = 0;
	out[0] = 0;
	if (!p)
		return;
	for (p += 9; *p && *p != '"' && n + 1 < outsize; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'u') { /* a control character: never part of a query */
				for (int i = 0; i < 4 && p[1]; i++)
					p++;
				continue;
			}
		}
		out[n++] = *p;
	}
	out[n] = 0;
}

static uin_view_t *view_at(int view)
{
	return view >= 1 && view <= MAX_VIEWS && views[view - 1].open ? &views[view - 1] : NULL;
}

/* the input view opened last: it takes the keys */
static uin_view_t *top_input(void)
{
	uin_view_t *top = NULL;
	for (int i = 0; i < MAX_VIEWS; i++)
		if (views[i].open && (views[i].flags & UI_INPUT) && (!top || views[i].serial > top->serial))
			top = &views[i];
	return top;
}

/* open views in the order they were opened */
static int open_order(uin_view_t **out)
{
	int n = 0;
	for (int i = 0; i < MAX_VIEWS; i++)
		if (views[i].open)
			out[n++] = &views[i];
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && out[j - 1]->serial > out[j]->serial; j--) {
			uin_view_t *t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

/* --- services for screens ---------------------------------------------------------- */

double uin_now(void) { return now; }

bool uin_kind_open(int kind)
{
	for (int i = 0; i < MAX_VIEWS; i++)
		if (views[i].open && views[i].kind == kind)
			return true;
	return false;
}

const char *uin_query(uin_view_t *v, const char *request)
{
	int serial = v ? v->serial : 0;
	int num = v ? (int)(v - views) + 1 : 0;
	reply.len = 0;
	if (reply.data)
		reply.data[0] = 0;
	if (query_fn)
		query_fn(num, request, &reply);
	if (v && (!v->open || v->serial != serial))
		return NULL; /* the request closed the view (arena_action:close) */
	return reply.data ? reply.data : "";
}

void uin_cmd(const char *text) { Cbuf_AddText(text); }

void uin_sound(int which)
{
	char req[32];
	snprintf(req, sizeof(req), "localsound:misc/menu%d", which);
	uin_query(NULL, req);
}

bool uin_search_get(const uin_view_t *v, const char *key, char *out, size_t outsize)
{
	size_t n = strlen(key);
	for (const char *p = v->search; *p;) {
		if (*p == '?' || *p == '&') {
			p++;
			continue;
		}
		const char *e = p;
		while (*e && *e != '&')
			e++;
		if ((size_t)(e - p) > n && !strncmp(p, key, n) && p[n] == '=') {
			size_t len = (size_t)(e - p) - n - 1;
			if (len >= outsize)
				len = outsize - 1;
			memcpy(out, p + n + 1, len);
			out[len] = 0;
			return true;
		}
		p = e;
	}
	return false;
}

void uin_safe_rect(float *x, float *y, float *w, float *h)
{
	int sw, sh;
	float f = ui_native_safe ? BW_CLAMP(ui_native_safe->value, 0.0f, 0.25f) : 0;
	R_GetSize(&sw, &sh);
	*x = sw * f;
	*y = sh * f;
	*w = sw - 2 * *x;
	*h = sh - 2 * *y;
}

/* cubic-bezier(.2,.8,.2,1): x(u) solved by bisection, then y(u) */
float uin_ease_focus(float t)
{
	float lo = 0, hi = 1, u = t;
	if (t <= 0)
		return 0;
	if (t >= 1)
		return 1;
	for (int i = 0; i < 20; i++) {
		float x;
		u = (lo + hi) * 0.5f;
		x = 3 * (1 - u) * (1 - u) * u * 0.2f + 3 * (1 - u) * u * u * 0.2f + u * u * u;
		if (x < t)
			lo = u;
		else
			hi = u;
	}
	return 3 * (1 - u) * (1 - u) * u * 0.8f + 3 * (1 - u) * u * u * 1.0f + u * u * u;
}

void uin_debug_box(float x, float y, float w, float h, const char *label, uint32_t rgba)
{
	d_fill(x, y, w, 1, rgba);
	d_fill(x, y + h - 1, w, 1, rgba);
	d_fill(x, y + 1, 1, h - 2, rgba);
	d_fill(x + w - 1, y + 1, 1, h - 2, rgba);
	if (label && *label)
		d_text(UIN_F_STD8, x + 4, y + 4, label, UIN_LEFT | UIN_TOP, rgba);
}

/* --- keys ---------------------------------------------------------------------------- */

static int key_action(int key)
{
	switch (key) {
	case K_GP_DPAD_UP: case K_UPARROW: case K_KP_UPARROW: case 'w': case K_MWHEELUP:
		return UIN_A_UP;
	case K_GP_DPAD_DOWN: case K_DOWNARROW: case K_KP_DOWNARROW: case 's': case K_MWHEELDOWN:
		return UIN_A_DOWN;
	case K_GP_DPAD_LEFT: case K_LEFTARROW: case K_KP_LEFTARROW:
		return UIN_A_LEFT;
	case K_GP_DPAD_RIGHT: case K_RIGHTARROW: case K_KP_RIGHTARROW:
		return UIN_A_RIGHT;
	case K_GP_A: case K_ENTER: case K_KP_ENTER: case K_SPACE:
		return UIN_A_ACCEPT;
	case K_GP_B: case K_ESCAPE: case K_BACKSPACE:
		return UIN_A_BACK;
	case K_GP_X:
		return UIN_A_X;
	case K_GP_Y:
		return UIN_A_Y;
	case K_GP_MENU:
		return UIN_A_START;
	default:
		return UIN_A_NONE;
	}
}

static bool repeats(int action)
{
	return action == UIN_A_UP || action == UIN_A_DOWN || action == UIN_A_LEFT || action == UIN_A_RIGHT;
}

static void dispatch(int action, int key, bool repeat)
{
	uin_view_t *v;
	if (uin_error_active()) {
		uin_error_key(action, key);
		return;
	}
	v = top_input();
	if (!v || v->fade < 1 || !v->screen->key)
		return; /* input waits for the fade-in, as the pages lock it */
	v->screen->key(v, action, key, repeat);
}

/* While a menu is open every key is taken, down and up. A key the menu
 * doesn't use must not fall through to the binds: Back is +showscores and
 * B is +duck in pad_defaults (cl_input.c). Releases of held binds still
 * reach them: IN_KeyEvent skips the UI for those. Back+Start (the
 * console) is caught in vid_xbox.c before any of this. */
bool UI_KeyEvent(int key, bool down, int unicode)
{
	int action;
	(void)unicode;
	if (!top_input())
		return false;
	action = key_action(key);
	if (!down) {
		if (held.key == key)
			held.key = 0;
		return true;
	}
	if (held.key == key)
		return true; /* the system's own key repeat: ours paces it */
	if (repeats(action)) {
		held.key = key;
		held.action = action;
		held.next = now + 0.4;
	}
	dispatch(action, key, false);
	return true;
}

bool UI_WantsInput(void) { return top_input() != NULL; }
void UI_MouseMove(float x, float y) { (void)x, (void)y; }

/* --- views ----------------------------------------------------------------------------- */

bool UI_LayoutExists(const char *layout) { return layout_kind(layout) != UIN_KIND_NONE; }

int UI_Open(const char *layout, int flags) { return UI_OpenEx(layout, flags, NULL); }

int UI_OpenEx(const char *layout, int flags, const char *bootstrap)
{
	int kind = layout_kind(layout);
	uin_view_t *v = NULL;
	if (kind == UIN_KIND_NONE) {
		Con_DPrintf("ui: %s has no native screen\n", layout ? layout : "(null)");
		return 0;
	}
	for (int i = 0; i < MAX_VIEWS && !v; i++)
		if (!views[i].open)
			v = &views[i];
	if (!v) {
		Con_Printf("ui: no free view for %s\n", layout);
		return 0;
	}
	memset(v, 0, sizeof(*v));
	v->open = true;
	v->kind = kind;
	v->flags = flags;
	v->serial = next_serial++;
	v->screen = screens[kind];
	v->opened = now;
	v->rgb[0] = v->rgb[1] = v->rgb[2] = 1;
	v->alpha = 1;
	Str_Copy(v->layout, layout, sizeof(v->layout));
	parse_search(bootstrap, v->search, sizeof(v->search));
	/* a direction held into a new menu doesn't repeat in it */
	held.key = 0;
	if (v->screen->open) {
		int serial = v->serial;
		v->screen->open(v);
		if (!v->open || v->serial != serial)
			return 0; /* it closed itself while opening */
	}
	return (int)(v - views) + 1;
}

void UI_Close(int view)
{
	uin_view_t *v = view_at(view);
	if (!v)
		return;
	/* closed first, so a close callback that queries sees it gone */
	v->open = false;
	if (v->screen->close)
		v->screen->close(v);
	if (!top_input())
		held.key = 0;
}

void UI_CloseAll(void)
{
	for (int i = 0; i < MAX_VIEWS; i++)
		if (views[i].open)
			UI_Close(i + 1);
}

bool UI_IsOpen(int view) { return view_at(view) != NULL; }

void UI_SetFlags(int view, int flags)
{
	uin_view_t *v = view_at(view);
	if (v)
		v->flags = flags;
}

int UI_Flags(int view)
{
	uin_view_t *v = view_at(view);
	return v ? v->flags : 0;
}

void UI_ViewSize(int view, float *w, float *h)
{
	(void)view;
	*w = hud_w;
	*h = hud_h;
}

/* Stiletto's progs make no ui_* builtin calls and its pages' scripts are
 * gone: nothing posts, evaluates or acts */
void UI_SetActionHandler(ui_action_fn fn) { (void)fn; }
void UI_SetQueryHandler(ui_query_fn fn) { query_fn = fn; }
void UI_Post(int view, const char *event, const char *json) { (void)view, (void)event, (void)json; }
void UI_Eval(int view, const char *js) { (void)view, (void)js; }

bool UI_AddFont(const char *family, const char *path, int weight)
{
	if (uin_font_add(family, path, weight)) {
		uin_atlas_invalidate();
		return true;
	}
	return false;
}

/* --- frames and drawing ---------------------------------------------------------- */

void UI_Frame(double dt)
{
	uin_view_t *order[MAX_VIEWS];
	int serials[MAX_VIEWS];
	int n;
	if (dt > 0 && dt < 1)
		now += dt;
	else if (dt >= 1)
		now += 0.1; /* a hitch (a map load) mustn't skip a whole fade */
	/* The release of a held direction can go elsewhere: IN_KeyEvent hands
	 * keys to the console and the engine's menus (EM swallows every
	 * key-up) before the UI. So the repeat also ends when the key is no
	 * longer down, or when another key layer sits above us; otherwise it
	 * would run on under that layer and after it closes. */
	if (held.key && (!Key_IsDown(held.key) || Con_IsActive() || EM_Active()))
		held.key = 0;
	if (held.key && now >= held.next) {
		held.next = now + 0.1;
		dispatch(held.action, held.key, true);
	}
	n = open_order(order);
	/* serials taken before any frame runs: a slot that an earlier view's
	 * frame closed and reopened holds a new view, which waits for the
	 * next pass */
	for (int i = 0; i < n; i++)
		serials[i] = order[i]->serial;
	for (int i = 0; i < n; i++) {
		uin_view_t *v = order[i];
		if (!v->open || v->serial != serials[i])
			continue; /* closed by an earlier view's frame */
		v->fade = BW_MIN(1.0f, (float)((now - v->opened) / UIN_FADE_S));
		if (v->screen->frame)
			v->screen->frame(v, dt);
	}
}

static void stats_frame(double draw_s)
{
	int verts, calls;
	double us = draw_s * 1e6;
	if (!ui_native_stats->ivalue) {
		stats.since = 0;
		return;
	}
	d_counters(&verts, &calls);
	stats.draw_sum += us;
	stats.draw_max = BW_MAX(stats.draw_max, us);
	stats.draw_n++;
	stats.verts_max = BW_MAX(stats.verts_max, verts);
	stats.calls_max = BW_MAX(stats.calls_max, calls);
	if (!stats.since)
		stats.since = now;
	if (now - stats.since >= 2) {
		int w0, h0, w1, h1;
		uin_atlas_page_size(0, &w0, &h0);
		uin_atlas_page_size(1, &w1, &h1);
		Con_Printf("ui: hud %.0f us avg %.0f max (%d), draw %.0f us avg %.0f max (%d), max %d verts %d calls, "
		           "atlas H %d, pages %dx%d %dx%d\n",
		           stats.hud_n ? stats.hud_sum / stats.hud_n : 0, stats.hud_max, stats.hud_n,
		           stats.draw_n ? stats.draw_sum / stats.draw_n : 0, stats.draw_max, stats.draw_n, stats.verts_max,
		           stats.calls_max, uin_atlas_height(), w0, h0, w1, h1);
		memset(&stats, 0, sizeof(stats));
		stats.since = now;
	}
}

/* ui_native_atlas: a page at 1:1 over everything, on a checkerboard so
 * white and transparent texels can be told apart */
static void draw_atlas_page(int page)
{
	int w, h, sw, sh, img = uin_atlas_image(page);
	r2dvert_t v[4];
	static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
	if (!img)
		return;
	uin_atlas_page_size(page, &w, &h);
	R_GetSize(&sw, &sh);
	d_xf(NULL);
	for (int y = 0; y < BW_MIN(h, sh); y += 8)
		for (int x = 0; x < BW_MIN(w, sw); x += 8)
			d_fill((float)x, (float)y, 8, 8, ((x ^ y) & 8) ? 0x303030ffu : 0x505050ffu);
	d_flush();
	for (int i = 0; i < 4; i++) {
		v[i].x = (float)((i == 1 || i == 2) ? w : 0);
		v[i].y = (float)(i >= 2 ? h : 0);
		v[i].s = (float)((i == 1 || i == 2) ? 1 : 0);
		v[i].t = (float)(i >= 2 ? 1 : 0);
		memset(v[i].rgba, 255, 4);
	}
	R_Draw2DTriangles(img, v, 4, idx, 6, R2D_ALPHA);
}

void UI_Draw(void)
{
	uin_view_t *order[MAX_VIEWS];
	double t0 = Sys_Seconds();
	int n, sw, sh;
	if (!uin_atlas_ready())
		return;
	R_GetSize(&sw, &sh);
	d_xf(NULL);
	d_blend(R2D_ALPHA);
	/* the client's (cl_menu.c), registered after UI_Init */
	if (!cl_loadstage)
		cl_loadstage = Cvar_Find("cl_loadstage");
	if (cl_loadstage && cl_loadstage->string[0] && uin_loading_draw((float)sw, (float)sh)) {
		/* a map is loading: its screen alone */
	} else {
		n = open_order(order);
		for (int i = 0; i < n; i++) {
			uin_view_t *v = order[i];
			if ((v->flags & UI_EMBED) || !v->open)
				continue; /* the HUD draws through UI_DrawAt; one opened with ui_open, here */
			d_xf(NULL);
			v->screen->draw(v, 0, 0, (float)sw, (float)sh);
		}
		d_xf(NULL);
		if (uin_error_active())
			uin_error_draw((float)sw, (float)sh);
	}
	d_xf(NULL);
	d_flush();
	if (show_atlas)
		draw_atlas_page(show_atlas - 1);
	d_unclip();
	stats_frame(Sys_Seconds() - t0);
	d_reset_counters();
	if (shot_pending[0]) {
		/* r_main.c takes it at R_EndFrame: this very frame */
		Cmd_ExecuteString(va("screenshot %s", shot_pending));
		shot_pending[0] = 0;
	}
}

void UI_DrawAt(int view, float x, float y, float w, float h, const float *rgb, float alpha)
{
	uin_view_t *v = view_at(view);
	double t0;
	if (!v || !uin_atlas_ready())
		return;
	t0 = Sys_Seconds();
	hud_w = w;
	hud_h = h;
	for (int i = 0; i < 3; i++)
		v->rgb[i] = rgb ? rgb[i] : 1;
	v->alpha = alpha;
	d_xf(NULL);
	d_blend(R2D_ALPHA);
	v->screen->draw(v, x, y, w, h);
	d_xf(NULL);
	d_flush();
	d_unclip();
	if (ui_native_stats->ivalue) {
		double us = (Sys_Seconds() - t0) * 1e6;
		stats.hud_sum += us;
		stats.hud_max = BW_MAX(stats.hud_max, us);
		stats.hud_n++;
	}
}

/* --- commands -------------------------------------------------------------------------- */

/* ui_font <family> <game file> [weight]: panorama/webcore_fonts.cfg, as the
 * Panorama build reads it (cl_webcore.c register_fonts) */
static void UIFont_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ui_font <family> <file> [weight]\n");
		return;
	}
	if (!UI_AddFont(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 400))
		Con_Printf("ui_font: couldn't add %s\n", argv[2]);
}

static void UINativeBake_f(int argc, const char **argv)
{
	int h = argc > 1 ? atoi(argv[1]) : 0;
	if (h <= 0) {
		int sw, sh;
		float x, y, w, hh;
		R_GetSize(&sw, &sh);
		uin_safe_rect(&x, &y, &w, &hh);
		h = (int)(hh + 0.5f);
	}
	if (h < 120 || h > 2160) {
		Con_Printf("usage: ui_native_bake [layout height, 120-2160]\n");
		return;
	}
	if (uin_atlas_bake_to_cache(h))
		uin_atlas_invalidate(); /* the running atlas picks the new files up */
}

static void UINativeAtlas_f(int argc, const char **argv)
{
	show_atlas = argc > 1 ? BW_CLAMP(atoi(argv[1]), 0, UIN_NUM_PAGES) : (show_atlas + 1) % (UIN_NUM_PAGES + 1);
	if (show_atlas)
		Con_Printf("ui_native_atlas: page %d (ui_native_atlas 0 hides it)\n", show_atlas - 1);
}

static void UINativeShot_f(int argc, const char **argv)
{
	Str_Copy(shot_pending, argc > 1 ? argv[1] : "*", sizeof(shot_pending));
}

/* ui_key <key> [down|up]: a key through the whole input path
 * (IN_KeyEvent: console, engine menus, the router, binds), for scripted
 * tests of a pad-only flow. A test hook, so it reaches into the client
 * (as the Panorama backend's ui_key does) past the link rule above. */
static void UIKey_f(int argc, const char **argv)
{
	int k;
	if (argc < 2) {
		Con_Printf("usage: ui_key <key> [down|up]\n");
		return;
	}
	if ((k = Key_NumForName(argv[1])) < 0) {
		Con_Printf("ui_key: unknown key %s\n", argv[1]);
		return;
	}
	if (argc < 3 || Str_ICmp(argv[2], "up"))
		IN_KeyEvent(k, true, k < 127 ? k : 0);
	if (argc < 3 || !Str_ICmp(argv[2], "up"))
		IN_KeyEvent(k, false, 0);
}

/* ui_open <layout> [input] and ui_close <view|all>, as the Panorama build
 * has them: a view drawn over the whole screen, for screenshot tests of the
 * same page in both builds (tools/ui/uidiff.py) */
static void UINativeOpen_f(int argc, const char **argv)
{
	int v;
	if (argc < 2) {
		Con_Printf("usage: ui_open <layout> [input]\n");
		return;
	}
	v = UI_Open(argv[1], argc > 2 && !strcmp(argv[2], "input") ? UI_INPUT : 0);
	if (v)
		Con_Printf("ui: view %d is %s\n", v, argv[1]);
}

static void UINativeClose_f(int argc, const char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "all"))
		UI_CloseAll();
	else
		UI_Close(atoi(argv[1]));
}

static void UINativeList_f(int argc, const char **argv)
{
	for (int i = 0; i < MAX_VIEWS; i++)
		if (views[i].open)
			Con_Printf("%d %s (%s)%s%s%s\n", i + 1, views[i].layout, views[i].screen->name,
			           (views[i].flags & UI_INPUT) ? " input" : "", views[i].search[0] ? " " : "", views[i].search);
}

void UI_Init(void)
{
#ifdef BW_XBOX
	const char *safe = "0.05";
#else
	const char *safe = "0";
#endif
	ui_native_safe = Cvar_Get("ui_native_safe", safe, 0, "Native UI: TV-safe inset of the HUD and footer, a fraction of the screen");
	ui_hud_style = Cvar_Get("ui_hud_style", "0", 0, "Native UI: 0 draws the HUD as the PC port does, 1 adds the Figma extras");
	ui_hud_debug = Cvar_Get("ui_hud_debug", "0", 0, "Native UI: 1 outlines the HUD's cluster boxes");
	ui_native_hudtest = Cvar_Get("ui_native_hudtest", "0", 0, "Native UI: 1 shows the HUD with fixed test values");
	ui_native_stats = Cvar_Get("ui_native_stats", "0", 0, "Native UI: 1 prints draw timings every 2 seconds");
	Cmd_AddD("ui_font", UIFont_f, "ui_font <family> <file> [weight]: a font the native UI's faces can name");
	Cmd_AddD("ui_native_bake", UINativeBake_f,
	         "ui_native_bake [H]: bake the native UI atlas for layout height H into ui/native_<H>.* (default: now)");
	Cmd_AddD("ui_native_atlas", UINativeAtlas_f, "ui_native_atlas [0|1|2]: show an atlas page at 1:1 (0 hides)");
	Cmd_AddD("ui_native_shot", UINativeShot_f, "ui_native_shot <name>: screenshot once the UI has drawn");
	Cmd_AddD("ui_list", UINativeList_f, "List the open UI views");
	Cmd_AddD("ui_open", UINativeOpen_f, "ui_open <layout> [input]: open a view over the whole screen (tests)");
	Cmd_AddD("ui_close", UINativeClose_f, "ui_close <view|all>: close a view (tests)");
	Cmd_AddD("ui_key", UIKey_f, "ui_key <key> [down|up]: press a key as the player would (tests)");
	uin_atlas_init();
}

void UI_Shutdown(void)
{
	UI_CloseAll();
	uin_atlas_shutdown();
	SB_Free(&reply);
}
