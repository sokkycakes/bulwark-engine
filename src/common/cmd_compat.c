/* Bulwark engine - console commands kept for compatibility with FTE and the
 * QuakeWorld clients before it: other spellings of existing commands, cvar
 * default handling, file system rescans and hashes, and the commands FTE
 * itself only keeps to say they are obsolete.
 * ISC licence, see LICENSE. */
#include "cmd.h"
#include "cvar.h"
#include "fs.h"
#include "digest.h"
#include "bsptool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void join_from(int first, int argc, const char **argv, char *out, size_t size)
{
	out[0] = 0;
	for (int i = first; i < argc; i++) {
		if (i > first)
			Str_Cat(out, " ", size);
		Str_Cat(out, argv[i], size);
	}
}

/* in <seconds> <command>: run a command later */
static void In_f(int argc, const char **argv)
{
	char cmd[4096];
	if (argc < 3) {
		Con_Printf("in <seconds to wait> <command to execute>\n");
		return;
	}
	join_from(2, argc, argv, cmd, sizeof(cmd));
	Cbuf_AddDeferred(atof(argv[1]), cmd);
}

/* setfl <var> <value> <a|u|s>: set, also archiving it, or putting it in
 * userinfo or serverinfo */
static void Setfl_f(int argc, const char **argv)
{
	cvar_t *v;
	int flags = 0;
	if (argc < 4) {
		Con_Printf("setfl <var> <value> <a|u|s>\n");
		return;
	}
	for (const char *f = argv[3]; *f; f++) {
		if (*f == 'a')
			flags |= CVAR_ARCHIVE;
		else if (*f == 'u')
			flags |= CVAR_USERINFO;
		else if (*f == 's')
			flags |= CVAR_SERVERINFO;
	}
	if (!(v = Cvar_Find(argv[1])))
		v = Cvar_Get(argv[1], "", CVAR_USERCREATED, NULL);
	if (!Cvar_MayFlag(v, flags))
		return;
	v->flags |= flags;
	Cvar_Set(v->name, argv[2]);
}

/* set_tp <var> <value>: set, under ezQuake's name for teamplay variables */
static void SetTp_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 3) {
		Con_Printf("set_tp <var> <value>\n");
		return;
	}
	if (!(v = Cvar_Find(argv[1])))
		v = Cvar_Get(argv[1], "", CVAR_USERCREATED, NULL);
	Cvar_Set(v->name, argv[2]);
}

/* newalias: alias under its other name */
static void Newalias_f(int argc, const char **argv)
{
	char line[4096];
	strbuf_t sb = {0};
	SB_AppendStr(&sb, "alias");
	for (int i = 1; i < argc; i++) {
		SB_AppendStr(&sb, " \"");
		SB_AppendStr(&sb, argv[i]);
		SB_AppendStr(&sb, "\"");
	}
	Str_Copy(line, sb.data ? sb.data : "alias", sizeof(line));
	SB_Free(&sb);
	Cmd_ExecuteString(line);
}

/* cvar_lockdefaults: today's values become the defaults (reset, differences
 * and the config then measure from them); cvar_purgedefaults undoes it */
static void LockDefaults_f(int argc, const char **argv)
{
	int n = 0;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (strcmp(v->defstring, v->string)) {
			Mem_Free(v->defstring);
			v->defstring = Mem_Strdup(v->string);
			n++;
		}
	Con_DPrintf("%d cvar defaults locked\n", n);
}

static void PurgeDefaults_f(int argc, const char **argv)
{
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (v->engdefault && strcmp(v->defstring, v->engdefault)) {
			Mem_Free(v->defstring);
			v->defstring = Mem_Strdup(v->engdefault);
		}
}

/* fs_hash <file>: the file's hashes */
static void FsHash_f(int argc, const char **argv)
{
	static const char *const algos[] = {"CRC16", "MD5", "SHA1", "SHA256"};
	size_t len;
	char *data;
	if (argc < 2) {
		Con_Printf("fs_hash <FILENAME>: computes various hashes of the specified file\n");
		return;
	}
	if (!(data = FS_LoadFile(argv[1], &len))) {
		Con_Printf("%s not found\n", argv[1]);
		return;
	}
	Con_Printf("%s (%zu bytes, from %s):\n", argv[1], len, FS_WhichPath(argv[1]));
	for (size_t i = 0; i < BW_ARRAY_COUNT(algos); i++) {
		char hex[80];
		if (Digest_Hex(algos[i], data, len, hex, sizeof(hex)))
			Con_Printf("  %-7s %s\n", algos[i], hex);
	}
	Mem_Free(data);
}

/* fs_restart, fs_flush, flush: pick up packages added or changed while
 * running */
static void FsRestart_f(int argc, const char **argv)
{
	FS_Restart();
	Con_Printf("file system rescanned\n");
}

/* colorize/colourise <text>: prints text with ezQuake's &cRGB colours
 * turned into the engine's ^xRGB */
static void Colorize_f(int argc, const char **argv)
{
	char text[4096], out[8192];
	size_t n = 0;
	join_from(1, argc, argv, text, sizeof(text));
	for (const char *p = text; *p && n + 8 < sizeof(out); p++) {
		if (p[0] == '&' && p[1] == 'c' && p[2] && p[3] && p[4]) {
			out[n++] = '^';
			out[n++] = 'x';
			memcpy(out + n, p + 2, 3);
			n += 3;
			p += 4;
		} else if (p[0] == '&' && p[1] == 'r') {
			out[n++] = '^';
			out[n++] = '7';
			p++;
		} else {
			out[n++] = *p;
		}
	}
	out[n] = 0;
	Con_Printf("%s\n", out);
}

/* restrict <command or cvar> [level]: the execution level needed to run a
 * command or change a cvar. The local console has 50, rcon rcon_level,
 * ranked players their rights. */
static void Restrict_f(int argc, const char **argv)
{
	int cur;
	bool cmd;
	if (argc < 2) {
		Con_Printf("restrict <command or cvar> [level 0-%d]\n", RESTRICT_MAX);
		return;
	}
	cmd = (cur = Cmd_Restriction(argv[1])) >= 0;
	if (!cmd && (cur = Cvar_Restriction(argv[1])) < 0) {
		Con_Printf("%s is not a command or cvar\n", argv[1]);
		return;
	}
	if (argc < 3) {
		Con_Printf("%s is restricted to level %d\n", argv[1], cur);
		return;
	}
	if (cur > Cmd_ExecLevel()) {
		Con_Printf("You can't change the restriction of %s\n", argv[1]);
		return;
	}
	{
		int level = BW_CLAMP(atoi(argv[2]), 0, BW_MIN(RESTRICT_MAX, Cmd_ExecLevel()));
		if (cmd)
			Cmd_SetRestriction(argv[1], level);
		else
			Cvar_Find(argv[1])->restriction = level;
	}
}

/* aliaslevel <alias> [level]: the level an alias's text runs at (never
 * above the level of what runs it) */
static void Aliaslevel_f(int argc, const char **argv)
{
	int level;
	if (argc < 2) {
		Con_Printf("aliaslevel <var> [execlevel]\n");
		return;
	}
	if (!Cmd_AliasLevel(argv[1], &level)) {
		Con_Printf("Alias not defined\n");
		return;
	}
	if (argc < 3) {
		if (level < 0)
			Con_Printf("alias %s runs at its caller's level\n", argv[1]);
		else
			Con_Printf("alias %s is set to run at level %d\n", argv[1], level);
		return;
	}
	level = atoi(argv[2]);
	Cmd_SetAliasLevel(argv[1], level < 0 ? -1 : BW_MIN(level, Cmd_ExecLevel()));
}

/* wf <file> <text>: writes a line of text to a file in the game directory;
 * df <file>: prints a file. FTE has both without saying what they do; these
 * are the plain readings of the names. */
static void Wf_f(int argc, const char **argv)
{
	char path[1024], text[4096];
	FILE *f;
	if (argc < 3) {
		Con_Printf("wf <file> <text>: write text to a file in the game directory\n");
		return;
	}
	join_from(2, argc, argv, text, sizeof(text));
	if (!FS_WritePath(argv[1], path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", argv[1]);
		return;
	}
	fprintf(f, "%s\n", text);
	fclose(f);
}

static void Df_f(int argc, const char **argv)
{
	size_t len;
	char *data;
	if (argc < 2) {
		Con_Printf("df <file>: print a file\n");
		return;
	}
	if (!(data = FS_LoadFile(argv[1], &len))) {
		Con_Printf("%s not found\n", argv[1]);
		return;
	}
	Con_Printf("%s%s", data, len && data[len - 1] != '\n' ? "\n" : "");
	Mem_Free(data);
}

/* commands FTE keeps only to say they no longer do anything */
static void Obsolete_f(int argc, const char **argv)
{
	Con_Printf("%s is obsolete, redundant, or otherwise outdated.\n", argv[0]);
}

/* What a server's text and game code must not reach: anything that loads
 * code, writes or deletes files, reaches outside the game, sends the rcon
 * password somewhere or throws the player out. The player's own console,
 * configs, binds and the menu run at RESTRICT_MAX and keep them. "restrict"
 * lowers one again for a server that is trusted. */
void Cmd_ProtectLocal(void)
{
	static const char *const cmds[] = {
		/* code */
		"plug_load", "plug_close", "plug_closeall", "compile", "applycompile", "decompile",
		/* files written or removed */
		"wf", "writeconfig", "saveconfig", "host_writeconfig", "cfg_save", "cfg_save_ifmodified", "condump",
		"logfile", "fraglogfile", "logplayers", "logrcon", "writeid", "writeip", "deletesave", "unsavegame",
		"rmdemo", "rmdemonum", "record", "easyrecord", "rerecord", "mvdrecord", "sv_demorecord",
		"sv_demoeasyrecord", "sv_demoremove", "sv_demonumremove", "edit", "aliasedit", "mod_terrain_save",
		"mod_terrain_convert", "mod_terrain_create", "r_editlights_save", "pin_save", "mod_bspx_strip",
		"mod_bspx_injest", "r_exportalleffects", "r_exportbuiltinparticles", "r_converteffectinfo",
		"sv_saveentfile", "pr_dumpplatform", "coredump_csqc", "coredump_menuqc", "coredump_ssqc",
		/* outside the game */
		"pkg", "curl", "download_map_url", "gamedir", "fs_changegame", "fs_changemod", "dtls_importtrust",
		"dtls_untrustall", "rcon",
		/* the player's own setup */
		"quit", "exit", "unbindall", "cvar_resettodefaults_all", "cvar_lockdefaults", "cvar_purgedefaults",
		"resetcfg", "cfg_reset"};
	static const char *const cvars[] = {"cmd_gamecodelevel", "cmd_serverlevel", "rcon_password", "rcon_address",
	                                    "rcon_level", "pr_sourcedir", "log_dir", "sv_snapdir", "cl_downloads"};
	for (size_t i = 0; i < BW_ARRAY_COUNT(cmds); i++)
		Cmd_SetRestriction(cmds[i], RESTRICT_MAX);
	for (size_t i = 0; i < BW_ARRAY_COUNT(cvars); i++) {
		cvar_t *v = Cvar_Find(cvars[i]);
		if (v)
			v->restriction = RESTRICT_MAX;
	}
}

void Cmd_InitCompat(void)
{
	static const char *const obsolete[] = {"check_maps", "download_map_url", "sv_downloadchunksperframe",
	                                       "sv_progsname", "sv_progtype", "sv_speedcheck", "sys_select_timeout"};
	Cmd_AddD("in", In_f, "in <seconds> <command>: run a command later");
	Cmd_AddD("setfl", Setfl_f, "setfl <var> <value> <a|u|s>: set a cvar, archived (a), in userinfo (u) or serverinfo (s)");
	Cmd_AddD("set_tp", SetTp_f, "set_tp <var> <value>: set (ezQuake's name)");
	Cmd_AddD("newalias", Newalias_f, "newalias <name> <commands>: alias");
	Cmd_AddD("cvar_lockdefaults", LockDefaults_f, "Make every cvar's current value its default");
	Cmd_AddD("cvar_purgedefaults", PurgeDefaults_f, "Put cvar defaults back to the engine's, keeping their values");
	Cmd_AddD("fs_hash", FsHash_f, "fs_hash <file>: CRC16, MD5, SHA1 and SHA256 of a file");
	Cmd_AddD("fs_restart", FsRestart_f, "Rescan the game directories for packages");
	Cmd_AddD("fs_flush", FsRestart_f, "Rescan the game directories for packages");
	Cmd_AddD("flush", FsRestart_f, "Rescan the game directories for packages");
	Cmd_AddD("colorize", Colorize_f, "colorize <text>: print text with &cRGB colours");
	Cmd_AddD("colourise", Colorize_f, "colourise <text>: print text with &cRGB colours");
	Cvar_Get("cmd_gamecodelevel", "29", 0, "The execution level of commands the game's QC runs (localcmd)");
	Cvar_Get("cmd_serverlevel", "29", 0, "The execution level of commands a server sends (stufftext)");
	BSP_InitTools();
	Cmd_AddD("wf", Wf_f, "wf <file> <text>: write text to a file in the game directory");
	Cmd_AddD("df", Df_f, "df <file>: print a file");
	Cmd_AddD("restrict", Restrict_f, "restrict <command or cvar> [level]: the execution level needed to use it");
	Cmd_AddD("aliaslevel", Aliaslevel_f, "aliaslevel <alias> [level]: the execution level an alias runs at");
	for (size_t i = 0; i < BW_ARRAY_COUNT(obsolete); i++)
		Cmd_AddD(obsolete[i], Obsolete_f, "Obsolete in FTE; does nothing");
}
