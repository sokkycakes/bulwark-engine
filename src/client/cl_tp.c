/* Bulwark engine - QuakeWorld teamplay: $macros about the player (health,
 * armor, location, what was just picked up...), %-codes in say_team, .loc
 * files naming places in a map, message triggers, the tp_* item lists and
 * the inventory and seat commands that go with them.
 *
 * Macros read the player's stats (svc_stats), so they describe games that
 * use the standard Quake fields; a game that keeps its own state (Nuclide)
 * shows zeros. Item numbers are the Quake items bits.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../render/render.h"
#include "../server/bspfile.h"
#include "../sound/sound.h"

#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Quake items bits */
#define IT_SHOTGUN 1
#define IT_SUPER_SHOTGUN 2
#define IT_NAILGUN 4
#define IT_SUPER_NAILGUN 8
#define IT_GRENADE_LAUNCHER 16
#define IT_ROCKET_LAUNCHER 32
#define IT_LIGHTNING 64
#define IT_AXE 4096
#define IT_ARMOR1 8192
#define IT_ARMOR2 16384
#define IT_ARMOR3 32768
#define IT_SUPERHEALTH 65536
#define IT_INVISIBILITY 524288
#define IT_INVULNERABILITY 1048576
#define IT_SUIT 2097152
#define IT_QUAD 4194304

enum { ST_HEALTH = 0, ST_AMMO = 3, ST_ARMOR = 4, ST_SHELLS = 6, ST_NAILS = 7, ST_ROCKETS = 8, ST_CELLS = 9,
       ST_ACTIVEWEAPON = 10, ST_ITEMS = 15 };

/* weapons by QuakeWorld number: 1 axe .. 8 lightning */
static const struct {
	int bit, ammo;
	const char *cvar;
} weapons[9] = {{0, 0, NULL},
                {IT_AXE, -1, "tp_name_axe"},
                {IT_SHOTGUN, ST_SHELLS, "tp_name_sg"},
                {IT_SUPER_SHOTGUN, ST_SHELLS, "tp_name_ssg"},
                {IT_NAILGUN, ST_NAILS, "tp_name_ng"},
                {IT_SUPER_NAILGUN, ST_NAILS, "tp_name_sng"},
                {IT_GRENADE_LAUNCHER, ST_ROCKETS, "tp_name_gl"},
                {IT_ROCKET_LAUNCHER, ST_ROCKETS, "tp_name_rl"},
                {IT_LIGHTNING, ST_CELLS, "tp_name_lg"}};

static void str_append(char *out, size_t size, const char *fmt, ...) BW_PRINTF(3, 4);
static void str_append(char *out, size_t size, const char *fmt, ...)
{
	size_t n = strlen(out);
	va_list ap;
	if (n + 1 >= size)
		return;
	va_start(ap, fmt);
	vsnprintf(out + n, size - n, fmt, ap);
	va_end(ap);
}

static const char *skey(const char *key)
{
	static char v[256];
	return CL_ServerKey(key, v, sizeof(v));
}

/* the arguments from 'first' on, quoted where they need it */
static const char *args_from(int argc, const char **argv, int first)
{
	static char line[1024];
	line[0] = 0;
	for (int i = first; i < argc; i++)
		str_append(line, sizeof(line), strpbrk(argv[i], " \t;") || !argv[i][0] ? "%s\"%s\"" : "%s%s", i > first ? " " : "", argv[i]);
	return line;
}

static cvar_t *cl_parseSay, *cl_triggers, *tp_loadlocs, *tp_weapon_order, *tp_soundtrigger, *tp_name_separator;

/* --- the item lists (tp_took, tp_pickup, tp_point) ------------------------ */

static const char *const item_names[] = {"quad", "pent", "ring", "suit", "ra", "ya", "ga", "mh", "lg", "rl", "gl",
                                         "sng", "ng", "ssg", "pack", "cells", "rockets", "nails", "shells", "flag",
                                         "armor", "weapon", "powerup", "teammate", "enemy", "eyes", "sentry", "disp",
                                         "runes", NULL};

typedef struct {
	const char *name;
	const char *defaults;
	char list[512];
} itemlist_t;

static itemlist_t lists[] = {
	{"tp_took", "quad pent ring ra ya mh lg rl flag", ""},
	{"tp_pickup", "quad pent ring suit ra ya ga mh lg rl gl sng ng ssg pack rockets flag", ""},
	{"tp_point", "quad pent ring suit ra ya ga mh lg rl gl sng pack rockets flag teammate enemy eyes", ""},
};

static void ItemList_f(int argc, const char **argv)
{
	itemlist_t *l = NULL;
	for (size_t i = 0; i < BW_ARRAY_COUNT(lists); i++)
		if (!Str_ICmp(argv[0], lists[i].name))
			l = &lists[i];
	if (!l)
		return;
	if (argc < 2) {
		Con_Printf("%s\n", l->list[0] ? l->list : "none");
		return;
	}
	if (!Str_ICmp(argv[1], "default")) {
		Str_Copy(l->list, l->defaults, sizeof(l->list));
		return;
	}
	if (!Str_ICmp(argv[1], "clear") || !Str_ICmp(argv[1], "none")) {
		l->list[0] = 0;
		return;
	}
	/* "+name" adds, "-name" removes, plain names replace the list */
	if (argv[1][0] != '+' && argv[1][0] != '-')
		l->list[0] = 0;
	for (int i = 1; i < argc; i++) {
		const char *name = argv[i][0] == '+' || argv[i][0] == '-' ? argv[i] + 1 : argv[i];
		bool known = false;
		for (int k = 0; item_names[k]; k++)
			known |= !Str_ICmp(item_names[k], name);
		if (!known) {
			Con_Printf("unknown item: %s\n", name);
			continue;
		}
		if (argv[i][0] == '-') {
			char out[512] = "", word[64];
			const char *p = l->list;
			while (sscanf(p, "%63s", word) == 1) {
				if (Str_ICmp(word, name))
					str_append(out, sizeof(out), "%s%s", out[0] ? " " : "", word);
				p = strstr(p, word) + strlen(word);
			}
			Str_Copy(l->list, out, sizeof(l->list));
		} else if (!strstr(l->list, name)) {
			str_append(l->list, sizeof(l->list), "%s%s", l->list[0] ? " " : "", name);
		}
	}
}

static bool in_list(int which, const char *item)
{
	char word[64];
	const char *p = lists[which].list;
	while (sscanf(p, "%63s", word) == 1) {
		if (!Str_ICmp(word, item))
			return true;
		p = strstr(p, word) + strlen(word);
	}
	return false;
}

/* --- locations ------------------------------------------------------------ */

typedef struct {
	float org[3];
	char name[64];
} loc_t;

static loc_t *locs;
static int numlocs;
static char locs_map[64];

static void free_locs(void)
{
	Mem_Free(locs);
	locs = NULL;
	numlocs = 0;
}

/* $loc_name_ra and friends in loc names */
static void expand_loc_name(const char *in, char *out, size_t size)
{
	size_t o = 0;
	for (const char *p = in; *p && o + 1 < size;) {
		if (*p == '$') {
			char name[64];
			size_t n = 0;
			cvar_t *v;
			while (isalnum((unsigned char)p[1 + n]) || p[1 + n] == '_')
				n++;
			/* $loc_name_ra, or the short $ra */
			snprintf(name, sizeof(name), "%.*s", (int)n, p + 1);
			if (n && !(v = Cvar_Find(name))) {
				snprintf(name, sizeof(name), "loc_name_%.*s", (int)n, p + 1);
				v = Cvar_Find(name);
			}
			if (n && v) {
				o += Str_Copy(out + o, v->string, size - o);
				if (o >= size)
					o = size - 1;
				p += n + 1;
				continue;
			}
		}
		out[o++] = *p++;
	}
	out[o] = 0;
}

/* locs/<name>.loc: "x y z name" a line, coordinates in eighths of a unit */
static bool load_locs(const char *name, bool quiet)
{
	char path[256], *data, *p;
	size_t len;
	snprintf(path, sizeof(path), "locs/%s%s", name, strstr(name, ".loc") ? "" : ".loc");
	if (!(data = FS_LoadFile(path, &len))) {
		if (!quiet)
			Con_Printf("Could not load %s\n", path);
		return false;
	}
	free_locs();
	for (p = data; *p;) {
		char *e = strchr(p, '\n'), raw[128];
		float x, y, z;
		int n = 0;
		if (e)
			*e = 0;
		if (sscanf(p, "%f %f %f %n", &x, &y, &z, &n) == 3 && n > 0) {
			loc_t *l;
			if (!(numlocs & 63))
				locs = Mem_Realloc(locs, sizeof(loc_t) * (size_t)(numlocs + 64));
			l = &locs[numlocs++];
			l->org[0] = x / 8, l->org[1] = y / 8, l->org[2] = z / 8;
			Str_Copy(raw, p + n, sizeof(raw));
			raw[strcspn(raw, "\r")] = 0;
			expand_loc_name(raw, l->name, sizeof(l->name));
		}
		if (!e)
			break;
		p = e + 1;
	}
	Mem_Free(data);
	return true;
}

static void Loadloc_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("loadloc <filename> : load a loc file\n");
		return;
	}
	load_locs(argv[1], false);
}

static const char *location_of(const float *org)
{
	int best = -1;
	float bestd = 0;
	for (int i = 0; i < numlocs; i++) {
		float d = 0;
		for (int k = 0; k < 3; k++)
			d += (locs[i].org[k] - org[k]) * (locs[i].org[k] - org[k]);
		if (best < 0 || d < bestd)
			best = i, bestd = d;
	}
	return best >= 0 ? locs[best].name : "someplace";
}

static void view_origin(float org[3], float fwd[3])
{
	float f[3];
	R_MainView(org, fwd ? fwd : f);
}

static const char *my_location(void)
{
	float org[3];
	view_origin(org, NULL);
	return location_of(org);
}

/* --- what happened lately (watching the stats change) ------------------------ */

static struct {
	int items, health;
	float ammo[4];
	char took[64], tookloc[64];
	char deathloc[64], lastloc[64], curloc[64];
	char droploc[64], droppedweapon[32];
	double droptime;
	char lastpowerup[32];
	char triggermatch[256];
	bool have;
} tp;

static const char *cvar_str(const char *name) { return Cvar_String(name); }

static const char *weapon_name(int num) { return num >= 1 && num <= 8 ? cvar_str(weapons[num].cvar) : cvar_str("tp_name_none"); }

static int weapon_num_for_bit(int bit)
{
	for (int i = 1; i <= 8; i++)
		if (weapons[i].bit == bit)
			return i;
	return 0;
}

/* the item a stat change means was picked up, as a tp item name */
static const char *pickup_name(int olditems, int items, const float *oldammo, const float *ammo, int health, int oldhealth)
{
	int gained = items & ~olditems;
	if (gained & IT_QUAD) return "quad";
	if (gained & IT_INVULNERABILITY) return "pent";
	if (gained & IT_INVISIBILITY) return "ring";
	if (gained & IT_SUIT) return "suit";
	if (gained & IT_ARMOR3) return "ra";
	if (gained & IT_ARMOR2) return "ya";
	if (gained & IT_ARMOR1) return "ga";
	if (gained & IT_SUPERHEALTH) return "mh";
	if (gained & IT_LIGHTNING) return "lg";
	if (gained & IT_ROCKET_LAUNCHER) return "rl";
	if (gained & IT_GRENADE_LAUNCHER) return "gl";
	if (gained & IT_SUPER_NAILGUN) return "sng";
	if (gained & IT_NAILGUN) return "ng";
	if (gained & IT_SUPER_SHOTGUN) return "ssg";
	{
		int more = 0;
		const char *which = NULL;
		static const char *const names[4] = {"shells", "nails", "rockets", "cells"};
		for (int i = 0; i < 4; i++)
			if (ammo[i] > oldammo[i]) {
				more++;
				which = names[i];
			}
		if (more > 1)
			return "pack";
		if (which)
			return which;
	}
	if (health > oldhealth && oldhealth > 0)
		return "health";
	return NULL;
}

static const char *item_display(const char *item)
{
	char cv[64];
	cvar_t *v;
	snprintf(cv, sizeof(cv), "tp_name_%s", item);
	v = Cvar_Find(cv);
	return v ? v->string : item;
}

/* called each frame: notices pickups, deaths and drops */
void TP_Frame(void)
{
	int items, health;
	float ammo[4];
	const char *loc;
	if (cl.state != ca_active) {
		tp.have = false;
		return;
	}
	items = CL_StatInt(ST_ITEMS);
	health = (int)CL_StatFloat(ST_HEALTH);
	for (int i = 0; i < 4; i++)
		ammo[i] = CL_StatFloat(ST_SHELLS + i);
	loc = my_location();
	if (strcmp(loc, tp.curloc)) {
		Str_Copy(tp.lastloc, tp.curloc[0] ? tp.curloc : loc, sizeof(tp.lastloc));
		Str_Copy(tp.curloc, loc, sizeof(tp.curloc));
	}
	if (tp.have) {
		const char *got = pickup_name(tp.items, items, tp.ammo, ammo, health, tp.health);
		if (got) {
			if (in_list(0, got)) {
				Str_Copy(tp.took, item_display(got), sizeof(tp.took));
				Str_Copy(tp.tookloc, loc, sizeof(tp.tookloc));
			}
			if (!strcmp(got, "quad") || !strcmp(got, "pent") || !strcmp(got, "ring") || !strcmp(got, "suit"))
				Str_Copy(tp.lastpowerup, item_display(got), sizeof(tp.lastpowerup));
		}
		if (health <= 0 && tp.health > 0) {
			Str_Copy(tp.deathloc, loc, sizeof(tp.deathloc));
			/* what the death left behind: the best weapon */
			for (int i = 8; i >= 3; i--)
				if (tp.items & weapons[i].bit) {
					Str_Copy(tp.droppedweapon, weapon_name(i), sizeof(tp.droppedweapon));
					break;
				}
			Str_Copy(tp.droploc, loc, sizeof(tp.droploc));
			tp.droptime = cl.realtime;
		}
	}
	tp.items = items;
	tp.health = health;
	memcpy(tp.ammo, ammo, sizeof(ammo));
	tp.have = true;
}

/* a new map: its locations */
void TP_NewMap(void)
{
	if (!Str_ICmp(locs_map, cl.mapname))
		return;
	Str_Copy(locs_map, cl.mapname, sizeof(locs_map));
	free_locs();
	memset(&tp, 0, sizeof(tp));
	if (tp_loadlocs->ivalue && cl.mapname[0])
		load_locs(cl.mapname, true);
}

/* --- macros ------------------------------------------------------------------- */

static char macro_buf[1024];

static void join(char *out, size_t size, const char *word)
{
	if (out[0])
		str_append(out, size, "%s", tp_name_separator->string[0] ? tp_name_separator->string : "/");
	str_append(out, size, "%s", word);
}

static int active_weapon_num(void) { return weapon_num_for_bit(CL_StatInt(ST_ACTIVEWEAPON)); }

static int best_weapon(void)
{
	int items = CL_StatInt(ST_ITEMS);
	for (const char *p = tp_weapon_order->string; *p; p++) {
		int n = *p - '0';
		if (n < 1 || n > 8 || !(items & weapons[n].bit))
			continue;
		if (weapons[n].ammo < 0 || CL_StatFloat(weapons[n].ammo) > 0)
			return n;
	}
	return 0;
}

static const char *armor_type(void)
{
	int items = CL_StatInt(ST_ITEMS);
	if (items & IT_ARMOR3) return cvar_str("tp_name_armortype_ra");
	if (items & IT_ARMOR2) return cvar_str("tp_name_armortype_ya");
	if (items & IT_ARMOR1) return cvar_str("tp_name_armortype_ga");
	return cvar_str("tp_name_none");
}

static void powerups(char *out, size_t size, bool colored, bool shorter)
{
	int items = CL_StatInt(ST_ITEMS);
	out[0] = 0;
	if (items & IT_QUAD)
		join(out, size, colored ? (shorter ? "^4q^7" : va("^4%s^7", cvar_str("tp_name_quad"))) : cvar_str("tp_name_quad"));
	if (items & IT_INVULNERABILITY)
		join(out, size, colored ? (shorter ? "^1p^7" : va("^1%s^7", cvar_str("tp_name_pent"))) : cvar_str("tp_name_pent"));
	if (items & IT_INVISIBILITY)
		join(out, size, colored ? (shorter ? "^3r^7" : va("^3%s^7", cvar_str("tp_name_ring"))) : cvar_str("tp_name_ring"));
	if (items & IT_SUIT)
		join(out, size, colored ? (shorter ? "^2s^7" : va("^2%s^7", cvar_str("tp_name_suit"))) : cvar_str("tp_name_suit"));
}

static void need(char *out, size_t size)
{
	int items = CL_StatInt(ST_ITEMS), armor = (int)CL_StatFloat(ST_ARMOR), health = (int)CL_StatFloat(ST_HEALTH);
	bool weapon = false;
	out[0] = 0;
	if ((items & IT_ARMOR3) ? armor < Cvar_Value("tp_need_ra")
	    : (items & IT_ARMOR2) ? armor < Cvar_Value("tp_need_ya")
	                            : armor < Cvar_Value("tp_need_ga"))
		join(out, size, cvar_str("tp_name_armor"));
	if (health < Cvar_Value("tp_need_health"))
		join(out, size, cvar_str("tp_name_health"));
	for (const char *p = Cvar_String("tp_need_weapon"); *p; p++) {
		int n = *p - '0';
		if (n >= 1 && n <= 8 && (items & weapons[n].bit))
			weapon = true;
	}
	if (!weapon)
		join(out, size, cvar_str("tp_name_weapon"));
	if ((items & (IT_SHOTGUN | IT_SUPER_SHOTGUN)) && CL_StatFloat(ST_SHELLS) < Cvar_Value("tp_need_shells"))
		join(out, size, cvar_str("tp_name_shells"));
	if ((items & (IT_NAILGUN | IT_SUPER_NAILGUN)) && CL_StatFloat(ST_NAILS) < Cvar_Value("tp_need_nails"))
		join(out, size, cvar_str("tp_name_nails"));
	if ((items & (IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER)) && CL_StatFloat(ST_ROCKETS) < Cvar_Value("tp_need_rockets"))
		join(out, size, cvar_str("tp_name_rockets"));
	if ((items & IT_LIGHTNING) && CL_StatFloat(ST_CELLS) < Cvar_Value("tp_need_cells"))
		join(out, size, cvar_str("tp_name_cells"));
	if (!out[0])
		Str_Copy(out, cvar_str("tp_name_nothing"), size);
}

/* what the crosshair is on: where it hits the world */
static const char *point_location(void)
{
	float org[3], fwd[3], end[3];
	bsptrace_t tr;
	static const float zero[3] = {0, 0, 0};
	if (!cl.world)
		return "someplace";
	view_origin(org, fwd);
	for (int k = 0; k < 3; k++)
		end[k] = org[k] + fwd[k] * 8192;
	BSP_TraceModel(cl.world, 0, zero, org, zero, zero, end, 1, &tr);
	return location_of(tr.endpos);
}

static const char *player_key(const char *key)
{
	static char v[256];
	return cl.state >= ca_loading ? CL_PlayerKey(cl.playerslot, key, v, sizeof(v)) : "";
}

static const char *match_type(void)
{
	int players = 0, teams = 0;
	char seen[16][32], team[32], name[64];
	for (int i = 0; i < CL_MAX_PLAYERS; i++) {
		bool known = false;
		if (!CL_PlayerKey(i, "name", name, sizeof(name))[0])
			continue;
		players++;
		CL_PlayerKey(i, "team", team, sizeof(team));
		for (int k = 0; k < teams; k++)
			known |= !strcmp(seen[k], team);
		if (!known && teams < 16)
			Str_Copy(seen[teams++], team, sizeof(seen[0]));
	}
	if (cl.state < ca_loading)
		return "-";
	if (!atoi(skey("teamplay")))
		return players == 2 ? "duel" : "ffa";
	if (players <= teams * 2)
		return "2on2";
	if (players <= teams * 3)
		return "3on3";
	return "4on4";
}

static const char *macro(const char *name)
{
	char *o = macro_buf;
	size_t size = sizeof(macro_buf);
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	o[0] = 0;
	if (!strcmp(name, "qt")) return "\"";
	if (!strcmp(name, "random")) { snprintf(o, size, "%d", rand()); return o; }
	if (!strcmp(name, "time")) { strftime(o, size, "%H:%M", t); return o; }
	if (!strcmp(name, "date")) { strftime(o, size, "%Y-%m-%d", t); return o; }
	if (!strcmp(name, "ukdate")) { strftime(o, size, "%d.%m.%Y", t); return o; }
	if (!strcmp(name, "usdate")) { strftime(o, size, "%m.%d.%Y", t); return o; }
	if (!strcmp(name, "version")) return "Bulwark " BULWARK_VERSION;
	if (!strcmp(name, "dedicated")) return "0";
	if (!strcmp(name, "gamedir")) return FS_GameDir();
	if (!strcmp(name, "demoplayback")) return !cl.demoplayback ? "0" : CL_DemoMultiview() ? "mvdplayback" : "1";
	if (!strcmp(name, "connectiontype"))
		return cl.state < ca_loading ? "disconnected" : cl.demoplayback ? "demo" : atoi(player_key("*spectator")) ? "spectator" : "player";
	if (!strcmp(name, "latency") || !strcmp(name, "ping")) { snprintf(o, size, "%d", atoi(player_key("ping"))); return o; }
	if (!strcmp(name, "team")) return player_key("team");
	if (!strcmp(name, "tf_skin")) return player_key("skin");
	if (!strcmp(name, "matchname")) return skey("matchname");
	if (!strcmp(name, "matchtype")) return match_type();
	if (!strcmp(name, "matchstatus")) {
		const char *s = skey("status");
		return s[0] ? s : cl.state == ca_active ? "playing" : "disconnected";
	}
	if (!strcmp(name, "location")) return my_location();
	if (!strcmp(name, "lastloc")) return tp.lastloc[0] ? tp.lastloc : my_location();
	if (!strcmp(name, "deathloc")) return tp.deathloc[0] ? tp.deathloc : my_location();
	if (!strcmp(name, "droploc")) return tp.droploc[0] ? tp.droploc : my_location();
	if (!strcmp(name, "droptime")) { snprintf(o, size, "%d", tp.droptime > 0 ? (int)(cl.realtime - tp.droptime) : 0); return o; }
	if (!strcmp(name, "droppedweapon")) return tp.droppedweapon[0] ? tp.droppedweapon : cvar_str("tp_name_none");
	if (!strcmp(name, "took")) return tp.took[0] ? tp.took : cvar_str("tp_name_nothing");
	if (!strcmp(name, "tookloc")) return tp.tookloc[0] ? tp.tookloc : my_location();
	if (!strcmp(name, "tookatloc")) {
		snprintf(o, size, "%s %s %s", tp.took[0] ? tp.took : cvar_str("tp_name_nothing"), cvar_str("tp_name_at"),
		         tp.tookloc[0] ? tp.tookloc : my_location());
		return o;
	}
	if (!strcmp(name, "point")) return cvar_str("tp_name_nothing");
	if (!strcmp(name, "pointloc")) return point_location();
	if (!strcmp(name, "pointatloc")) {
		snprintf(o, size, "%s %s %s", cvar_str("tp_name_nothing"), cvar_str("tp_name_at"), point_location());
		return o;
	}
	if (!strcmp(name, "lastpowerup")) return tp.lastpowerup[0] ? tp.lastpowerup : cvar_str("tp_name_none");
	if (!strcmp(name, "triggermatch")) return tp.triggermatch;
	if (!strcmp(name, "health") || !strcmp(name, "chealth")) {
		int h = (int)CL_StatFloat(ST_HEALTH);
		if (name[0] == 'c')
			snprintf(o, size, "%s%d^7", h < 50 ? "^1" : h < 100 ? "^3" : "^2", h);
		else
			snprintf(o, size, "%d", h);
		return o;
	}
	if (!strcmp(name, "armor")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_ARMOR)); return o; }
	if (!strcmp(name, "armortype")) return armor_type();
	if (!strcmp(name, "colored_armor")) {
		int items = CL_StatInt(ST_ITEMS);
		snprintf(o, size, "%s%d^7", items & IT_ARMOR3 ? "^1" : items & IT_ARMOR2 ? "^3" : items & IT_ARMOR1 ? "^2" : "^7",
		         (int)CL_StatFloat(ST_ARMOR));
		return o;
	}
	if (!strcmp(name, "shells")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_SHELLS)); return o; }
	if (!strcmp(name, "nails")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_NAILS)); return o; }
	if (!strcmp(name, "rockets")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_ROCKETS)); return o; }
	if (!strcmp(name, "cells")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_CELLS)); return o; }
	if (!strcmp(name, "ammo")) { snprintf(o, size, "%d", (int)CL_StatFloat(ST_AMMO)); return o; }
	if (!strcmp(name, "weaponnum")) { snprintf(o, size, "%d", active_weapon_num()); return o; }
	if (!strcmp(name, "weapon")) return active_weapon_num() ? weapon_name(active_weapon_num()) : "";
	if (!strcmp(name, "weapons")) {
		int items = CL_StatInt(ST_ITEMS);
		for (int i = 8; i >= 2; i--)
			if (items & weapons[i].bit)
				join(o, size, weapon_name(i));
		if (!o[0])
			Str_Copy(o, cvar_str("tp_name_none"), size);
		return o;
	}
	if (!strcmp(name, "bestweapon")) return best_weapon() ? weapon_name(best_weapon()) : cvar_str("tp_name_none");
	if (!strcmp(name, "bestammo")) {
		int b = best_weapon();
		snprintf(o, size, "%d", b && weapons[b].ammo >= 0 ? (int)CL_StatFloat(weapons[b].ammo) : 0);
		return o;
	}
	if (!strcmp(name, "powerups")) { powerups(o, size, false, false); return o; }
	if (!strcmp(name, "colored_powerups")) { powerups(o, size, true, false); return o; }
	if (!strcmp(name, "colored_short_powerups")) { powerups(o, size, true, true); return o; }
	if (!strcmp(name, "need")) { need(o, size); return o; }
	if (!strcmp(name, "ledstatus") || !strcmp(name, "ledpoint")) {
		int h = (int)CL_StatFloat(ST_HEALTH);
		return h <= 0 ? "^1@^7" : h < 50 ? "^3@^7" : "^2@^7";
	}
	return R_EditLightsMacro(name);
}

static const char *const macro_names[] = {
	"random", "time", "ukdate", "usdate", "date", "version", "qt", "dedicated", "r_editlights_current_origin",
	"r_editlights_current_angles", "r_editlights_current_color", "r_editlights_current_radius",
	"r_editlights_current_corona", "r_editlights_current_coronasize", "r_editlights_current_style",
	"r_editlights_current_shadows", "r_editlights_current_cubemap", "r_editlights_current_ambient",
	"r_editlights_current_diffuse", "r_editlights_current_specular", "r_editlights_current_normalmode",
	"r_editlights_current_realtimemode", "latency", "location", "health", "armortype", "armor", "shells", "nails",
	"rockets", "cells", "weaponnum", "weapons", "weapon", "ammo", "bestweapon", "bestammo", "powerups",
	"droppedweapon", "tf_skin", "team", "deathloc", "tookatloc", "tookloc", "took", "connectiontype", "demoplayback",
	"point", "pointatloc", "pointloc", "matchname", "matchtype", "need", "ledstatus", "ledpoint", "droploc",
	"droptime", "matchstatus", "triggermatch", "chealth", "ping", "colored_armor", "colored_powerups",
	"colored_short_powerups", "lastloc", "lastpowerup", "gamedir", NULL};

static const char *macro_provider(const char *name)
{
	for (int i = 0; macro_names[i]; i++)
		if (!strcmp(macro_names[i], name))
			return macro(name);
	return NULL;
}

static void Macrolist_f(int argc, const char **argv)
{
	for (int i = 0; macro_names[i]; i++)
		Con_Printf("$%s\n", macro_names[i]);
}

/* %-codes in say and say_team: %h health, %a armor, %A armor type, %l
 * location, %d death location, %w weapon:ammo, %b best weapon:ammo, %p
 * powerups, %u what you need, %t what you took, %x where you point, %r
 * last location, %S skin, %T team, %% a percent sign */
void TP_ParseSay(const char *in, char *out, size_t size)
{
	size_t o = 0;
	if (!cl_parseSay->ivalue) {
		Str_Copy(out, in, size);
		return;
	}
	for (const char *p = in; *p && o + 1 < size; p++) {
		const char *rep = NULL;
		char tmp[128];
		if (*p != '%' || !p[1]) {
			out[o++] = *p;
			continue;
		}
		switch (p[1]) {
		case '%': rep = "%"; break;
		case 'h': rep = macro("health"); break;
		case 'a': rep = macro("armor"); break;
		case 'A': rep = armor_type(); break;
		case 'l': rep = my_location(); break;
		case 'd': rep = macro("deathloc"); break;
		case 'r': rep = macro("lastloc"); break;
		case 'p': rep = macro("powerups"); break;
		case 'u': rep = macro("need"); break;
		case 't': rep = macro("took"); break;
		case 'x': rep = point_location(); break;
		case 'S': rep = player_key("skin"); break;
		case 'T': rep = player_key("team"); break;
		case 'w':
			snprintf(tmp, sizeof(tmp), "%s:%d", macro("weapon"), (int)CL_StatFloat(ST_AMMO));
			rep = tmp;
			break;
		case 'b': {
			int b = best_weapon();
			snprintf(tmp, sizeof(tmp), "%s:%d", b ? weapon_name(b) : cvar_str("tp_name_none"),
			         b && weapons[b].ammo >= 0 ? (int)CL_StatFloat(weapons[b].ammo) : 0);
			rep = tmp;
			break;
		}
		default: break;
		}
		if (!rep) {
			out[o++] = *p;
			continue;
		}
		{
			char copy[512];
			Str_Copy(copy, rep, sizeof(copy));
			o += Str_Copy(out + o, copy, size - o);
			if (o >= size)
				o = size - 1;
		}
		p++;
	}
	out[o] = 0;
}

/* --- message triggers ------------------------------------------------------------ */

typedef struct {
	char name[64];
	char match[256];
	int level; /* -1: any */
} trigger_t;

static trigger_t *triggers;
static int numtriggers;

static void MsgTrigger_f(int argc, const char **argv)
{
	trigger_t *t = NULL;
	if (argc < 2) {
		if (!numtriggers)
			Con_Printf("no triggers defined\n");
		for (int i = 0; i < numtriggers; i++)
			Con_Printf("%s : \"%s\"\n", triggers[i].name, triggers[i].match);
		return;
	}
	for (int i = 0; i < numtriggers; i++)
		if (!Str_ICmp(triggers[i].name, argv[1]))
			t = &triggers[i];
	if (argc < 3) {
		if (t)
			Con_Printf("%s: \"%s\"\n", t->name, t->match);
		else
			Con_Printf("trigger \"%s\" not found\n", argv[1]);
		return;
	}
	if (!t) {
		triggers = Mem_Realloc(triggers, sizeof(trigger_t) * (size_t)(numtriggers + 1));
		t = &triggers[numtriggers++];
		memset(t, 0, sizeof(*t));
		Str_Copy(t->name, argv[1], sizeof(t->name));
	}
	Str_Copy(t->match, argv[2], sizeof(t->match));
	t->level = -1;
	/* "-l <level>": only prints at that level (3 chat, 2 high...) */
	for (int i = 3; i + 1 < argc; i++)
		if (!strcmp(argv[i], "-l"))
			t->level = atoi(argv[i + 1]);
}

/* a print from the server: runs the triggers that match it, and plays
 * sounds a teammate asked for with tp_soundtrigger ("~sound.wav") */
void TP_CheckTriggers(const char *text, int level)
{
	if (cl_triggers->ivalue) {
		for (int i = 0; i < numtriggers; i++) {
			const trigger_t *t = &triggers[i];
			if (!t->match[0] || (t->level >= 0 && t->level != level) || !strstr(text, t->match))
				continue;
			Str_Copy(tp.triggermatch, text, sizeof(tp.triggermatch));
			tp.triggermatch[strcspn(tp.triggermatch, "\n")] = 0;
			if (Cmd_Exists(t->name) || Cmd_AliasText(t->name))
				Cbuf_AddText(va("%s\n", t->name));
		}
	}
	if (level == 3 && tp_soundtrigger->string[0]) {
		const char *s = strrchr(text, tp_soundtrigger->string[0]);
		if (s && s[1] && s[1] != ' ') {
			char snd[128];
			Str_Copy(snd, s + 1, sizeof(snd));
			snd[strcspn(snd, " \n\r")] = 0;
			if (FS_SafeName(snd) && snd[0]) {
				if (!strchr(snd, '.'))
					str_append(snd, sizeof(snd), ".wav");
				S_StartSound(0, -1, snd, NULL, 1, 0, 100, 0);
			}
		}
	}
}

/* --- inventory (Quake 2's), seats ------------------------------------------------ */

static void Inventory_f(int argc, const char **argv)
{
	if (cl.state < ca_loading || cl.demoplayback) {
		Con_Printf("Not connected.\n");
		return;
	}
	CL_SendStringCmd(args_from(argc, argv, 0));
}

/* p <seat> <command>, p1..p4 <command>: a command for one split screen
 * player (seats count from 1 here). The command runs between two
 * _seatcontext commands, so its aliases and the commands it sends to the
 * server or the client game are that player's (in_cmdseat). */
static void Seat_f(int argc, const char **argv)
{
	int seat, first = 1;
	const char *line;
	if (!Str_ICmp(argv[0], "p")) {
		if (argc < 3) {
			Con_Printf("p <seat> <command>\n");
			return;
		}
		seat = atoi(argv[1]);
		first = 2;
	} else {
		seat = argv[0][1] - '0';
		if (argc < 2) {
			Con_Printf("%s <command>\n", argv[0]);
			return;
		}
	}
	line = args_from(argc, argv, first);
	if (seat < 1 || seat > MAX_SEATS) {
		Con_Printf("there are seats 1 to %d\n", MAX_SEATS);
		return;
	}
	if (seat > 1 && !CL_SeatPlaying(seat - 1) && cl.state == ca_active) {
		Con_DPrintf("seat %d isn't playing\n", seat);
		return;
	}
	Cbuf_InsertText(va("_seatcontext %d\n%s\n_seatcontext %d\n", seat - 1, line, in_cmdseat));
}

static void SeatContext_f(int argc, const char **argv)
{
	int k = argc > 1 ? atoi(argv[1]) : 0;
	in_cmdseat = k >= 0 && k < MAX_SEATS ? k : 0;
}

/* +p/-p: the next commands are for the next seat (FTE's splitscreen
 * toggle); seats are chosen with p1..p4 here, so it does nothing */
static void SeatToggle_f(int argc, const char **argv) { (void)argc, (void)argv; }

void TP_Init(void)
{
	static const char *const names[][2] = {
		{"tp_name_armor", "armor"}, {"tp_name_armortype_ga", "g"}, {"tp_name_armortype_ra", "r"},
		{"tp_name_armortype_ya", "y"}, {"tp_name_at", "at"}, {"tp_name_axe", "axe"}, {"tp_name_backpack", "pack"},
		{"tp_name_cells", "cells"}, {"tp_name_disp", "dispenser"}, {"tp_name_enemy", "enemy"},
		{"tp_name_eyes", "eyes"}, {"tp_name_flag", "flag"}, {"tp_name_ga", "ga"}, {"tp_name_gl", "gl"},
		{"tp_name_health", "health"}, {"tp_name_lg", "lg"}, {"tp_name_mh", "mega"}, {"tp_name_nails", "nails"},
		{"tp_name_ng", "ng"}, {"tp_name_none", ""}, {"tp_name_nothing", "nothing"}, {"tp_name_pent", "pent"},
		{"tp_name_pented", "pented"}, {"tp_name_quad", "quad"}, {"tp_name_quaded", "quaded"}, {"tp_name_ra", "ra"},
		{"tp_name_ring", "ring"}, {"tp_name_rl", "rl"}, {"tp_name_rockets", "rockets"},
		{"tp_name_rune_1", "resistance rune"}, {"tp_name_rune_2", "strength rune"}, {"tp_name_rune_3", "haste rune"},
		{"tp_name_rune_4", "regeneration rune"}, {"tp_name_sentry", "sentry gun"}, {"tp_name_sg", "sg"},
		{"tp_name_shells", "shells"}, {"tp_name_sng", "sng"}, {"tp_name_ssg", "ssg"}, {"tp_name_suit", "suit"},
		{"tp_name_teammate", ""}, {"tp_name_weapon", "weapon"}, {"tp_name_ya", "ya"},
		{"tp_need_cells", "20"}, {"tp_need_ga", "50"}, {"tp_need_health", "50"}, {"tp_need_nails", "40"},
		{"tp_need_ra", "50"}, {"tp_need_rl", "1"}, {"tp_need_rockets", "5"}, {"tp_need_shells", "10"},
		{"tp_need_weapon", "35687"}, {"tp_need_ya", "50"}, {"tp_autostatus", ""}, {"tp_forceTriggers", "0"},
		{"tp_disputablemacros", "1"}, {"loc_name_ga", "ga"}, {"loc_name_gl", "gl"}, {"loc_name_lg", "lg"},
		{"loc_name_mh", "mh"}, {"loc_name_ng", "ng"}, {"loc_name_pent", "pent"}, {"loc_name_quad", "quad"},
		{"loc_name_ra", "ra"}, {"loc_name_ring", "ring"}, {"loc_name_rl", "rl"}, {"loc_name_separator", "-"},
		{"loc_name_sng", "sng"}, {"loc_name_ssg", "ssg"}, {"loc_name_suit", "suit"}, {"loc_name_ya", "ya"},
		{NULL, NULL}};
	for (int i = 0; names[i][0]; i++)
		Cvar_Get(names[i][0], names[i][1], CVAR_ARCHIVE, "Teamplay name or threshold");
	cl_parseSay = Cvar_Get("cl_parseSay", "1", CVAR_ARCHIVE, "Replace %-codes (%h health, %l location...) in say and say_team");
	cl_triggers = Cvar_Get("cl_triggers", "1", CVAR_ARCHIVE, "Run msg_trigger aliases");
	tp_loadlocs = Cvar_Get("tp_loadlocs", "1", CVAR_ARCHIVE, "Load locs/<map>.loc with each map");
	tp_weapon_order = Cvar_Get("tp_weapon_order", "78654321", CVAR_ARCHIVE, "Best weapon first, for $bestweapon");
	tp_soundtrigger = Cvar_Get("tp_soundtrigger", "~", CVAR_ARCHIVE, "Chat ending in this and a file name plays the sound");
	tp_name_separator = Cvar_Get("tp_name_separator", "/", CVAR_ARCHIVE, "Between names in macros that list several");
	for (size_t i = 0; i < BW_ARRAY_COUNT(lists); i++)
		Str_Copy(lists[i].list, lists[i].defaults, sizeof(lists[i].list));
	Cmd_SetMacroProvider(macro_provider);
	Cmd_AddD("tp_took", ItemList_f, "tp_took [items|default|clear]: what $took reports picking up");
	Cmd_AddD("tp_pickup", ItemList_f, "tp_pickup [items|default|clear]: what counts as a pickup");
	Cmd_AddD("tp_point", ItemList_f, "tp_point [items|default|clear]: what $point reports");
	Cmd_AddD("macrolist", Macrolist_f, "List the $macros");
	Cmd_AddD("loadloc", Loadloc_f, "loadloc <file>: load locs/<file>.loc, the map's place names");
	Cmd_AddD("msg_trigger", MsgTrigger_f, "msg_trigger [name [text [-l level]]]: run alias <name> when a message contains the text");
	Cmd_AddD("invuse", Inventory_f, "Use the selected inventory item");
	Cmd_AddD("invnext", Inventory_f, "Select the next inventory item");
	Cmd_AddD("invprev", Inventory_f, "Select the previous inventory item");
	Cmd_AddD("invleft", Inventory_f, "Select the inventory item to the left");
	Cmd_AddD("invright", Inventory_f, "Select the inventory item to the right");
	Cmd_AddD("p", Seat_f, "p <seat> <command>: run a command for one splitscreen seat");
	Cmd_AddD("p1", Seat_f, "p1 <command>: run a command for seat 1");
	Cmd_AddD("p2", Seat_f, "p2 <command>: run a command for seat 2");
	Cmd_AddD("p3", Seat_f, "p3 <command>: run a command for seat 3");
	Cmd_AddD("p4", Seat_f, "p4 <command>: run a command for seat 4");
	Cmd_AddD("_seatcontext", SeatContext_f, NULL);
	Cmd_AddD("+p", SeatToggle_f, "Following input is for the next seat");
	Cmd_AddD("-p", SeatToggle_f, NULL);
}
