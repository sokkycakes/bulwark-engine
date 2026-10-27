/* Bulwark engine - the engine's own scoreboards and weapon wheel, for games
 * that don't draw them (a game that registers +showscores and friends gets
 * the commands instead), and the skin commands.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../client/client.h"
#include "../client/console.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool showscores, showteamscores, showdm, showinfo, infoplaque, wheel;
static float wheel_x, wheel_y;
static const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0}, gold[3] = {1, 0.8f, 0.3f};

typedef struct {
	int slot;
	float frags;
	int ping;
	char name[64], team[32];
} scoreline_t;

static int cmp_frags(const void *a, const void *b)
{
	const scoreline_t *x = a, *y = b;
	return x->frags < y->frags ? 1 : x->frags > y->frags ? -1 : x->slot - y->slot;
}

static int gather(scoreline_t *out)
{
	int n = 0;
	char v[64];
	for (int i = 0; i < CL_MAX_PLAYERS; i++) {
		if (!CL_PlayerKey(i, "name", out[n].name, sizeof(out[n].name))[0])
			continue;
		out[n].slot = i;
		out[n].frags = (float)atof(CL_PlayerKey(i, "frags", v, sizeof(v)));
		out[n].ping = atoi(CL_PlayerKey(i, "ping", v, sizeof(v)));
		CL_PlayerKey(i, "team", out[n].team, sizeof(out[n].team));
		n++;
	}
	qsort(out, (size_t)n, sizeof(*out), cmp_frags);
	return n;
}

typedef struct {
	char team[32];
	float frags;
	int players, ping;
} teamline_t;

static int gather_teams(const scoreline_t *p, int n, teamline_t *out)
{
	int t = 0;
	for (int i = 0; i < n; i++) {
		int k;
		for (k = 0; k < t; k++)
			if (!strcmp(out[k].team, p[i].team))
				break;
		if (k == t) {
			if (t == CL_MAX_PLAYERS)
				continue;
			memset(&out[t], 0, sizeof(out[t]));
			Str_Copy(out[t].team, p[i].team, sizeof(out[t].team));
			t++;
		}
		out[k].frags += p[i].frags;
		out[k].players++;
		out[k].ping += p[i].ping;
	}
	for (int i = 0; i < t; i++)
		for (int k = i + 1; k < t; k++)
			if (out[k].frags > out[i].frags) {
				teamline_t tmp = out[i];
				out[i] = out[k];
				out[k] = tmp;
			}
	return t;
}

static void box(float x, float y, float w, float h) { R_DrawFill(x, y, w, h, black, 0.6f, 0); }

static void draw_scores(int w, int h, float size)
{
	static scoreline_t lines[CL_MAX_PLAYERS];
	int n = gather(lines);
	float bw = size * 40, x = (w - bw) / 2, y = h * 0.15f;
	box(x - size, y - size, bw + size * 2, (n + 2) * size * 1.25f + size);
	R_DrawString(x, y, "frags  ping  team      name", size, size, gold, 1, 0, false);
	y += size * 1.5f;
	for (int i = 0; i < n; i++, y += size * 1.25f) {
		char line[160];
		snprintf(line, sizeof(line), "%5g  %4d  %-8.8s  %s", lines[i].frags, lines[i].ping, lines[i].team, lines[i].name);
		R_DrawString(x, y, line, size, size, lines[i].slot == cl.playerslot ? gold : white, 1, 0, true);
	}
}

static void draw_teamscores(int w, int h, float size)
{
	static scoreline_t lines[CL_MAX_PLAYERS];
	static teamline_t teams[CL_MAX_PLAYERS];
	int n = gather(lines), t = gather_teams(lines, n, teams);
	float bw = size * 32, x = (w - bw) / 2, y = h * 0.15f;
	box(x - size, y - size, bw + size * 2, (t + 2) * size * 1.25f + size);
	R_DrawString(x, y, "frags  players  ping  team", size, size, gold, 1, 0, false);
	y += size * 1.5f;
	for (int i = 0; i < t; i++, y += size * 1.25f) {
		char line[128];
		snprintf(line, sizeof(line), "%5g  %7d  %4d  %.32s", teams[i].frags, teams[i].players,
		         teams[i].players ? teams[i].ping / teams[i].players : 0, teams[i].team[0] ? teams[i].team : "(none)");
		R_DrawString(x, y, line, size, size, white, 1, 0, true);
	}
}

/* the leaders, small, in the corner */
static void draw_dm(int w, int h, float size)
{
	static scoreline_t lines[CL_MAX_PLAYERS];
	int n = BW_MIN(gather(lines), 4);
	float x = w - size * 22, y = size * 4;
	if (!n)
		return;
	box(x - size * 0.5f, y - size * 0.5f, size * 21, n * size * 1.25f + size * 0.5f);
	for (int i = 0; i < n; i++, y += size * 1.25f) {
		char line[96];
		snprintf(line, sizeof(line), "%4g %s", lines[i].frags, lines[i].name);
		R_DrawString(x, y, line, size, size, lines[i].slot == cl.playerslot ? gold : white, 1, 0, true);
	}
}

static void draw_info(int w, int h, float size)
{
	const char *p = cl.serverinfo;
	float y = h * 0.15f, x = size * 4;
	int rows = 0;
	for (const char *q = p; *q; q++)
		rows += *q == '\\';
	box(x - size, y - size, w - x * 2 + size * 2, (rows / 2 + 2) * size * 1.25f);
	R_DrawString(x, y, "server info", size, size, gold, 1, 0, false);
	y += size * 1.5f;
	while (*p == '\\') {
		char key[64], value[256], line[340];
		size_t n;
		p++;
		n = strcspn(p, "\\");
		snprintf(key, sizeof(key), "%.*s", (int)BW_MIN(n, sizeof(key) - 1), p);
		p += n;
		if (*p != '\\')
			break;
		p++;
		n = strcspn(p, "\\");
		snprintf(value, sizeof(value), "%.*s", (int)BW_MIN(n, sizeof(value) - 1), p);
		p += n;
		snprintf(line, sizeof(line), "%-20s %s", key, value);
		R_DrawString(x, y, line, size, size, white, 1, 0, true);
		y += size * 1.25f;
	}
}

static void draw_plaque(int w, int h, float size)
{
	char host[128], limit[32], line[256];
	int players = 0;
	float y = size * 3;
	for (int i = 0; i < CL_MAX_PLAYERS; i++) {
		char name[8];
		players += CL_PlayerKey(i, "name", name, sizeof(name))[0] != 0;
	}
	CL_ServerKey("hostname", host, sizeof(host));
	CL_ServerKey("timelimit", limit, sizeof(limit));
	box(w / 2 - size * 20, y - size * 0.5f, size * 40, size * 4.5f);
	snprintf(line, sizeof(line), "%s", host[0] ? host : cl.servername);
	R_DrawString((w - R_StringWidth(line, size, size, true)) / 2, y, line, size, size, gold, 1, 0, true);
	snprintf(line, sizeof(line), "%s - %d player%s%s%s%s", cl.mapname, players, players == 1 ? "" : "s",
	         atof(limit) > 0 ? ", time limit " : "", atof(limit) > 0 ? limit : "", atof(limit) > 0 ? " min" : "");
	R_DrawString((w - R_StringWidth(line, size, size, true)) / 2, y + size * 1.5f, line, size, size, white, 1, 0, true);
	{
		int t = (int)cl.time;
		snprintf(line, sizeof(line), "%d:%02d played", t / 60, t % 60);
		R_DrawString((w - R_StringWidth(line, size, size, true)) / 2, y + size * 3, line, size, size, white, 1, 0, true);
	}
}

/* --- weapon wheel ----------------------------------------------------------------- */

#define IT_STAT 15
static const struct {
	int bit;
	const char *name;
} wheel_weapons[8] = {{4096, "axe"}, {1, "shotgun"}, {2, "super shotgun"}, {4, "nailgun"}, {8, "super nailgun"},
                      {16, "grenades"}, {32, "rockets"}, {64, "lightning"}};

static bool wheel_has(int i)
{
	int items = CL_StatInt(IT_STAT);
	return !items || (items & wheel_weapons[i].bit);
}

static int wheel_choice(void)
{
	float len = sqrtf(wheel_x * wheel_x + wheel_y * wheel_y), a;
	int slot;
	if (len < 40)
		return -1;
	a = atan2f(wheel_x, -wheel_y); /* 0 at the top, clockwise */
	if (a < 0)
		a += 2 * (float)M_PI;
	slot = (int)floorf(a / (2 * (float)M_PI / 8) + 0.5f) % 8;
	return wheel_has(slot) ? slot : -1;
}

static void draw_wheel(int w, int h, float size)
{
	float cx = w / 2.0f, cy = h / 2.0f, r = BW_MIN(w, h) * 0.3f;
	int pick = wheel_choice();
	for (int i = 0; i < 8; i++) {
		float a = i * 2 * (float)M_PI / 8, x = cx + sinf(a) * r, y = cy - cosf(a) * r;
		char label[48];
		const float *rgb = i == pick ? gold : white;
		snprintf(label, sizeof(label), "%d %s", i + 1, wheel_weapons[i].name);
		box(x - size * 8, y - size, size * 16, size * 2);
		R_DrawString(x - R_StringWidth(label, size, size, false) / 2, y - size / 2, label, size, size, rgb,
		             wheel_has(i) ? 1 : 0.3f, 0, false);
	}
}

bool HUD_MouseMove(float dx, float dy)
{
	if (!wheel)
		return false;
	wheel_x = BW_CLAMP(wheel_x + dx, -200.0f, 200.0f);
	wheel_y = BW_CLAMP(wheel_y + dy, -200.0f, 200.0f);
	return true;
}

static void Wheel_f(int argc, const char **argv)
{
	if (argv[0][0] == '+') {
		wheel = true;
		wheel_x = wheel_y = 0;
		return;
	}
	if (wheel) {
		int pick = wheel_choice();
		wheel = false;
		if (pick >= 0)
			Cbuf_AddText(va("impulse %d\n", pick + 1));
	}
}

void R_DrawHud(int w, int h)
{
	float size = 8;
	if (cl.state != ca_active || Con_IsActive())
		return;
	if (infoplaque)
		draw_plaque(w, h, size);
	if (showdm)
		draw_dm(w, h, size);
	if (showscores)
		draw_scores(w, h, size);
	else if (showteamscores)
		draw_teamscores(w, h, size);
	if (showinfo)
		draw_info(w, h, size);
	if (wheel)
		draw_wheel(w, h, size);
}

static void Toggle_f(int argc, const char **argv)
{
	bool on = argv[0][0] == '+';
	const char *n = argv[0] + 1;
	if (!strcmp(n, "showscores"))
		showscores = on;
	else if (!strcmp(n, "showteamscores"))
		showteamscores = on;
	else if (!strcmp(n, "showdm"))
		showdm = on;
	else if (!strcmp(n, "showinfo"))
		showinfo = on;
	else if (!strcmp(n, "infoplaque"))
		infoplaque = on;
}

static void Ctfscores_f(int argc, const char **argv)
{
	static scoreline_t lines[CL_MAX_PLAYERS];
	static teamline_t teams[CL_MAX_PLAYERS];
	int n = gather(lines), t = gather_teams(lines, n, teams);
	if (cl.state < ca_loading) {
		Con_Printf("Not connected.\n");
		return;
	}
	for (int i = 0; i < t; i++)
		Con_Printf("%-16s %5g frags, %d player%s\n", teams[i].team[0] ? teams[i].team : "(no team)", teams[i].frags,
		           teams[i].players, teams[i].players == 1 ? "" : "s");
}

/* --- skins ---------------------------------------------------------------------- */

static void reload_skin(const char *skin)
{
	char name[96];
	if (!skin[0] || !FS_SafeName(skin))
		return;
	snprintf(name, sizeof(name), "skins/%s", skin);
	R_ReloadImage(name);
}

/* skins: fetch the players' skins again (after new ones were added) */
static void Skins_f(int argc, const char **argv)
{
	int n = 0;
	if (cl.state < ca_loading) {
		Con_Printf("Can't \"%s\", not connected\n", argv[0]);
		return;
	}
	for (int i = 0; i < CL_MAX_PLAYERS; i++) {
		char name[64], skin[64];
		if (!CL_PlayerKey(i, "name", name, sizeof(name))[0])
			continue;
		CL_PlayerKey(i, "skin", skin, sizeof(skin));
		if (!Str_ICmp(argv[0], "allskins") || skin[0]) {
			Con_Printf("%s: %s\n", name, skin[0] ? skin : "(default)");
			reload_skin(skin);
			n++;
		}
	}
	if (!n)
		Con_Printf("no player skins\n");
}

static void list_skin(const char *name, const char *searchpath, void *ctx)
{
	(*(int *)ctx)++;
	Con_Printf("%s\n", name + 6);
}

static void Listskins_f(int argc, const char **argv)
{
	int n = 0;
	FS_List("skins/*.pcx", true, list_skin, &n);
	FS_List("skins/*.png", true, list_skin, &n);
	FS_List("skins/*.tga", true, list_skin, &n);
	FS_List("skins/*.jpg", true, list_skin, &n);
	Con_Printf("%d skins\n", n);
}

void HUD_Init(void)
{
	static const char *const toggles[] = {"showscores", "showteamscores", "showdm", "showinfo", "infoplaque", NULL};
	static const char *const descs[] = {"the scoreboard", "the team scores", "the leaders", "the server info",
	                                    "the game's details", NULL};
	for (int i = 0; toggles[i]; i++) {
		Cmd_AddD(va("+%s", toggles[i]), Toggle_f, va("Show %s while held", descs[i]));
		Cmd_AddD(va("-%s", toggles[i]), Toggle_f, NULL);
	}
	Cmd_AddD("+weaponwheel", Wheel_f, "Hold to pick a weapon with the mouse");
	Cmd_AddD("-weaponwheel", Wheel_f, NULL);
	Cmd_AddD("ctfscores", Ctfscores_f, "Print the team scores");
	Cmd_AddD("skins", Skins_f, "Reload the skins players use");
	Cmd_AddD("allskins", Skins_f, "Reload every player's skin");
	Cmd_AddD("listskins", Listskins_f, "List the skins in skins/");
}
