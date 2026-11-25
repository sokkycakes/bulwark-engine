/* Bulwark engine - Local Play: split screen games on this computer, started
 * from the game's title. The Local Play page (layout/web/local.xml) shows
 * the four seats, lets controllers join with Start, picks the mode, map and
 * set length, and starts a listen server that every local player joins as a
 * seat of the one connection (docs/spec/splitscreen.md). A Versus game plays
 * the game's sets (Stiletto's arena director, docs/spec/setflow.md) with a
 * hero pick and rematch vote per player; leaving the game comes back here
 * with the same players. docs/spec/localplay.md
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../server/sv_local.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cvar_t *local_mode, *local_map, *local_maps, *local_firstto, *local_bots, *local_bot_profiles;

static struct {
	bool active;      /* the game was started here */
	bool was_in_game; /* ... and got as far as playing */
	int bots_pending; /* bots to add once the server runs */
	int bots_added;
	double bots_at;
} lp;

bool LP_Active(void) { return lp.active; }

int LP_Players(void)
{
	int n = 1;
	for (int k = 1; k < MAX_SEATS; k++)
		if (cl_seats[k].state != SEAT_FREE)
			n++;
	return n;
}

static bool versus(void) { return !Str_ICmp(local_mode->string, "versus"); }

/* what the current settings start, or why they can't */
static const char *start_problem(void)
{
	int n = LP_Players();
	if (versus() && n > 2)
		return "Versus is for two players";
	if (!versus() && n + local_bots->ivalue < 2)
		return "Add a player or a bot";
	return NULL;
}

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

/* "Keyboard + Pad 1" */
static void device_text(unsigned devs, char *out, size_t n)
{
	out[0] = 0;
	for (int d = 0; d <= MAX_SEATS; d++)
		if (devs & (1u << d))
			snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? " + " : "",
			         d ? va("Pad %d", d) : "Keyboard");
}

static bool map_ok(const char *m)
{
	size_t n = strlen(m);
	if (!n || n > 63)
		return false;
	for (const char *p = m; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
			return false;
	return FS_FileExists(va("maps/%s.bsp", m));
}

static void q_getlocal(strbuf_t *out)
{
	static const char *const states[] = {"free", "joining", "loading", "ready"};
	char devs[64], maps[512];
	const char *problem = start_problem();
	SB_AppendStr(out, "{\"seats\":[");
	for (int k = 0; k < MAX_SEATS; k++) {
		const clseat_t *s = &cl_seats[k];
		bool on = k == 0 || s->state != SEAT_FREE;
		device_text(s->device, devs, sizeof(devs));
		SB_Printf(out, "%s{\"seat\":%d,\"on\":%s,\"state\":\"%s\",\"name\":", k ? "," : "", k, on ? "true" : "false",
		          k == 0 ? "ready" : states[s->state]);
		json_str(out, k == 0 ? (Cvar_String("name")[0] ? Cvar_String("name") : "player") : on ? s->name : "");
		SB_AppendStr(out, ",\"devices\":");
		json_str(out, devs);
		SB_Printf(out, ",\"device\":%u}", s->device);
	}
	SB_AppendStr(out, "],\"maps\":[");
	Str_Copy(maps, local_maps->string, sizeof(maps));
	bool first = true;
	for (char *m = strtok(maps, " ,"); m; m = strtok(NULL, " ,")) {
		if (!map_ok(m))
			continue;
		if (!first)
			SB_AppendStr(out, ",");
		json_str(out, m);
		first = false;
	}
	SB_AppendStr(out, "],\"mode\":");
	json_str(out, versus() ? "versus" : "ffa");
	SB_AppendStr(out, ",\"map\":");
	json_str(out, local_map->string);
	SB_Printf(out, ",\"firstto\":%d,\"bots\":%d,\"players\":%d", local_firstto->ivalue, local_bots->ivalue, LP_Players());
	SB_AppendStr(out, ",\"split\":");
	json_str(out, Cvar_String("cl_splitscreen2"));
	SB_AppendStr(out, ",\"split3\":");
	json_str(out, Cvar_String("cl_splitscreen3"));
	SB_AppendStr(out, ",\"problem\":");
	json_str(out, problem ? problem : "");
	SB_AppendStr(out, "}");
}

/* a controller (or the keyboard) joins as a new local player. One that
 * the first player shares with another device (the keyboard and pad 1
 * start together) leaves the first player for its own seat, so two people
 * can play with one controller and the keyboard. */
static bool join(int device)
{
	int seat = CL_SeatForDevice(device);
	if (device < 0 || device > MAX_SEATS)
		return false;
	if (seat > 0)
		return false;
	if (seat == 0) {
		unsigned others = cl_seats[0].device & ~(1u << device);
		if (!others)
			return false; /* the first player's only device */
	}
	if (LP_Players() >= MAX_SEATS)
		return false;
	Cbuf_AddText(va("seat_add guest %d\n", device));
	return true;
}

/* a guest leaves; the keyboard and the first controller go back to the
 * first player if they had them */
static bool leave(int seat)
{
	unsigned devs;
	if (seat < 1 || seat >= MAX_SEATS || cl_seats[seat].state == SEAT_FREE)
		return false;
	devs = cl_seats[seat].device;
	Cbuf_AddText(va("seat_remove %d\n", seat + 1));
	for (int d = 0; d <= 1; d++)
		if (devs & (1u << d))
			Cbuf_AddText(va("seat_device 1 %d\n", d));
	return true;
}

static void remove_guests(void)
{
	for (int k = 1; k < MAX_SEATS; k++)
		leave(k);
}

static bool set_option(const char *key, const char *val)
{
	if (!strcmp(key, "mode")) {
		if (strcmp(val, "versus") && strcmp(val, "ffa"))
			return false;
		Cvar_Set("local_mode", val);
	} else if (!strcmp(key, "map")) {
		if (!map_ok(val))
			return false;
		Cvar_Set("local_map", val);
	} else if (!strcmp(key, "firstto")) {
		int n = atoi(val);
		if (n < 1 || n > 5)
			return false;
		Cvar_SetValue("local_firstto", (float)n);
	} else if (!strcmp(key, "bots")) {
		int n = atoi(val);
		if (n < 0 || n > 6)
			return false;
		Cvar_SetValue("local_bots", (float)n);
	} else if (!strcmp(key, "split")) {
		if (strcmp(val, "horizontal") && strcmp(val, "vertical"))
			return false;
		Cvar_Set("cl_splitscreen2", val);
	} else if (!strcmp(key, "split3")) {
		if (strcmp(val, "top") && strcmp(val, "bottom") && strcmp(val, "left") && strcmp(val, "right") &&
		    strcmp(val, "quad"))
			return false;
		Cvar_Set("cl_splitscreen3", val);
	} else {
		return false;
	}
	return true;
}

static bool start(void)
{
	const char *map = local_map->string;
	if (start_problem())
		return false;
	if (!map_ok(map)) {
		Con_Printf("local: no map %s\n", map);
		return false;
	}
	lp.active = true;
	lp.was_in_game = false;
	lp.bots_pending = versus() ? (LP_Players() < 2 ? 1 : 0) : local_bots->ivalue;
	lp.bots_at = 0;
	lp.bots_added = 0;
	/* a listen server for this computer only; the guests ask for their
	 * seats once it is up */
	Cbuf_AddText(va("set arena_setFirstTo %d\nset g_gametype %s\nset deathmatch 1\nset coop 0\nset teamplay 0\n"
	                "set g_nomotd 1\nset sv_background 0\nset sv_public 0\nmaxplayers 8\nmap %s\n",
	                versus() ? local_firstto->ivalue : 0, versus() ? "duel" : "deathmatch", map));
	Con_Printf("local: %s on %s, %d player%s\n", versus() ? "versus" : "free for all", map, LP_Players(),
	           LP_Players() > 1 ? "s" : "");
	return true;
}

/* local:<verb>[:<arg>[:<value>]] */
static void q_local(const char *a, strbuf_t *out)
{
	char verb[16], arg[64] = "";
	const char *rest = strchr(a, ':');
	size_t n = rest ? (size_t)(rest - a) : strlen(a);
	bool ok = false;
	snprintf(verb, sizeof(verb), "%.*s", (int)BW_MIN(n, sizeof(verb) - 1), a);
	if (rest)
		Str_Copy(arg, rest + 1, sizeof(arg));
	if (!strcmp(verb, "join")) {
		ok = join(atoi(arg));
	} else if (!strcmp(verb, "leave")) {
		ok = leave(atoi(arg));
	} else if (!strcmp(verb, "set")) {
		char *v = strchr(arg, ':');
		if (v) {
			*v++ = 0;
			ok = set_option(arg, v);
		}
	} else if (!strcmp(verb, "start")) {
		ok = start();
	} else if (!strcmp(verb, "back")) {
		remove_guests();
		Cbuf_AddText("menu_webcore title\n");
		ok = true;
	}
	SB_AppendStr(out, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

bool LP_Query(const char *req, strbuf_t *out)
{
	if (!strcmp(req, "getlocal")) {
		q_getlocal(out);
		return true;
	}
	if (!strncmp(req, "local:", 6)) {
		q_local(req + 6, out);
		return true;
	}
	return false;
}

void LP_Frame(void)
{
	if (!lp.active)
		return;
	if (cl.state == ca_active) {
		lp.was_in_game = true;
		/* the bots join once the game is running */
		if (lp.bots_pending > 0 && sv.active) {
			if (!lp.bots_at)
				lp.bots_at = Sys_Seconds() + 1.0;
			else if (Sys_Seconds() >= lp.bots_at) {
				/* the next profile on the list, so each bot is someone else */
				char list[256], *p = NULL;
				int n = 0;
				Str_Copy(list, local_bot_profiles->string, sizeof(list));
				for (char *t = strtok(list, " ,"); t; t = strtok(NULL, " ,"))
					n++;
				Str_Copy(list, local_bot_profiles->string, sizeof(list));
				p = strtok(list, " ,");
				for (int i = 0; p && n && i < lp.bots_added % n; i++)
					p = strtok(NULL, " ,");
				if (p)
					Cbuf_AddText(va("addBot %s\n", p));
				lp.bots_added++;
				lp.bots_pending--;
				lp.bots_at = Sys_Seconds() + 0.5;
			}
		}
		return;
	}
	/* the game ended (a set closed, the pause menu's Leave, an error):
	 * back to Local Play with the same players */
	if (cl.state == ca_disconnected && lp.was_in_game) {
		lp.active = false;
		lp.was_in_game = false;
		lp.bots_pending = 0;
		if (Cvar_Find("arena_setFirstTo"))
			Cvar_Set("arena_setFirstTo", "0");
		if (Cvar_Find("g_nomotd"))
			Cvar_Set("g_nomotd", "0");
		WC_OpenPage(LP_URL);
	}
}

static void LocalStart_f(int argc, const char **argv)
{
	for (int i = 1; i + 1 < argc; i += 2)
		if (!set_option(argv[i], argv[i + 1]))
			Con_Printf("local_start: bad %s %s\n", argv[i], argv[i + 1]);
	if (!start())
		Con_Printf("local_start: %s\n", start_problem() ? start_problem() : "can't start");
}

void LP_Init(void)
{
	local_mode = Cvar_Get("local_mode", "versus", CVAR_ARCHIVE, "Local Play: versus (a two player set) or ffa (free for all)");
	local_map = Cvar_Get("local_map", "duel_alley", CVAR_ARCHIVE, "Local Play: the map");
	local_maps = Cvar_Get("local_maps", "duel_alley aerowalk lqdm1 lqdm3 lqdm6 lqdm7", 0,
	                      "Local Play: the maps offered (those that exist)");
	local_firstto = Cvar_Get("local_firstto", "2", CVAR_ARCHIVE, "Local Play versus: game wins that take the set");
	local_bots = Cvar_Get("local_bots", "0", CVAR_ARCHIVE, "Local Play free for all: bots added to the game");
	local_bot_profiles = Cvar_Get("local_bot_profiles", "megan blue lavender alex ted tundra", 0,
	                              "Local Play: the bot profiles bots are added with, in turn (addBot <profile>)");
	Cmd_AddD("local_start", LocalStart_f,
	         "local_start [mode versus|ffa] [map <map>] [firstto n] [bots n] [split horizontal|vertical]: start a Local Play game");
}
