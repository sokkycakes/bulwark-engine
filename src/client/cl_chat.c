/* Bulwark engine - chat the player chooses not to see: QuakeWorld's ignore
 * lists (by name, player number or team, plus spectators and opponents)
 * and message filters ("filter #tag"). They apply to chat the server sends
 * as prints; a game that sends and draws chat itself (Nuclide's CSQC chat)
 * has to do its own ignoring.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IGNORES 64
#define MAX_FILTERS 8

static char ignored[MAX_IGNORES][64];
static int numignored;
static char ignored_teams[MAX_IGNORES][32];
static int numignored_teams;
static char filters[MAX_FILTERS][16];
static int numfilters;
static cvar_t *ignore_spec, *ignore_opponents;

static bool in_list(char list[][64], int n, const char *s)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(list[i], s))
			return true;
	return false;
}

static void list_add(char *list, size_t width, int *n, const char *s)
{
	for (int i = 0; i < *n; i++)
		if (!strcmp(list + (size_t)i * width, s))
			return;
	if (*n >= MAX_IGNORES) {
		Con_Printf("the list is full\n");
		return;
	}
	Str_Copy(list + (size_t)(*n)++ * width, s, width);
}

static bool list_remove(char *list, size_t width, int *n, const char *s)
{
	for (int i = 0; i < *n; i++)
		if (!strcmp(list + (size_t)i * width, s)) {
			memmove(list + (size_t)i * width, list + (size_t)(i + 1) * width, (size_t)(*n - i - 1) * width);
			(*n)--;
			return true;
		}
	return false;
}

static void print_lists(void)
{
	Con_Printf("User Ignore List: %s", numignored ? "" : "empty");
	for (int i = 0; i < numignored; i++)
		Con_Printf("%s\"%s\"", i ? ", " : "", ignored[i]);
	Con_Printf("\n");
	if (numignored_teams) {
		Con_Printf("Team Ignore List: ");
		for (int i = 0; i < numignored_teams; i++)
			Con_Printf("%s\"%s\"", i ? ", " : "", ignored_teams[i]);
		Con_Printf("\n");
	}
	Con_Printf("User Mute List: empty\n");
}

/* the name of player <number>, as "users" and the scoreboard number them */
static const char *player_name(const char *num, char *out, size_t size)
{
	int n = atoi(num);
	out[0] = 0;
	if (n > 0)
		CL_PlayerKey(n - 1, "name", out, size);
	return out;
}

static void Ignore_f(int argc, const char **argv)
{
	if (argc < 2) {
		print_lists();
		return;
	}
	for (int i = 1; i < argc; i++) {
		list_add(&ignored[0][0], sizeof(ignored[0]), &numignored, argv[i]);
		Con_Printf("ignoring %s\n", argv[i]);
	}
}

static void IgnoreId_f(int argc, const char **argv)
{
	char name[64];
	if (argc < 2) {
		print_lists();
		return;
	}
	if (!player_name(argv[1], name, sizeof(name))[0]) {
		Con_Printf("no player %s\n", argv[1]);
		return;
	}
	list_add(&ignored[0][0], sizeof(ignored[0]), &numignored, name);
	Con_Printf("ignoring %s\n", name);
}

static void IgnoreTeam_f(int argc, const char **argv)
{
	if (argc < 2) {
		print_lists();
		return;
	}
	list_add(&ignored_teams[0][0], sizeof(ignored_teams[0]), &numignored_teams, argv[1]);
	Con_Printf("ignoring team %s\n", argv[1]);
}

static void Unignore_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("%s <name>\n", argv[0]);
		return;
	}
	if (!list_remove(&ignored[0][0], sizeof(ignored[0]), &numignored, argv[1]))
		Con_Printf("%s isn't being ignored\n", argv[1]);
}

static void UnignoreId_f(int argc, const char **argv)
{
	char name[64];
	if (argc < 2) {
		Con_Printf("%s <player number>\n", argv[0]);
		return;
	}
	if (!list_remove(&ignored[0][0], sizeof(ignored[0]), &numignored, player_name(argv[1], name, sizeof(name))))
		Con_Printf("player %s isn't being ignored\n", argv[1]);
}

static void UnignoreTeam_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("%s <team>\n", argv[0]);
		return;
	}
	if (!list_remove(&ignored_teams[0][0], sizeof(ignored_teams[0]), &numignored_teams, argv[1]))
		Con_Printf("team %s isn't being ignored\n", argv[1]);
}

static void UnignoreAll_f(int argc, const char **argv)
{
	numignored = 0;
	Con_Printf("ignore list cleared\n");
}

static void UnignoreAllTeam_f(int argc, const char **argv)
{
	numignored_teams = 0;
	Con_Printf("team ignore list cleared\n");
}

/* filter [#tag...|clear]: with filters set, chat ending in "#tag" shows
 * only when the tag is one of them */
static void Filter_f(int argc, const char **argv)
{
	if (argc < 2) {
		if (!numfilters)
			Con_Printf("No filters defined\n");
		for (int i = 0; i < numfilters; i++)
			Con_Printf("%s#%s", i ? " " : "Current filters: ", filters[i]);
		if (numfilters)
			Con_Printf("\n");
		return;
	}
	numfilters = 0;
	if (!Str_ICmp(argv[1], "clear")) {
		Con_Printf("filters cleared\n");
		return;
	}
	for (int i = 1; i < argc && numfilters < MAX_FILTERS; i++)
		Str_Copy(filters[numfilters++], argv[i][0] == '#' ? argv[i] + 1 : argv[i], sizeof(filters[0]));
}

/* which player, by slot, a chat line came from: "name: text" or "(name): text" */
static int speaker(const char *text)
{
	int best = -1;
	size_t bestlen = 0;
	bool team = text[0] == '(';
	const char *s = team ? text + 1 : text;
	for (int i = 0; i < CL_MAX_PLAYERS; i++) {
		char name[64];
		size_t n;
		if (!CL_PlayerKey(i, "name", name, sizeof(name))[0])
			continue;
		n = strlen(name);
		if (n > bestlen && !strncmp(s, name, n) && (team ? s[n] == ')' : s[n] == ':')) {
			best = i;
			bestlen = n;
		}
	}
	return best;
}

/* false if a print shouldn't be shown */
bool CL_ChatAllowed(const char *text, int level)
{
	int who;
	char name[64], team[32], myteam[32];
	if (level != 3) /* PRINT_CHAT */
		return true;
	if (numfilters) {
		const char *hash = strrchr(text, '#');
		if (hash && hash > text && hash[-1] == ' ') {
			char tag[16];
			size_t n = strcspn(hash + 1, " \n");
			bool ok = false;
			Str_Copy(tag, hash + 1, BW_MIN(sizeof(tag), n + 1));
			for (int i = 0; i < numfilters; i++)
				ok |= !Str_ICmp(filters[i], tag);
			if (!ok)
				return false;
		}
	}
	if ((who = speaker(text)) < 0 || who == cl.playerslot)
		return true;
	CL_PlayerKey(who, "name", name, sizeof(name));
	CL_PlayerKey(who, "team", team, sizeof(team));
	CL_PlayerKey(cl.playerslot, "team", myteam, sizeof(myteam));
	if (in_list(ignored, numignored, name))
		return false;
	for (int i = 0; i < numignored_teams; i++)
		if (!strcmp(ignored_teams[i], team))
			return false;
	if (ignore_spec->ivalue) {
		char spec[8];
		if (atoi(CL_PlayerKey(who, "*spectator", spec, sizeof(spec))))
			return false;
	}
	if (ignore_opponents->ivalue && team[0] && strcmp(team, myteam))
		return false;
	return true;
}

void CL_InitChat(void)
{
	ignore_spec = Cvar_Get("ignore_spec", "0", CVAR_ARCHIVE, "Hide spectators' chat");
	ignore_opponents = Cvar_Get("ignore_opponents", "0", CVAR_ARCHIVE, "Hide chat from players on other teams");
	Cmd_AddD("ignore", Ignore_f, "ignore [name...]: hide a player's chat (no name lists who is ignored)");
	Cmd_AddD("ignore_id", IgnoreId_f, "ignore_id <player number>: hide a player's chat");
	Cmd_AddD("ignore_team", IgnoreTeam_f, "ignore_team <team>: hide a team's chat");
	Cmd_AddD("ignorelist", Ignore_f, "Who is ignored");
	Cmd_AddD("unignore", Unignore_f, "unignore <name>: show a player's chat again");
	Cmd_AddD("unignore_id", UnignoreId_f, "unignore_id <player number>: show a player's chat again");
	Cmd_AddD("unignore_team", UnignoreTeam_f, "unignore_team <team>: show a team's chat again");
	Cmd_AddD("unignoreall", UnignoreAll_f, "Clear the ignore list");
	Cmd_AddD("unignoreall_team", UnignoreAllTeam_f, "Clear the team ignore list");
	Cmd_AddD("filter", Filter_f, "filter [#tag...|clear]: only show tagged chat with these tags");
}
