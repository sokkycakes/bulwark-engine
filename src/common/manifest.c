/* Bulwark engine - game manifests and switching games or mods, plus the
 * chat word filter. The manifest format is the one fte_manifests.txt
 * documents: one command per line, // comments, - and + lines holding
 * commands for before and after default.cfg. ISC licence, see LICENSE. */
#include "manifest.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static manifest_t active;
static manifest_t *pending; /* waiting for __fs_apply */
static strbuf_t pending_then; /* commands for once it is done */
static mf_fetch_fn fetcher;
static void load_filter(void);

void Mf_SetFetcher(mf_fetch_fn fn) { fetcher = fn; }
const manifest_t *Mf_Active(void) { return &active; }

void Mf_Free(manifest_t *mf)
{
	SB_Free(&mf->precmds);
	SB_Free(&mf->postcmds);
	memset(mf, 0, sizeof(*mf));
}

static void add_dir(char list[][64], int *count, const char *dir)
{
	for (int i = 0; i < *count; i++)
		if (!strcmp(list[i], dir))
			return;
	if (*count < MF_MAX_DIRS)
		Str_Copy(list[(*count)++], dir, 64);
}

bool Mf_Parse(manifest_t *mf, const char *text, const char *source)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	bool first = true, ok = true;
	memset(mf, 0, sizeof(*mf));
	Str_Copy(mf->source, source ? source : "", sizeof(mf->source));
	while (text && *text) {
		char line[4096];
		const char *end = strchr(text, '\n');
		size_t n = end ? (size_t)(end - text) : strlen(text);
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, text, n);
		line[n] = 0;
		text = end ? end + 1 : "";
		if (n && line[n - 1] == '\r')
			line[n - 1] = 0;
		const char *l = line;
		while (*l == ' ' || *l == '\t')
			l++;
		if (*l == '-' || *l == '+') {
			SB_AppendStr(*l == '-' ? &mf->precmds : &mf->postcmds, l + 1);
			SB_AppendStr(*l == '-' ? &mf->precmds : &mf->postcmds, "\n");
			continue;
		}
		Cmd_TokenizeInto(t, l);
		if (!t->argc)
			continue;
		const char *k = t->argv[0], *v = t->argc > 1 ? t->argv[1] : "";
		if (first) {
			first = false;
			if (Str_ICmp(k, "FTEMANIFEST")) {
				Con_Printf("%s is not a manifest (no FTEMANIFEST line)\n", source ? source : "text");
				ok = false;
				break;
			}
			continue;
		}
		if (!Str_ICmp(k, "game"))
			Str_Copy(mf->game, v, sizeof(mf->game));
		else if (!Str_ICmp(k, "name"))
			Str_Copy(mf->name, v, sizeof(mf->name));
		else if (!Str_ICmp(k, "protocolname"))
			Str_Copy(mf->protocolname, v, sizeof(mf->protocolname));
		else if (!Str_ICmp(k, "mainconfig"))
			Str_Copy(mf->mainconfig, v, sizeof(mf->mainconfig));
		else if (!Str_ICmp(k, "updateurl"))
			Str_Copy(mf->updateurl, v, sizeof(mf->updateurl));
		else if (!Str_ICmp(k, "downloadsurl"))
			Str_Copy(mf->downloadsurl, v, sizeof(mf->downloadsurl));
		else if (!Str_ICmp(k, "install"))
			Str_Copy(mf->install, v, sizeof(mf->install));
		else if (!Str_ICmp(k, "rtcbroker"))
			Str_Copy(mf->rtcbroker, v, sizeof(mf->rtcbroker));
		else if (!Str_ICmp(k, "disablehomedir"))
			mf->disablehomedir = atoi(v) != 0;
		else if (!Str_ICmp(k, "basegame") && *v)
			add_dir(mf->basegame, &mf->numbasegames, v);
		else if (!Str_ICmp(k, "gamedir") && *v)
			add_dir(mf->gamedir, &mf->numgamedirs, v);
		else if (!Str_ICmp(k, "defaultexec"))
			SB_AppendStr(&mf->precmds, va("%s\n", v));
		else if ((!Str_ICmp(k, "package") || !Str_ICmp(k, "archivedpackage")) && t->argc >= 3 &&
		         mf->numpackages < MF_MAX_PACKAGES) {
			mf_package_t *p = &mf->packages[mf->numpackages++];
			int a = 3;
			Str_Copy(p->path, t->argv[1], sizeof(p->path));
			Str_Copy(p->crc, t->argv[2], sizeof(p->crc));
			if (!Str_ICmp(k, "archivedpackage") && t->argc > 3)
				Str_Copy(p->archived, t->argv[a++], sizeof(p->archived));
			for (; a < t->argc && p->nummirrors < 4; a++)
				Str_Copy(p->mirrors[p->nummirrors++], t->argv[a], sizeof(p->mirrors[0]));
		} else if (!Str_ICmp(k, "minver") || !Str_ICmp(k, "maxver")) {
			/* versions of other engines; Bulwark's own isn't numbered that way */
		} else
			Con_DPrintf("%s: unknown manifest command %s\n", source ? source : "manifest", k);
	}
	Mem_Free(t);
	if (first && ok) {
		Con_Printf("%s is empty\n", source ? source : "manifest");
		ok = false;
	}
	if (!ok)
		Mf_Free(mf);
	else if (!mf->game[0])
		Str_Copy(mf->game, "quake", sizeof(mf->game));
	return ok;
}

/* reads <basedir>/<name>, an OS file outside the search path */
static char *read_os(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0 || len > (1 << 20)) {
		fclose(f);
		return NULL;
	}
	char *buf = Mem_Alloc((size_t)len + 1);
	if (fread(buf, 1, (size_t)len, f) != (size_t)len)
		len = 0;
	buf[len] = 0;
	fclose(f);
	return buf;
}

static bool load_os(manifest_t *mf, const char *path)
{
	char *text = read_os(path);
	bool ok = text && Mf_Parse(mf, text, path);
	Mem_Free(text);
	return ok;
}

static bool load_named(manifest_t *mf, const char *name)
{
	if (!FS_SafeName(name) || strchr(name, '/'))
		return false;
	return load_os(mf, va("%s/%s.fmf", FS_BaseDir(), name));
}

/* the directories a manifest mounts, base games first; '*' marks private ones */
static int mf_dirs(const manifest_t *mf, const char **dirs)
{
	int n = 0;
	for (int i = 0; i < mf->numbasegames; i++)
		dirs[n++] = mf->basegame[i][0] == '*' ? mf->basegame[i] + 1 : mf->basegame[i];
	for (int i = 0; i < mf->numgamedirs && n < 2 * MF_MAX_DIRS; i++)
		dirs[n++] = mf->gamedir[i][0] == '*' ? mf->gamedir[i] + 1 : mf->gamedir[i];
	return n;
}

static void mount(const manifest_t *mf)
{
	const char *dirs[2 * MF_MAX_DIRS];
	int n = mf_dirs(mf, dirs);
	if (!n) {
		dirs[n++] = "base";
	}
	FS_SetGameDirs(dirs, n);
	if (mf->protocolname[0])
		Cvar_Set("com_protocolname", mf->protocolname);
	Cvar_Set("com_fullgamename", mf->name[0] ? mf->name : FS_GameDir());
	Cvar_Set("com_gamename", mf->game);
	if (mf->mainconfig[0]) {
		char cfg[64];
		Str_Copy(cfg, mf->mainconfig, sizeof(cfg));
		char *dot = strrchr(cfg, '.');
		if (dot && !Str_ICmp(dot, ".cfg"))
			*dot = 0;
		Cvar_Set("cfg_save_name", cfg);
	}
	load_filter();
}

/* a manifest that just lists the directories given */
static void implicit(manifest_t *mf, const char *const *games, int count)
{
	memset(mf, 0, sizeof(*mf));
	Str_Copy(mf->game, "quake", sizeof(mf->game));
	for (int i = 0; i < count; i++)
		add_dir(i ? mf->gamedir : mf->basegame, i ? &mf->numgamedirs : &mf->numbasegames, games[i]);
}

void Mf_Startup(const char *const *games, int count)
{
	manifest_t mf;
	if (!(count == 1 && load_named(&mf, games[0])) && !(count == 0 && load_named(&mf, "default"))) {
		const char *base = "base";
		if (count)
			implicit(&mf, games, count);
		else
			implicit(&mf, &base, 1);
	}
	Mf_Free(&active);
	active = mf;
	mount(&active);
}

/* text names file, as a whole word in any case ("config.cfg" but not
 * "myconfig.cfg") */
static bool mentions(const char *text, const char *file)
{
	size_t n = strlen(file);
	for (const char *p = text; *p; p++) {
		if (strncasecmp(p, file, n))
			continue;
		bool start = p == text || isspace((unsigned char)p[-1]) || p[-1] == '"' || p[-1] == ';';
		bool end = !p[n] || isspace((unsigned char)p[n]) || p[n] == '"' || p[n] == ';';
		if (start && end)
			return true;
	}
	return false;
}

/* the startup configs, in order. A game with a quake.rc gets it in place
 * of default.cfg: Nuclide games set their defaults there (Rad-Therapy's
 * movement cvars are in a default_cvar.cfg only its quake.rc runs). It
 * counts only from the same game directory as the default.cfg (or
 * default_controls.cfg) or a later one, so a mod on top of base with its
 * own default.cfg (TF) doesn't run base's quake.rc. The saved config and
 * autoexec.cfg follow unless the quake.rc runs them. */
static void configs_text(strbuf_t *sb)
{
	const char *fallback = FS_FileExists("default.cfg") ? "default.cfg" :
	                       FS_FileExists("default_controls.cfg") ? "default_controls.cfg" : NULL;
	int rcdir = FS_WhichGameDir("quake.rc");
	char *rc = NULL;
	if (rcdir >= 0 && (!fallback || rcdir >= FS_WhichGameDir(fallback)))
		rc = FS_LoadFile("quake.rc", NULL);
	if (active.precmds.data)
		SB_AppendStr(sb, active.precmds.data);
	if (rc)
		SB_AppendStr(sb, "exec quake.rc\n");
	else if (fallback)
		SB_Printf(sb, "exec %s\n", fallback);
	if (active.postcmds.data)
		SB_AppendStr(sb, active.postcmds.data);
	const char *cfg = va("%s.cfg", Cvar_String("cfg_save_name"));
	if (FS_FileExists(cfg) && !(rc && mentions(rc, cfg)))
		SB_Printf(sb, "exec %s\n", cfg);
	if (FS_FileExists("autoexec.cfg") && !(rc && mentions(rc, "autoexec.cfg")))
		SB_AppendStr(sb, "exec autoexec.cfg\n");
	if (rc)
		Mem_Free(rc);
}

void Mf_ExecConfigs(void)
{
	strbuf_t sb = {0};
	configs_text(&sb);
	if (sb.data)
		Cbuf_AddText(sb.data);
	SB_Free(&sb);
}

void Mf_Switch(manifest_t *mf)
{
	if (pending) {
		Mf_Free(pending);
		Mem_Free(pending);
	}
	SB_Free(&pending_then);
	pending = Mem_Alloc(sizeof(*pending));
	*pending = *mf;
	memset(mf, 0, sizeof(*mf));
	strbuf_t sb = {0};
	if (Cmd_Exists("killserver"))
		SB_AppendStr(&sb, "killserver\n");
	if (Cmd_Exists("disconnect"))
		SB_AppendStr(&sb, "disconnect\n");
	SB_AppendStr(&sb, "__fs_apply\n");
	Cbuf_InsertText(sb.data);
	SB_Free(&sb);
}

void Mf_Then(const char *cmds)
{
	SB_AppendStr(&pending_then, cmds);
}

void Mf_SwitchGamedir(const char *dir)
{
	manifest_t mf;
	if (!load_named(&mf, dir)) {
		/* the current game with dir as its mod, unless dir is one of its
		 * base games */
		memset(&mf, 0, sizeof(mf));
		Str_Copy(mf.source, active.source, sizeof(mf.source));
		Str_Copy(mf.game, active.game, sizeof(mf.game));
		Str_Copy(mf.name, active.name, sizeof(mf.name));
		Str_Copy(mf.protocolname, active.protocolname, sizeof(mf.protocolname));
		Str_Copy(mf.mainconfig, active.mainconfig, sizeof(mf.mainconfig));
		Str_Copy(mf.downloadsurl, active.downloadsurl, sizeof(mf.downloadsurl));
		memcpy(mf.basegame, active.basegame, sizeof(mf.basegame));
		mf.numbasegames = active.numbasegames;
		if (active.precmds.data)
			SB_AppendStr(&mf.precmds, active.precmds.data);
		if (active.postcmds.data)
			SB_AppendStr(&mf.postcmds, active.postcmds.data);
		bool isbase = false;
		for (int i = 0; i < mf.numbasegames; i++)
			if (!strcmp(mf.basegame[i][0] == '*' ? mf.basegame[i] + 1 : mf.basegame[i], dir))
				isbase = true;
		if (!mf.numbasegames && active.numgamedirs)
			/* an implicit -game list: its first directory is the base */
			add_dir(mf.basegame, &mf.numbasegames, active.gamedir[0]);
		if (!isbase)
			add_dir(mf.gamedir, &mf.numgamedirs, dir);
	}
	Mf_Switch(&mf);
}

/* __fs_apply: the second half of a switch, once the game has stopped */
static void FsApply_f(int argc, const char **argv)
{
	if (!pending)
		return;
	Cfg_SaveIfModified();
	Mf_Free(&active);
	active = *pending;
	Mem_Free(pending);
	pending = NULL;
	mount(&active);
	Con_Printf("Switching to %s\n", active.name[0] ? active.name : FS_GameDir());
	Cfg_ResetDefaults();
	/* in reverse: each insert goes in front */
	/* the menu restarts last, so a game started by the "then" commands is
	 * already connecting and the menu doesn't queue its background map */
	if (Cmd_Exists("menu_restart"))
		Cbuf_InsertText("menu_restart\n");
	if (pending_then.data)
		Cbuf_InsertText(pending_then.data);
	SB_Free(&pending_then);
	/* the configs run after the restarts so they see the new files */
	strbuf_t keep = {0};
	configs_text(&keep);
	if (keep.data)
		Cbuf_InsertText(keep.data);
	SB_Free(&keep);
	if (Cmd_Exists("snd_restart"))
		Cbuf_InsertText("snd_restart\n");
	if (Cmd_Exists("vid_reload"))
		Cbuf_InsertText("vid_reload\n");
}

/* --- listing games and mods --------------------------------------------- */

static bool looks_like_game(const char *dir)
{
	static const char *const marks[] = {"default.cfg", "default_controls.cfg", "progs.dat", "csprogs.dat",
	                                    "qwprogs.dat", "menu.dat", "pak0.pak", "pak0.pk3", "maps", NULL};
	DIR *d = opendir(dir);
	struct dirent *e;
	bool yes = false;
	if (!d)
		return false;
	while (!yes && (e = readdir(d))) {
		const char *ext = strrchr(e->d_name, '.');
		for (int i = 0; marks[i]; i++)
			if (!Str_ICmp(e->d_name, marks[i]))
				yes = true;
		if (ext && (!Str_ICmp(ext, ".pk3") || !Str_ICmp(ext, ".pak") || !Str_ICmp(ext, ".pk3dir")))
			yes = true;
	}
	closedir(d);
	return yes;
}

/* the .fmf files and game directories of the base directory; returns how
 * many. Manifests are numbered for "fs_changegame <n>". */
static int list_games(bool print, int pick, char *picked, size_t pickedsize)
{
	DIR *d = opendir(FS_BaseDir());
	struct dirent *e;
	char names[256][64];
	int n = 0, count = 0, num = 0;
	if (!d)
		return 0;
	while ((e = readdir(d)) && n < 256)
		if (e->d_name[0] != '.')
			Str_Copy(names[n++], e->d_name, sizeof(names[0]));
	closedir(d);
	qsort(names, (size_t)n, sizeof(names[0]), (int (*)(const void *, const void *))strcmp);
	if (print)
		Con_Printf("Valid games/mods are:\n");
	for (int i = 0; i < n; i++) {
		const char *ext = strrchr(names[i], '.');
		if (!ext || Str_ICmp(ext, ".fmf"))
			continue;
		manifest_t mf;
		char path[512];
		snprintf(path, sizeof(path), "%.400s/%.63s", FS_BaseDir(), names[i]);
		if (!load_os(&mf, path))
			continue;
		num++;
		if (print)
			Con_Printf("\tfs_changegame \"%d\" //%s (%s)\n", num, mf.name[0] ? mf.name : names[i], names[i]);
		if (num == pick && picked)
			Str_Copy(picked, path, pickedsize);
		Mf_Free(&mf);
		count++;
	}
	for (int i = 0; i < n; i++) {
		char path[512];
		struct stat st;
		snprintf(path, sizeof(path), "%.400s/%.63s", FS_BaseDir(), names[i]);
		if (stat(path, &st) || !S_ISDIR(st.st_mode) || !looks_like_game(path))
			continue;
		if (print)
			Con_Printf("\t%s (%s/)\n", names[i], FS_BaseDir());
		count++;
	}
	if (print && !count)
		Con_Printf("No games or mods known.\nYou may need to use -basedir $PATHTOGAME on the commandline.\n");
	return count;
}

/* the n-th game directory of the base directory, for the menu's
 * getgamedirinfo: 0 its name, 1 its title, 2 settings overrides, 3 the
 * command that loads it, 4 an icon, 5 the directories it uses. NULL past
 * the end. The list is read again whenever n is 0. */
const char *Mf_GameDirInfo(int n, int prop)
{
	static char dirs[128][64];
	static int numdirs;
	if (n == 0) {
		DIR *d = opendir(FS_BaseDir());
		struct dirent *e;
		numdirs = 0;
		while (d && (e = readdir(d)) && numdirs < 128) {
			char path[512];
			struct stat st;
			snprintf(path, sizeof(path), "%.400s/%.63s", FS_BaseDir(), e->d_name);
			if (e->d_name[0] != '.' && !stat(path, &st) && S_ISDIR(st.st_mode) && looks_like_game(path))
				Str_Copy(dirs[numdirs++], e->d_name, sizeof(dirs[0]));
		}
		if (d)
			closedir(d);
		qsort(dirs, (size_t)numdirs, sizeof(dirs[0]), (int (*)(const void *, const void *))strcmp);
	}
	if (n < 0 || n >= numdirs)
		return NULL;
	const char *dir = dirs[n];
	static char out[256];
	manifest_t mf;
	switch (prop) {
	case 0:
	case 5:
		return dir;
	case 1:
		out[0] = 0;
		if (load_named(&mf, dir)) {
			Str_Copy(out, mf.name, sizeof(out));
			Mf_Free(&mf);
		}
		return out;
	case 2:
		return "";
	case 3:
		snprintf(out, sizeof(out), "gamedir \"%s\"\n", dir);
		return out;
	case 4:
		snprintf(out, sizeof(out), "%s/icon.tga", dir);
		return FS_FileExists(out) ? out : "";
	default:
		return "";
	}
}

/* gamedir [dir]: shows or changes the mod directory */
static void Gamedir_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Current gamedir: %s\n", FS_GameDir());
		list_games(true, 0, NULL, 0);
		return;
	}
	if (!FS_SafeName(argv[1]) || strchr(argv[1], '/')) {
		Con_Printf("gamedir: %s isn't a directory name\n", argv[1]);
		return;
	}
	Mf_SwitchGamedir(argv[1]);
}

/* fs_changegame [n|manifest|url]: switches to another game's manifest */
static void FsChangegame_f(int argc, const char **argv)
{
	char path[512];
	manifest_t mf;
	if (argc < 2) {
		list_games(true, 0, NULL, 0);
		return;
	}
	const char *a = argv[1];
	if (strstr(a, "://")) {
		if (fetcher)
			fetcher(a);
		else
			Con_Printf("fs_changegame: this build can't download manifests\n");
		return;
	}
	path[0] = 0;
	if (isdigit((unsigned char)a[0]) && atoi(a) > 0)
		list_games(false, atoi(a), path, sizeof(path));
	if (!path[0]) {
		struct stat st;
		if (!stat(a, &st) && S_ISREG(st.st_mode))
			Str_Copy(path, a, sizeof(path));
		else if (FS_SafeName(a) && !strchr(a, '/'))
			snprintf(path, sizeof(path), "%s/%s%s", FS_BaseDir(), a, strrchr(a, '.') ? "" : ".fmf");
	}
	if (path[0] && load_os(&mf, path)) {
		Mf_Switch(&mf);
		return;
	}
	/* a plain directory works as a game of its own */
	if (FS_SafeName(a) && !strchr(a, '/') && looks_like_game(va("%s/%s", FS_BaseDir(), a))) {
		implicit(&mf, &a, 1);
		Mf_Switch(&mf);
		return;
	}
	Con_Printf("Game unknown\n");
}

/* fs_showmanifest: the manifest in effect, as a file would say it */
static void FsShowmanifest_f(int argc, const char **argv)
{
	const manifest_t *m = &active;
	Con_Printf("FTEMANIFEST 1\n");
	if (m->source[0])
		Con_Printf("// from %s\n", m->source);
	Con_Printf("game \"%s\"\n", m->game);
	if (m->name[0])
		Con_Printf("name \"%s\"\n", m->name);
	if (m->protocolname[0])
		Con_Printf("protocolname \"%s\"\n", m->protocolname);
	if (m->updateurl[0])
		Con_Printf("updateurl \"%s\"\n", m->updateurl);
	if (m->downloadsurl[0])
		Con_Printf("downloadsurl \"%s\"\n", m->downloadsurl);
	if (m->install[0])
		Con_Printf("install \"%s\"\n", m->install);
	if (m->rtcbroker[0])
		Con_Printf("rtcbroker \"%s\"\n", m->rtcbroker);
	Con_Printf("mainconfig \"%s.cfg\"\n", Cvar_String("cfg_save_name"));
	Con_Printf("// basedir \"%s\"\n", FS_BaseDir());
	for (int i = 0; i < m->numbasegames; i++)
		Con_Printf("basegame \"%s\"\n", m->basegame[i]);
	for (int i = 0; i < m->numgamedirs; i++)
		Con_Printf("gamedir \"%s\"\n", m->gamedir[i]);
	for (int i = 0; i < m->numpackages; i++) {
		const mf_package_t *p = &m->packages[i];
		Con_Printf("%s \"%s\" %s", p->archived[0] ? "archivedpackage" : "package", p->path, p->crc);
		if (p->archived[0])
			Con_Printf(" \"%s\"", p->archived);
		for (int k = 0; k < p->nummirrors; k++)
			Con_Printf(" \"%s\"", p->mirrors[k]);
		Con_Printf("\n");
	}
	for (const char *s = m->precmds.data; s && *s;) {
		const char *e = strchr(s, '\n');
		int len = e ? (int)(e - s) : (int)strlen(s);
		Con_Printf("-%.*s\n", len, s);
		s = e ? e + 1 : "";
	}
	for (const char *s = m->postcmds.data; s && *s;) {
		const char *e = strchr(s, '\n');
		int len = e ? (int)(e - s) : (int)strlen(s);
		Con_Printf("+%.*s\n", len, s);
		s = e ? e + 1 : "";
	}
}

/* --- the chat filter ---------------------------------------------------- */

typedef struct {
	char word[64], replacement[64];
} filterword_t;

static filterword_t *filter;
static int numfilter;

/* filter.txt: a word per line, optionally followed by what it becomes
 * (stars of the same length otherwise); // comments */
static void load_filter(void)
{
	char *text = FS_LoadFile("filter.txt", NULL);
	Mem_Free(filter);
	filter = NULL;
	numfilter = 0;
	if (!text)
		return;
	int cap = 0;
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	for (char *l = text; l && *l;) {
		char *e = strchr(l, '\n');
		if (e)
			*e = 0;
		Cmd_TokenizeInto(t, l);
		if (t->argc && t->argv[0][0]) {
			if (numfilter == cap) {
				cap = cap ? cap * 2 : 64;
				filter = Mem_Realloc(filter, (size_t)cap * sizeof(*filter));
			}
			Str_Copy(filter[numfilter].word, t->argv[0], sizeof(filter[0].word));
			Str_Copy(filter[numfilter].replacement, t->argc > 1 ? t->argv[1] : "", sizeof(filter[0].replacement));
			numfilter++;
		}
		l = e ? e + 1 : NULL;
	}
	Mem_Free(t);
	Mem_Free(text);
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '\''; }

bool Com_FilterText(char *text, size_t size)
{
	bool changed = false;
	for (int w = 0; w < numfilter; w++) {
		size_t wl = strlen(filter[w].word);
		for (char *p = text; *p;) {
			if (Str_NICmp(p, filter[w].word, wl) || (p > text && word_char(p[-1])) || word_char(p[wl])) {
				p++;
				continue;
			}
			char rep[64];
			if (filter[w].replacement[0])
				Str_Copy(rep, filter[w].replacement, sizeof(rep));
			else {
				size_t n = BW_MIN(wl, sizeof(rep) - 1);
				memset(rep, '*', n);
				rep[n] = 0;
			}
			size_t rl = strlen(rep), tail = strlen(p + wl);
			if ((size_t)(p - text) + rl + tail + 1 > size)
				break;
			memmove(p + rl, p + wl, tail + 1);
			memcpy(p, rep, rl);
			p += rl;
			changed = true;
		}
	}
	return changed;
}

/* com_reloadfilter: reads filter.txt again */
static void ComReloadfilter_f(int argc, const char **argv)
{
	load_filter();
	Con_Printf("%d filtered word%s\n", numfilter, numfilter == 1 ? "" : "s");
}

void Mf_Init(void)
{
	Cvar_Get("com_protocolname", "Bulwark", 0, "The game's name for master servers, from the manifest");
	Cvar_Get("com_fullgamename", "", 0, "The game's display name, from the manifest");
	Cvar_Get("com_gamename", "quake", 0, "The game's code name, from the manifest");
	Cmd_AddD("gamedir", Gamedir_f, "Show or change the mod directory");
	Cmd_AddD("fs_changegame", FsChangegame_f, "Switch between different manifests (or games in the base directory)");
	Cmd_AddD("fs_showmanifest", FsShowmanifest_f, "Show the manifest in effect");
	Cmd_AddD("__fs_apply", FsApply_f, "Internal: finishes a game switch");
	Cmd_AddD("com_reloadfilter", ComReloadfilter_f, "Reload the chat word filter (filter.txt)");
}
