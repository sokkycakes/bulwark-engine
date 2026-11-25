/* Bulwark engine - WebCore compatibility: games written for FTE's "webcore"
 * HTML plugin (Stiletto) run their menus and HUD as Panorama views here.
 *
 * A page address fte://data/web/<dir>/index.html becomes the layout
 * panorama/layout/web/<dir>.xml (other files: layout/web/<dir>/<name>.xml),
 * so a game, or Bulwark's own data directory, ships a Panorama port of each
 * page. Scripts get the same synchronous bridge the pages use,
 * fte_query(request), and the commands, cvars and gecko_* builtins behave
 * as docs/spec/webcore.md describes. That document was written from the
 * game's own files (pages, QC, configs, notes); no FTE source was read.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "lobby.h"
#include "vid.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../render/render.h"
#include "../server/sv_local.h"
#include "../sound/sound.h"
#include "../ui/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum { ROUTE_NONE, ROUTE_TITLE, ROUTE_LOBBY, ROUTE_PAUSE, ROUTE_ARENA, ROUTE_ONLINE, ROUTE_CUSTOM } route_t;

static cvar_t *webcore_menu_url, *webcore_menumap, *webcore_menu_enabled, *webcore_pause_url, *webcore_menu_music;
static cvar_t *forceqmenu;

static struct {
	bool plugin;      /* plug_load webcore: the game wants the HTML menus */
	bool starts_game; /* the command line starts a game: no title at start-up */
	bool startup_done;
	int menu;         /* the menu surface's view, 0 when closed */
	route_t route;
	char url[512];
	char selector[16]; /* menu_webcore arena <selector> */
	bool title_pending;
	char page_pending[512]; /* WC_OpenPage: shown instead of the title once idle */
	int prevstate;
	bool backdrop;    /* we started the title's background map */
	int music_track;  /* -1 menu_boot, then 1, 2, ... */
	bool music_on;
	int frames;
} wc = {.prevstate = ca_disconnected, .music_track = -1};

/* gecko_create views: the name the game draws them by */
#define MAX_GECKO 16
static struct {
	char name[64];
	char url[512];
	int view;
	float w, h;
	bool csqc; /* made by the client game: closed with it */
} gecko[MAX_GECKO];

/* --- contexts ------------------------------------------------------------------------- */

static bool on_backdrop(void)
{
	char buf[16];
	const char *v;
	if (cl.state == ca_disconnected)
		return false;
	v = CL_ServerKey("background", buf, sizeof(buf));
	return v && atof(v) == 1;
}

/* playing a real game (not the title's background map) */
static bool in_game(void) { return cl.state == ca_active && !on_backdrop(); }

/* nothing being joined or loaded: the title may show */
static bool idle(void)
{
	if (on_backdrop())
		return true;
	return cl.state == ca_disconnected && !sv.active && !CL_DemoPlaying();
}

static bool menus_enabled(void) { return wc.plugin && webcore_menu_enabled->ivalue; }

/* --- page addresses ---------------------------------------------------------------- */

/* fte://data/web/hud/index.html?v=9 -> layout/web/hud.xml, query "?v=9" */
static bool page_layout(const char *url, char *layout, size_t lsize, char *query, size_t qsize)
{
	char path[512], *q;
	if (!Str_NICmp(url, "webcore:", 8))
		url += 8;
	if (!Str_NICmp(url, "fte://", 6))
		url += 6;
	Str_Copy(path, url, sizeof(path));
	q = strchr(path, '?');
	if (query)
		Str_Copy(query, q ? q : "", qsize);
	if (q)
		*q = 0;
	if (Str_NICmp(path, "data/web/", 9))
		return false;
	{
		char *dir = path + 9, *file = strrchr(dir, '/'), *dot;
		if (!file || !file[1] || !Str_ICmp(file + 1, "index.html")) {
			if (file)
				*file = 0;
			/* a page directly under data/web/: scratch.html -> layout/web/scratch.xml */
			else if ((dot = strrchr(dir, '.')) && (!Str_ICmp(dot, ".html") || !Str_ICmp(dot, ".htm")))
				*dot = 0;
			snprintf(layout, lsize, "layout/web/%s.xml", dir);
		} else {
			*file++ = 0;
			if ((dot = strrchr(file, '.')))
				*dot = 0;
			snprintf(layout, lsize, "layout/web/%s/%s.xml", dir, file);
		}
	}
	return UI_LayoutExists(layout);
}

/* a JS string literal */
static void js_string(strbuf_t *sb, const char *s)
{
	SB_Append(sb, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			SB_Append(sb, "\\", 1);
			SB_Append(sb, s, 1);
		} else if (c < 0x20) {
			SB_Printf(sb, "\\u%04x", c);
		} else {
			SB_Append(sb, s, 1);
		}
	}
	SB_Append(sb, "\"", 1);
}

/* a view of a page, with window.location as the page would see it */
static int open_page(const char *url, int flags)
{
	char layout[600], query[256];
	strbuf_t js = {0};
	int view;
	if (!page_layout(url, layout, sizeof(layout), query, sizeof(query))) {
		Con_DPrintf("webcore: no Panorama layout for %s\n", url);
		return 0;
	}
	SB_AppendStr(&js, "var location = {href: ");
	js_string(&js, url);
	SB_AppendStr(&js, ", search: ");
	js_string(&js, query);
	SB_AppendStr(&js, "}; window.location = location;\n");
	view = UI_OpenEx(layout, flags, js.data);
	SB_Free(&js);
	return view;
}

/* --- menu music ---------------------------------------------------------------------- */

static void music_next(void)
{
	char name[64];
	if (wc.music_track < 0) {
		wc.music_track = 0;
		if (S_PlayMusic("music/menu_boot", false))
			return;
	}
	snprintf(name, sizeof(name), "music/menu%d", ++wc.music_track);
	if (S_PlayMusic(name, false))
		return;
	/* past the last track: around again */
	wc.music_track = -1;
	if (!S_PlayMusic("music/menu_boot", false))
		S_PlayMusic("music/menu1", false);
}

static void music_frame(void)
{
	bool want = wc.plugin && webcore_menu_music->ivalue && !in_game() &&
	            (wc.route == ROUTE_TITLE || wc.route == ROUTE_LOBBY || wc.route == ROUTE_ONLINE || EM_Active() || idle());
	if (want && !S_MusicPlaying()) {
		wc.music_on = true;
		music_next();
	} else if (!want && wc.music_on) {
		wc.music_on = false;
		wc.music_track = -1;
		S_StopMusic();
	}
}

/* --- the menu surface ------------------------------------------------------------- */

static void close_menu(void)
{
	if (wc.menu)
		UI_Close(wc.menu);
	wc.menu = 0;
	wc.route = ROUTE_NONE;
	wc.url[0] = 0;
	if (idle())
		wc.title_pending = true; /* the title comes back on a later frame */
}

static void start_backdrop(void)
{
	const char *map = webcore_menumap->string;
	char path[128];
	if (!*map || sv.active || cl.state != ca_disconnected)
		return;
	snprintf(path, sizeof(path), "maps/%s.bsp", map);
	if (!FS_FileExists(path)) {
		Con_DPrintf("webcore: no title map %s\n", path);
		return;
	}
	wc.backdrop = true;
	/* as Nuclide's own menu starts it (map_background) */
	Cbuf_AddText(va("set g_gametype singleplayer\nset sv_playerslots 2\nset coop 1\nset deathmatch 0\n"
	                "set sv_background 1\nmap %s\n",
	                map));
}

static bool open_menu(const char *url, route_t route)
{
	int view;
	if (wc.menu && !Str_ICmp(wc.url, url)) {
		wc.route = route;
		return true; /* the same page stays as it is */
	}
	view = open_page(url, UI_INPUT);
	if (!view)
		return false;
	if (wc.menu)
		UI_Close(wc.menu);
	wc.menu = view;
	wc.route = route;
	Str_Copy(wc.url, url, sizeof(wc.url));
	wc.title_pending = false;
	if (route == ROUTE_TITLE)
		start_backdrop();
	return true;
}

static bool open_title(void)
{
	if (!menus_enabled())
		return false;
	return open_menu(webcore_menu_url->string, ROUTE_TITLE);
}

static void open_arena(const char *selector)
{
	static const char *const fallbacks[][2] = {
	    {"team", "showTeamSelectionMenu"}, {"class", "showClassSelectionMenu"}, {"hero", "showHeroSelectionMenu"}};
	const char *fallback = "showClassSelectionMenu";
	/* a Live set seats its players and picks heroes in its own view */
	if (SF_SetGame())
		return;
	for (size_t i = 0; i < BW_ARRAY_COUNT(fallbacks); i++)
		if (!Str_ICmp(selector, fallbacks[i][0]))
			fallback = fallbacks[i][1];
	Str_Copy(wc.selector, selector, sizeof(wc.selector));
	if (!in_game() || !open_menu("fte://data/web/ql-menu/index.html?mode=arena", ROUTE_ARENA))
		Cbuf_AddText(va("%s vgui\n", fallback)); /* the game's own selector */
}

static void MenuWebcore_f(int argc, const char **argv)
{
	const char *a = argc > 1 ? argv[1] : "";
	if (!*a) {
		if (in_game())
			open_menu(webcore_pause_url->string, ROUTE_PAUSE);
		else
			open_title();
	} else if (!Str_ICmp(a, "title")) {
		open_title();
	} else if (!Str_ICmp(a, "lobby")) {
		open_menu("fte://data/web/lobby-menu/index.html", ROUTE_LOBBY);
	} else if (!Str_ICmp(a, "online")) {
		open_menu("fte://data/web/online/index.html", ROUTE_ONLINE);
	} else if (!Str_ICmp(a, "pause")) {
		open_menu(webcore_pause_url->string, ROUTE_PAUSE);
	} else if (!Str_ICmp(a, "arena")) {
		open_arena(argc > 2 ? argv[2] : "class");
	} else if (!open_menu(a, ROUTE_CUSTOM)) {
		Con_Printf("menu_webcore: can't open %s\n", a);
	}
}

/* a page shown out of a game instead of the title (the online hub after a
 * set): now if nothing is being played, else once the game is left */
void WC_OpenPage(const char *url)
{
	Str_Copy(wc.page_pending, url, sizeof(wc.page_pending));
}

static void MenuWebcoreTitle_f(int argc, const char **argv) { open_title(); }
static void MenuWebcoreLobby_f(int argc, const char **argv)
{
	open_menu("fte://data/web/lobby-menu/index.html", ROUTE_LOBBY);
}
static void MenuLobbyCreate_f(int argc, const char **argv)
{
	Cbuf_AddText("lobby_create_lan\nmenu_webcore_lobby\n");
}
static void CloseMenu_f(int argc, const char **argv) { close_menu(); }

static void PlugLoadWebcore(void) { wc.plugin = true; }

/* togglemenu, when the game uses these menus; false leaves it to the others */
bool WC_ToggleMenu(void)
{
	if (!menus_enabled())
		return false;
	if (in_game()) {
		if (wc.menu)
			close_menu();
		else
			open_menu(webcore_pause_url->string, ROUTE_PAUSE);
		return true;
	}
	if (!wc.menu)
		open_title();
	return true;
}

bool WC_MenuOpen(void) { return wc.menu != 0; }

/* keys while the surface is open; the console key and console come first
 * (IN_KeyEvent). In a game Esc reaches the page and then closes it. */
bool WC_KeyEvent(int key, bool down, int unicode)
{
	if (!wc.menu)
		return false;
	if (key == K_ESCAPE && down && in_game()) {
		UI_KeyEvent(key, down, unicode);
		close_menu();
		return true;
	}
	return UI_KeyEvent(key, down, unicode);
}

void WC_SetStartsGame(bool starts) { wc.starts_game = starts; }

void WC_Frame(void)
{
	wc.frames++;
	SF_Frame();
	if (wc.menu && !UI_IsOpen(wc.menu)) {
		wc.menu = 0; /* closed with ui_close */
		wc.route = ROUTE_NONE;
	}
	/* start-up: once the configs have run, unless a game is starting */
	if (!wc.startup_done && wc.frames > 2) {
		wc.startup_done = true;
		if (!wc.starts_game && idle() && !Lobby_Active())
			wc.title_pending = true;
	}
	if (wc.prevstate != ca_disconnected && cl.state == ca_disconnected) {
		wc.title_pending = true;
		wc.backdrop = false;
		/* views a client game made die with it */
	}
	wc.prevstate = cl.state;
	/* a real game took over: title and lobby pages give way */
	if (in_game() && (wc.route == ROUTE_TITLE || wc.route == ROUTE_LOBBY || wc.route == ROUTE_ONLINE))
		close_menu();
	/* the set view seats players and picks heroes: no arena menu over it */
	if (wc.route == ROUTE_ARENA && SF_SetGame())
		close_menu();
	if (wc.page_pending[0] && cl.state == ca_disconnected && !CL_DemoPlaying() && menus_enabled()) {
		char url[512];
		Str_Copy(url, wc.page_pending, sizeof(url));
		wc.page_pending[0] = 0;
		if (open_menu(url, ROUTE_ONLINE)) {
			wc.title_pending = false;
			Con_SetActive(false);
		}
	}
	/* the lobby ended without a game: back to the title */
	if (wc.route == ROUTE_LOBBY && !Lobby_Active() && idle())
		open_title();
	if (wc.title_pending && !wc.menu && idle() && !Lobby_Active() && menus_enabled() && wc.startup_done) {
		wc.title_pending = false;
		/* the console the engine shows when nothing else is up gives way */
		if (open_title())
			Con_SetActive(false);
	}
	if (wc.backdrop && cl.state == ca_disconnected && !sv.active)
		wc.backdrop = false;
	music_frame();
}

/* --- gecko_* views ------------------------------------------------------------------ */

static int gecko_find(const char *name)
{
	for (int i = 0; i < MAX_GECKO; i++)
		if (gecko[i].view && !Str_ICmp(gecko[i].name, name))
			return i;
	return -1;
}

static void gecko_close(int i)
{
	if (gecko[i].view)
		UI_Close(gecko[i].view);
	memset(&gecko[i], 0, sizeof(gecko[i]));
}

/* the views a client game made close with it */
void WC_ClientGameShutdown(void)
{
	for (int i = 0; i < MAX_GECKO; i++)
		if (gecko[i].view && gecko[i].csqc)
			gecko_close(i);
}

static bool gecko_navigate(int i, const char *uri)
{
	int view;
	if (!Str_NICmp(uri, "cmd:", 4))
		return true; /* transparent, opaque, focus: views are always transparent here */
	view = open_page(uri, UI_EMBED);
	if (!view)
		return false;
	if (gecko[i].view)
		UI_Close(gecko[i].view);
	gecko[i].view = view;
	Str_Copy(gecko[i].url, uri, sizeof(gecko[i].url));
	return true;
}

static void bi_gecko_create(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0), *uri = vm->argc > 1 ? QC_PSTR(vm, 1) : "";
	int i = gecko_find(name);
	if (i < 0) {
		for (i = 0; i < MAX_GECKO && gecko[i].view; i++)
			;
		if (i == MAX_GECKO || !*uri) {
			QC_ReturnFloat(vm, 0);
			return;
		}
		Str_Copy(gecko[i].name, name, sizeof(gecko[i].name));
	}
	gecko[i].csqc = vm == cl.csqc;
	if (!gecko_navigate(i, uri)) {
		if (!gecko[i].view)
			memset(&gecko[i], 0, sizeof(gecko[i]));
		QC_ReturnFloat(vm, 0);
		return;
	}
	QC_ReturnFloat(vm, 1);
}

static void bi_gecko_destroy(qcvm_t *vm)
{
	int i = gecko_find(QC_PSTR(vm, 0));
	if (i >= 0)
		gecko_close(i);
}

static void bi_gecko_navigate(qcvm_t *vm)
{
	int i = gecko_find(QC_PSTR(vm, 0));
	if (i >= 0)
		gecko_navigate(i, QC_PSTR(vm, 1));
}

static void bi_gecko_resize(qcvm_t *vm)
{
	int i = gecko_find(QC_PSTR(vm, 0));
	if (i >= 0) {
		gecko[i].w = QC_PFLOAT(vm, 1);
		gecko[i].h = QC_PFLOAT(vm, 2);
	}
}

static void bi_gecko_get_texture_extent(qcvm_t *vm)
{
	int i = gecko_find(QC_PSTR(vm, 0));
	float v[3] = {0, 0, 0};
	if (i >= 0)
		UI_ViewSize(gecko[i].view, &v[0], &v[1]);
	QC_ReturnVector(vm, v);
}

static void bi_gecko_getproperty(qcvm_t *vm)
{
	int i = gecko_find(QC_PSTR(vm, 0));
	const char *prop = QC_PSTR(vm, 1);
	if (i >= 0 && !Str_ICmp(prop, "url"))
		QC_ReturnString(vm, gecko[i].url);
	else if (i >= 0 && !Str_ICmp(prop, "status"))
		QC_ReturnString(vm, "ready");
	else
		QC_ReturnString(vm, "");
}

/* no Stiletto view takes input; these keep other games going */
static void bi_gecko_keyevent(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }
static void bi_gecko_mousemove(qcvm_t *vm) { (void)vm; }

/* hud_drawnative(velocity): Stiletto's FTE build drew a native HUD here
 * when native_hud is set; this draws the HUD page over the whole screen */
static void bi_hud_drawnative(qcvm_t *vm)
{
	int i = gecko_find("webcore_hud");
	int w, h;
	if (i < 0) {
		cvar_t *url = Cvar_Find("webcore_hud_url");
		for (i = 0; i < MAX_GECKO && gecko[i].view; i++)
			;
		if (i == MAX_GECKO)
			return;
		Str_Copy(gecko[i].name, "webcore_hud", sizeof(gecko[i].name));
		gecko[i].csqc = true;
		if (!gecko_navigate(i, url && *url->string ? url->string : "fte://data/web/hud/index.html")) {
			memset(&gecko[i], 0, sizeof(gecko[i]));
			return;
		}
	}
	R_GetSize(&w, &h);
	UI_DrawAt(gecko[i].view, 0, 0, (float)w, (float)h, NULL, 1);
}

/* drawpic of a gecko view's name draws the view there */
static bool pic_hook(const char *name, float x, float y, float w, float h, const float *rgb, float alpha,
                     float *size)
{
	int i = gecko_find(name);
	if (i < 0)
		return false;
	if (size) {
		size[0] = gecko[i].w;
		size[1] = gecko[i].h;
		if (size[0] <= 0 || size[1] <= 0)
			UI_ViewSize(gecko[i].view, &size[0], &size[1]);
		return true;
	}
	UI_DrawAt(gecko[i].view, x, y, w, h, rgb, alpha);
	return true;
}

const qc_builtin_t cl_webcore_builtins[] = {
	{"gecko_create", bi_gecko_create}, {"gecko_destroy", bi_gecko_destroy},
	{"gecko_navigate", bi_gecko_navigate}, {"gecko_resize", bi_gecko_resize},
	{"gecko_get_texture_extent", bi_gecko_get_texture_extent}, {"gecko_getproperty", bi_gecko_getproperty},
	{"gecko_keyevent", bi_gecko_keyevent}, {"gecko_mousemove", bi_gecko_mousemove},
	{"hud_drawnative", bi_hud_drawnative}, {NULL, NULL}};

/* --- the bridge: fte_query ---------------------------------------------------------- */

/* the console commands pages may run, exactly as written */
static const char *const cbuf_allowed[] = {
	"menu_options", "quit", "lobby_create_lan", "lobby_create_online", "menu_webcore_lobby", "menu_lobby_create",
	"menu_webcore_title", "menu_webcore", "webcore_closemenu", "menu_hostmenu", "menu_changeteam", "menu_callvote",
	"lobby_close", "disconnect", "menu_webcore online", "ready", "map_restart", "togglemenu", "menu_maps", "menu_newmulti", NULL};

static void reply_ok(strbuf_t *out, bool ok, const char *error)
{
	if (ok)
		SB_AppendStr(out, "{\"ok\":true}");
	else if (error)
		SB_Printf(out, "{\"ok\":false,\"error\":\"%s\"}", error);
	else
		SB_AppendStr(out, "{\"ok\":false}");
}

static bool is_object(const char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	return *s == '{';
}

static void q_cbuf(const char *cmd, strbuf_t *out)
{
	for (int i = 0; cbuf_allowed[i]; i++)
		if (!strcmp(cmd, cbuf_allowed[i])) {
			Cbuf_AddText(cmd);
			Cbuf_AddText("\n");
			SB_AppendStr(out, "ok");
			return;
		}
	Con_DPrintf("webcore: page asked to run \"%s\", which isn't allowed\n", cmd);
	SB_AppendStr(out, "denied");
}

static void q_localsound(const char *sample, strbuf_t *out)
{
	static const char *const exts[] = {"", ".wav", ".ogg"};
	char name[128], path[160];
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		snprintf(name, sizeof(name), "%s%s", sample, exts[i]);
		snprintf(path, sizeof(path), "sound/%s", name);
		if (strchr(sample, '.') ? i == 0 : (i > 0 && FS_FileExists(path))) {
			/* a quick run of the same sound doesn't restart it */
			if (!S_LocalSoundPlaying(name))
				S_LocalSound(name);
			break;
		}
	}
	SB_AppendStr(out, "ok");
}

static void q_cvar(const char *name, strbuf_t *out)
{
	cvar_t *v = Cvar_Find(name);
	if (v && !strstr(name, "password") && !strstr(name, "rcon"))
		SB_AppendStr(out, v->string);
}

static void q_getstats(strbuf_t *out)
{
	SB_AppendStr(out, "[");
	if (cl.state == ca_active)
		for (int i = 0; i < 32; i++) {
			float f;
			if (cl.stattype[i] == 2)
				memcpy(&f, &cl.statval[i], sizeof(f));
			else
				f = (float)(int32_t)cl.statval[i];
			SB_Printf(out, "%s%g", i ? "," : "", cl.stattype[i] == 1 ? 0.0 : f);
		}
	SB_AppendStr(out, "]");
}

/* the HUD's numbers, from the cvars the client game publishes */
static void q_gethud(strbuf_t *out)
{
	static const struct {
		const char *key, *cvar;
		bool number;
	} fields[] = {
		{"health", "webcore_hud_health", true},      {"healthMax", "webcore_hud_health_max", true},
		{"ammo", "webcore_hud_ammo", true},          {"ammoCurrent", "webcore_hud_clip", true},
		{"ammoMax", "webcore_hud_ammo_max", true},   {"charges", "webcore_hud_charges", true},
		{"timer", "webcore_hud_timer", false},       {"speed", "webcore_hud_speed", true},
		{"scoreLeft", "webcore_hud_score_left", true}, {"scoreRight", "webcore_hud_score_right", true},
		{"player1", "webcore_hud_player1", false},   {"player2", "webcore_hud_player2", false},
		{"reload", "webcore_hud_reload", true},      {"exLevel", "webcore_hud_ex", true},
	};
	bool first = true;
	if (cl.state != ca_active)
		return;
	SB_AppendStr(out, "{");
	for (size_t i = 0; i < BW_ARRAY_COUNT(fields); i++) {
		cvar_t *v = Cvar_Find(fields[i].cvar);
		if (!v)
			continue;
		SB_Printf(out, "%s\"%s\":", first ? "" : ",", fields[i].key);
		first = false;
		if (fields[i].number) {
			SB_Printf(out, "%g", atof(v->string));
		} else {
			SB_AppendStr(out, "\"");
			for (const char *s = v->string; *s; s++)
				if (*s == '"' || *s == '\\')
					SB_Printf(out, "\\%c", *s);
				else if ((unsigned char)*s >= 0x20)
					SB_Append(out, s, 1);
			SB_AppendStr(out, "\"");
		}
	}
	SB_AppendStr(out, "}");
}

static void q_getarena(strbuf_t *out)
{
	cvar_t *v = Cvar_Find("webcore_arena_snapshot");
	SB_AppendStr(out, v && is_object(v->string) ? v->string : "{\"active\":false}");
}

static bool arena_active(void)
{
	cvar_t *v = Cvar_Find("webcore_arena_snapshot");
	return in_game() && v && strstr(v->string, "\"active\":true");
}

/* a word of the space-separated list */
static bool word_in(const char *word, const char *list)
{
	size_t n = strlen(word);
	for (const char *p = list; *p;) {
		while (*p == ' ')
			p++;
		const char *e = p;
		while (*e && *e != ' ')
			e++;
		if ((size_t)(e - p) == n && !strncmp(p, word, n))
			return true;
		p = e;
	}
	return false;
}

static void q_arena_action(const char *a, strbuf_t *out)
{
	static const struct {
		const char *action, *cmd;
	} simple[] = {{"join", "cmd play"}, {"play", "cmd play"}, {"choosehero", "cmd choosehero"},
	              {"ready", "cmd ready"}, {"unready", "cmd notready"}, {"spectate", "cmd spectate"}};
	if (!strcmp(a, "close")) {
		close_menu();
		reply_ok(out, true, NULL);
		return;
	}
	if (!arena_active()) {
		reply_ok(out, false, "arena not active");
		return;
	}
	if (!strncmp(a, "hero:", 5)) {
		const char *id = a + 5;
		cvar_t *ids = Cvar_Find("webcore_arena_hero_ids");
		bool ok = *id != 0;
		for (const char *p = id; *p && ok; p++)
			ok = isalnum((unsigned char)*p) || *p == '_';
		if (!ok || !ids || !word_in(id, ids->string)) {
			reply_ok(out, false, "unknown hero");
			return;
		}
		Cbuf_AddText(va("cmd selecthero %s\n", id));
		reply_ok(out, true, NULL);
		return;
	}
	for (size_t i = 0; i < BW_ARRAY_COUNT(simple); i++)
		if (!strcmp(a, simple[i].action)) {
			Cbuf_AddText(va("%s\n", simple[i].cmd));
			if (!strcmp(a, "spectate"))
				close_menu();
			reply_ok(out, true, NULL);
			return;
		}
	reply_ok(out, false, "unknown action");
}

static void q_lobby_join(const char *target, strbuf_t *out)
{
	size_t n = strlen(target);
	bool ok = n >= 1 && n <= 64;
	for (const char *p = target; *p && ok; p++)
		ok = isalnum((unsigned char)*p) || strchr("._:-[]", *p);
	if (!ok) {
		SB_AppendStr(out, "denied");
		return;
	}
	Cbuf_AddText(va("lobby_join %s\n", target));
	SB_AppendStr(out, "ok");
}

/* glyph:<key or command>[ <device>]: the controller prompt for a binding,
 * as "<image>|<label>" ("{images}/glyphs/steamdeck/south.png|A"), or ""
 * when no controller binding exists (docs/spec/steam-input.md) */
static void q_glyph(const char *what, strbuf_t *out)
{
	char name[128], path[160], label[64];
	int dev = -1;
	const char *sp = strchr(what, ' ');
	snprintf(name, sizeof(name), "%.*s", sp ? (int)(sp - what) : (int)strlen(what), what);
	if (sp)
		dev = atoi(sp + 1);
	if (IN_PadGlyph && IN_PadGlyph(name, dev, path, sizeof(path), label, sizeof(label))) {
		SB_AppendStr(out, path);
		SB_AppendStr(out, "|");
		SB_AppendStr(out, label);
	}
}

static void query(int view, const char *req, strbuf_t *out)
{
	(void)view;
	if (!strncmp(req, "cbuf:", 5))
		q_cbuf(req + 5, out);
	else if (!strncmp(req, "localsound:", 11))
		q_localsound(req + 11, out);
	else if (!strncmp(req, "clipboard_copy:", 15))
		SB_AppendStr(out, VID_SetClipboard(req + 15) ? "ok" : "failed");
	else if (!strncmp(req, "cvar:", 5))
		q_cvar(req + 5, out);
	else if (!strncmp(req, "glyph:", 6))
		q_glyph(req + 6, out);
	else if (!strncmp(req, "get_cvar ", 9))
		q_cvar(req + 9, out);
	else if (!strcmp(req, "getstats"))
		q_getstats(out);
	else if (!strcmp(req, "gethud"))
		q_gethud(out);
	else if (!strcmp(req, "getarena"))
		q_getarena(out);
	else if (!strncmp(req, "arena_action:", 13))
		q_arena_action(req + 13, out);
	else if (!strncmp(req, "lobby_join:", 11))
		q_lobby_join(req + 11, out);
	else if (!SF_Query(req, out))
		Lobby_Query(req, out); /* the lobby's requests; anything else replies "" */
}

/* prints the reply a page would get, to check the bridge or to capture
 * data for rendering a page elsewhere */
static void WebcoreQuery_f(int argc, const char **argv)
{
	strbuf_t out = {0};
	if (argc < 2) {
		Con_Printf("usage: webcore_query <request>\n");
		return;
	}
	query(0, argv[1], &out);
	Con_Printf("%s\n", out.data ? out.data : "");
	SB_Free(&out);
}

/* --- set-up -------------------------------------------------------------------------- */

/* fonts the pages name by family (FTE's plugin registered them from the
 * game's loose font files, docs/spec/webcore.md 2.2): ui_font lines in
 * panorama/webcore_fonts.cfg, which Bulwark's data directory provides */
static void register_fonts(void)
{
	/* ahead of the rest of the config that loaded the plugin, which goes
	 * on to open the title */
	if (FS_FileExists("panorama/webcore_fonts.cfg"))
		Cbuf_InsertText("exec panorama/webcore_fonts.cfg\n");
}

static void PlugLoad(void)
{
	PlugLoadWebcore();
	register_fonts();
}

bool WC_PluginLoad(const char *name)
{
	/* FTE plugins this module stands in for */
	if (Str_ICmp(name, "webcore") && Str_ICmp(name, "cef"))
		return false;
	if (!wc.plugin)
		PlugLoad();
	return true;
}

void WC_Init(void)
{
	webcore_menu_url = Cvar_Get("webcore_menu_url", "fte://data/web/title-menu/index.html", 0,
	                            "The title page of the HTML menus");
	webcore_menumap = Cvar_Get("webcore_menumap", "background02", CVAR_ARCHIVE,
	                           "Map shown behind the title page, empty for none");
	webcore_menu_enabled = Cvar_Get("webcore_menu_enabled", "1", 0, "0 keeps the HTML menus from opening");
	webcore_pause_url = Cvar_Get("webcore_pause_url", "fte://data/web/pause-menu/index.html", 0,
	                             "The in-game pause page of the HTML menus");
	webcore_menu_music = Cvar_Get("webcore_menu_music", "1", CVAR_ARCHIVE, "Play the menu music under the title");
	forceqmenu = Cvar_Get("forceqmenu", "0", 0, "1 skips the menu module (menu.dat)");
	/* FTE's player model settings, which Stiletto's autoexec sets */
	Cvar_Get("headmodel", "", CVAR_ARCHIVE | CVAR_USERINFO, "Head model (userinfo)");
	Cvar_Get("team_model", "", CVAR_ARCHIVE | CVAR_USERINFO, "Player model on teams (userinfo)");
	Cvar_Get("team_headmodel", "", CVAR_ARCHIVE | CVAR_USERINFO, "Head model on teams (userinfo)");
	/* Stiletto's configs turn its Slint UI off; there is none here */
	Cvar_Get("slint_menu", "0", 0, NULL);
	Cvar_Get("slint_ui", "0", 0, NULL);
	Cvar_Get("slint_hud", "0", 0, NULL);
	Cmd_AddD("menu_webcore", MenuWebcore_f,
	         "menu_webcore [title|lobby|pause|arena <team|class|hero>|<url>]: open an HTML menu page");
	Cmd_AddD("menu_webcore_title", MenuWebcoreTitle_f, "Open the title page");
	Cmd_AddD("menu_webcore_lobby", MenuWebcoreLobby_f, "Open the lobby page");
	Cmd_AddD("menu_lobby_create", MenuLobbyCreate_f, "Create a LAN lobby and show it");
	Cmd_AddD("webcore_closemenu", CloseMenu_f, "Close the HTML menu page");
	Cmd_AddD("webcore_query", WebcoreQuery_f, "webcore_query \"<request>\": print the bridge's reply to a page request");
	SF_Init();
	UI_SetQueryHandler(query);
	R_SetPicHook(pic_hook);
	Plug_SetBuiltinHook(WC_PluginLoad);
}
