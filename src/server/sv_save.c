/* Bulwark engine - saved games.
 *
 * A save is a short text header the engine writes (map, time, the spawn
 * parms, light styles and the rule cvars) followed by the game's own data.
 * Games that define SV_PerformSave and SV_PerformLoad write and read that
 * part themselves through the file handle they are given. For games without
 * them the engine writes every saved global and every entity's fields, and
 * puts them back the way they were.
 *
 * Loading starts the map over first, so precaches and static entities are
 * there, then restores the time and hands the rest to the game.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G sv.vm->gofs
#define F_CLASSNAME_SET(vm, e) (*QC_GetString(vm, QC_EInt(vm, e, (vm)->fofs.classname)) != 0)

#define SAVE_MAGIC "BULWARK SAVEGAME 1"

static cvar_t *sv_autosave, *sv_save_engine;
static bool autosave_pending;
/* player slots whose entities the last load put back, for that map only */
static bool slot_restored[256];
static int restored_spawncount = -1;

static const char *const rule_cvars[] = {"skill", "deathmatch", "coop", "teamplay"};

static bool save_path(const char *name, char *out, size_t outsize)
{
	char rel[256];
	if (!*name || !FS_SafeName(name) || strchr(name, '/') || strchr(name, '\\')) {
		Con_Printf("bad save name \"%s\"\n", name);
		return false;
	}
	snprintf(rel, sizeof(rel), "saves/%s.sav", name);
	return FS_WritePath(rel, out, outsize);
}

/* "quoted" with \" \\ and \n escaped, so any value fits on one line */
static void put_quoted(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		if (*s == '\n') {
			fputs("\\n", f);
			continue;
		}
		fputc(*s, f);
	}
	fputc('"', f);
}

/* the next word or "quoted" string on the line; false at its end */
static bool get_token(const char **p, char *out, size_t outsize)
{
	const char *s = *p;
	size_t n = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s || *s == '\n' || *s == '\r')
		return false;
	if (*s == '"') {
		for (s++; *s && *s != '"'; s++) {
			char c = *s;
			if (c == '\\' && s[1]) {
				c = *++s;
				if (c == 'n')
					c = '\n';
			}
			if (n + 1 < outsize)
				out[n++] = c;
		}
		if (*s == '"')
			s++;
	} else {
		while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
			if (n + 1 < outsize)
				out[n++] = *s++;
			else
				s++;
	}
	out[n] = 0;
	*p = s;
	return true;
}

static const char *next_line(const char *p)
{
	while (*p && *p != '\n')
		p++;
	return *p ? p + 1 : p;
}

/* --- the engine's own dump, for games without the save hooks ---------- */

static bool skip_def(const qcdef_t *d)
{
	const char *name = d->name ? d->name : "";
	size_t n = strlen(name);
	if (!*name || !strncmp(name, "::", 2))
		return true;
	/* a vector is saved whole, not as its _x _y _z parts */
	return n > 2 && name[n - 2] == '_' && strchr("xyz", name[n - 1]);
}

static void global_to_string(qcvm_t *vm, const qcdef_t *d, char *out, size_t outsize)
{
	qcval_t *v = vm->globals + d->ofs;
	switch (d->type & ~DEF_SAVEGLOBAL) {
	case ev_string: Str_Copy(out, QC_GetString(vm, v->i), outsize); break;
	case ev_float: snprintf(out, outsize, "%.9g", v->f); break;
	case ev_vector: snprintf(out, outsize, "%.9g %.9g %.9g", v[0].f, v[1].f, v[2].f); break;
	case ev_function: Str_Copy(out, v->i ? QC_FunctionName(vm, v->i) : "", outsize); break;
	default: snprintf(out, outsize, "%d", v->i); break;
	}
}

static void global_from_string(qcvm_t *vm, const qcdef_t *d, const char *s)
{
	qcval_t *v = vm->globals + d->ofs;
	switch (d->type & ~DEF_SAVEGLOBAL) {
	case ev_string: v->i = QC_NewString(vm, s); break;
	case ev_float: v->f = (float)atof(s); break;
	case ev_vector: sscanf(s, "%f %f %f", &v[0].f, &v[1].f, &v[2].f); break;
	case ev_function: v->i = QC_FindFunction(vm, s); break;
	default: v->i = (int32_t)strtol(s, NULL, 0); break;
	}
}

static void write_generic(FILE *f)
{
	qcvm_t *vm = sv.vm;
	char buf[4096];
	fputs("globals\n{\n", f);
	for (int i = 0; i < vm->numglobaldefs; i++) {
		qcdef_t *d = &vm->globaldefs[i];
		int t = d->type & ~DEF_SAVEGLOBAL;
		if (!(d->type & DEF_SAVEGLOBAL) || skip_def(d) ||
		    (t != ev_string && t != ev_float && t != ev_vector && t != ev_entity && t != ev_function &&
		     t != ev_integer))
			continue;
		global_to_string(vm, d, buf, sizeof(buf));
		put_quoted(f, d->name);
		fputc(' ', f);
		put_quoted(f, buf);
		fputc('\n', f);
	}
	fputs("}\n", f);
	for (int e = 0; e < vm->num_edicts; e++) {
		if (vm->edicts[e].free)
			continue;
		fprintf(f, "edict %d\n{\n", e);
		for (int i = 1; i < vm->numfielddefs; i++) {
			qcdef_t *d = &vm->fielddefs[i];
			if (skip_def(d))
				continue;
			QC_FieldToString(vm, e, d, buf, sizeof(buf));
			if (!buf[0] || !strcmp(buf, "0") || !strcmp(buf, "0 0 0"))
				continue;
			put_quoted(f, d->name);
			fputc(' ', f);
			put_quoted(f, buf);
			fputc('\n', f);
		}
		fputs("}\n", f);
	}
}

/* reads key value lines up to the closing brace; e < 0 means globals */
static const char *read_block(const char *p, int e)
{
	qcvm_t *vm = sv.vm;
	char key[256], value[4096];
	if (*p == '{')
		p = next_line(p);
	while (*p && *p != '}') {
		const char *q = p;
		if (get_token(&q, key, sizeof(key)) && get_token(&q, value, sizeof(value))) {
			if (e < 0) {
				int ofs = QC_GlobalOfs(vm, key);
				for (int i = 0; ofs >= 0 && i < vm->numglobaldefs; i++)
					if ((int)vm->globaldefs[i].ofs == ofs && vm->globaldefs[i].name &&
					    !strcmp(vm->globaldefs[i].name, key)) {
						global_from_string(vm, &vm->globaldefs[i], value);
						break;
					}
			} else {
				qcdef_t *d = QC_FindField(vm, key);
				if (d)
					QC_SetFieldFromString(vm, e, d, value);
			}
		}
		p = next_line(p);
	}
	return *p ? next_line(p) : p;
}

static void read_generic(const char *p)
{
	qcvm_t *vm = sv.vm;
	int restored = 0;
	/* everything the map spawned goes; the save says what exists */
	for (int e = 1; e < vm->num_edicts; e++)
		if (!vm->edicts[e].free) {
			SV_UnlinkEdict(e);
			QC_FreeEdict(vm, e, 0);
		}
	while (*p) {
		char word[64], num[32];
		const char *q = p;
		if (!get_token(&q, word, sizeof(word))) {
			p = next_line(p);
			continue;
		}
		if (!strcmp(word, "globals")) {
			p = read_block(next_line(p), -1);
		} else if (!strcmp(word, "edict") && get_token(&q, num, sizeof(num))) {
			int e = atoi(num);
			if (e < 0 || e >= vm->max_edicts) {
				Con_Printf("^3save: entity %d is past max_edicts\n", e);
				return;
			}
			if (e >= vm->num_edicts)
				vm->num_edicts = e + 1;
			QC_ClearEdict(vm, e);
			p = read_block(next_line(p), e);
			restored++;
		} else {
			p = next_line(p);
		}
	}
	for (int e = 1; e < vm->num_edicts; e++)
		if (!vm->edicts[e].free)
			SV_LinkEdict(e, false);
	Con_DPrintf("save: restored %d entities\n", restored);
}

/* --- saving ------------------------------------------------------------ */

static bool game_running(void)
{
	if (!sv.active) {
		Con_Printf("not playing a game\n");
		return false;
	}
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED)
			return true;
	Con_Printf("can't save before you're in the game\n");
	return false;
}

bool SV_SaveGame(const char *name, const char *comment)
{
	qcvm_t *vm = sv.vm;
	char path[1024];
	int fn = sv_save_engine->ivalue ? 0 : QC_FindFunction(vm, "SV_PerformSave");
	FILE *f;
	if (!save_path(name, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write the save %s\n", name);
		return false;
	}
	/* the parms a level change would carry, so spawning after a load
	 * gives everyone what they had */
	SV_SaveSpawnParms();
	fprintf(f, "%s\n", SAVE_MAGIC);
	fputs("map ", f);
	put_quoted(f, sv.mapname);
	fputs("\ncomment ", f);
	put_quoted(f, comment ? comment : "");
	fprintf(f, "\ntime %.9g\nmaxclients %d\n", sv.time, svs.maxclients);
	for (size_t i = 0; i < sizeof(rule_cvars) / sizeof(rule_cvars[0]); i++) {
		if (!Cvar_Find(rule_cvars[i]))
			continue;
		fprintf(f, "cvar %s ", rule_cvars[i]);
		put_quoted(f, Cvar_String(rule_cvars[i]));
		fputc('\n', f);
	}
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE || !c->have_parms)
			continue;
		fprintf(f, "parms %d", i);
		for (int k = 0; k < MAX_SPAWN_PARMS; k++)
			fprintf(f, " %.9g", c->spawn_parms[k]);
		fputc('\n', f);
	}
	for (int i = 0; i < SV_MAX_LIGHTSTYLES; i++)
		if (sv.lightstyles[i]) {
			fprintf(f, "lightstyle %d ", i);
			put_quoted(f, sv.lightstyles[i]);
			fputc('\n', f);
		}
	if (fn) {
		int h;
		fputs("game\n", f);
		fflush(f);
		if ((h = QC_OpenWriteHandle(vm, f)) < 0) {
			fclose(f);
			Con_Printf("save: no free file handle for the game\n");
			return false;
		}
		QC_SetGFloat(vm, G.time, (float)sv.time);
		vm->globals[OFS_PARM(0)].f = (float)h;
		vm->globals[OFS_PARM(1)].f = (float)vm->num_edicts;
		vm->globals[OFS_PARM(2)].f = (float)svs.maxclients;
		vm->argc = 3;
		QC_Execute(vm, fn);
		/* the game should have closed it; if not, that closes the file too */
		QC_CloseHandle(vm, h);
	} else {
		fputs("engine\n", f);
		write_generic(f);
		fclose(f);
	}
	Con_Printf("saved %s\n", name);
	return true;
}

static void Save_f(int argc, const char **argv)
{
	char comment[256];
	if (argc < 2) {
		Con_Printf("usage: %s <name> [comment]\n", argv[0]);
		return;
	}
	if (!game_running())
		return;
	if (argc > 2)
		Str_Copy(comment, argv[2], sizeof(comment));
	else
		snprintf(comment, sizeof(comment), "%s, %d:%02d", sv.mapname, (int)sv.time / 60, (int)sv.time % 60);
	SV_SaveGame(argv[1], comment);
}

static void Quicksave_f(int argc, const char **argv)
{
	if (game_running())
		SV_SaveGame("quick", "quicksave");
}

static void Autosave_f(int argc, const char **argv)
{
	if (game_running())
		SV_SaveGame("autosave", sv.mapname);
}

/* --- loading ----------------------------------------------------------- */

static char *read_save(const char *name, size_t *len)
{
	char rel[256];
	if (!FS_SafeName(name) || strchr(name, '/')) {
		Con_Printf("bad save name \"%s\"\n", name);
		return NULL;
	}
	snprintf(rel, sizeof(rel), "saves/%s.sav", name);
	return FS_LoadFile(rel, len);
}

static bool load_legacy(const char *name, char *data, size_t len);

bool SV_LoadGame(const char *name)
{
	size_t len;
	char *data = read_save(name, &len);
	const char *p;
	char word[64], value[1024], mapname[64] = "";
	double time = 0;
	bool game_part = false, engine_part = false;
	float parms[64][MAX_SPAWN_PARMS];
	bool have_parms[64] = {0};
	char *styles[SV_MAX_LIGHTSTYLES] = {0};
	qcvm_t *vm;

	if (!data) {
		Con_Printf("no save called %s\n", name);
		return false;
	}
	if (data[0] >= '0' && data[0] <= '9') {
		bool ok = load_legacy(name, data, len);
		Mem_Free(data);
		return ok;
	}
	if (strncmp(data, SAVE_MAGIC, strlen(SAVE_MAGIC))) {
		Con_Printf("%s isn't a Bulwark save\n", name);
		Mem_Free(data);
		return false;
	}
	/* the header, up to "game" or "engine" */
	for (p = next_line(data); *p;) {
		const char *q = p;
		if (!get_token(&q, word, sizeof(word))) {
			p = next_line(p);
			continue;
		}
		p = next_line(p);
		if (!strcmp(word, "game")) {
			game_part = true;
			break;
		}
		if (!strcmp(word, "engine")) {
			engine_part = true;
			break;
		}
		if (!strcmp(word, "map") && get_token(&q, value, sizeof(value)))
			Str_Copy(mapname, value, sizeof(mapname));
		else if (!strcmp(word, "time") && get_token(&q, value, sizeof(value)))
			time = atof(value);
		else if (!strcmp(word, "cvar") && get_token(&q, word, sizeof(word)) && get_token(&q, value, sizeof(value)))
			Cvar_Set(word, value);
		else if (!strcmp(word, "parms") && get_token(&q, value, sizeof(value))) {
			int slot = atoi(value);
			if (slot >= 0 && slot < 64) {
				for (int k = 0; k < MAX_SPAWN_PARMS && get_token(&q, value, sizeof(value)); k++)
					parms[slot][k] = (float)atof(value);
				have_parms[slot] = true;
			}
		} else if (!strcmp(word, "lightstyle") && get_token(&q, value, sizeof(value))) {
			int i = atoi(value);
			if (i >= 0 && i < SV_MAX_LIGHTSTYLES && get_token(&q, value, sizeof(value))) {
				Mem_Free(styles[i]);
				styles[i] = Mem_Strdup(value);
			}
		}
	}
	if (!*mapname || (!game_part && !engine_part)) {
		Con_Printf("%s is damaged\n", name);
		goto fail;
	}

	Con_Printf("loading %s (%s)\n", name, mapname);
	autosave_pending = false;
	for (int i = 0; i < svs.maxclients; i++)
		svs.clients[i].have_parms = false;
	if (!SV_SpawnServer(mapname))
		goto fail;
	vm = sv.vm;
	for (int i = 0; i < svs.maxclients && i < 64; i++) {
		client_t *c = &svs.clients[i];
		if (!have_parms[i])
			continue;
		memcpy(c->spawn_parms, parms[i], sizeof(c->spawn_parms));
		c->have_parms = true;
	}
	for (int i = 0; i < SV_MAX_LIGHTSTYLES; i++)
		if (styles[i]) {
			Mem_Free(sv.lightstyles[i]);
			sv.lightstyles[i] = styles[i];
			styles[i] = NULL;
		}
	sv.time = time;
	QC_SetGFloat(vm, G.time, (float)sv.time);

	if (game_part) {
		int fn = QC_FindFunction(vm, "SV_PerformLoad"), h;
		if (!fn) {
			Con_Printf("^3%s was saved by the game, which has no SV_PerformLoad now\n", name);
		} else if ((h = QC_OpenReadHandle(vm, p, len - (size_t)(p - data))) >= 0) {
			vm->globals[OFS_PARM(0)].f = (float)h;
			vm->globals[OFS_PARM(1)].f = (float)vm->num_edicts;
			vm->globals[OFS_PARM(2)].f = (float)svs.maxclients;
			vm->argc = 3;
			QC_Execute(vm, fn);
			QC_CloseHandle(vm, h);
		}
	} else {
		read_generic(p);
	}
	/* a player entity the save put back is kept as it is, as in Quake:
	 * spawning it again would put the player at the start */
	memset(slot_restored, 0, sizeof(slot_restored));
	restored_spawncount = svs.spawncount;
	for (int i = 0; i < svs.maxclients && i < 256; i++) {
		int e = i + 1;
		slot_restored[i] = !vm->edicts[e].free && F_CLASSNAME_SET(vm, e);
	}
	Mem_Free(data);
	return true;
fail:
	for (int i = 0; i < SV_MAX_LIGHTSTYLES; i++)
		Mem_Free(styles[i]);
	Mem_Free(data);
	return false;
}


/* one line of a legacy save, without its end */
static const char *legacy_line(const char *p, char *out, size_t size)
{
	size_t n = strcspn(p, "\n");
	Str_Copy(out, p, BW_MIN(size, n + 1));
	out[strcspn(out, "\r")] = 0;
	return p[n] ? p + n + 1 : p + n;
}

/* --- the classic single player format (savegame_legacy) -----------------
 * A version line (5), a comment, the 16 spawn parms, skill, map, time, 64
 * lightstyles, then { } blocks of "key" "value" lines: the globals, and one
 * per entity slot from the world on, empty for free slots. */

#define LEGACY_VERSION 5

static void write_legacy_block(FILE *f, int e)
{
	qcvm_t *vm = sv.vm;
	char buf[4096];
	fputs("{\n", f);
	if (e < 0) {
		for (int i = 0; i < vm->numglobaldefs; i++) {
			qcdef_t *d = &vm->globaldefs[i];
			int t = d->type & ~DEF_SAVEGLOBAL;
			if (!(d->type & DEF_SAVEGLOBAL) || skip_def(d) ||
			    (t != ev_string && t != ev_float && t != ev_vector && t != ev_entity && t != ev_function))
				continue;
			global_to_string(vm, d, buf, sizeof(buf));
			put_quoted(f, d->name);
			fputc(' ', f);
			put_quoted(f, buf);
			fputc('\n', f);
		}
	} else if (!vm->edicts[e].free) {
		for (int i = 1; i < vm->numfielddefs; i++) {
			qcdef_t *d = &vm->fielddefs[i];
			if (skip_def(d))
				continue;
			QC_FieldToString(vm, e, d, buf, sizeof(buf));
			if (!buf[0] || !strcmp(buf, "0") || !strcmp(buf, "0 0 0"))
				continue;
			put_quoted(f, d->name);
			fputc(' ', f);
			put_quoted(f, buf);
			fputc('\n', f);
		}
	}
	fputs("}\n", f);
}

static void SaveLegacy_f(int argc, const char **argv)
{
	char path[1024], comment[40];
	qcvm_t *vm = sv.vm;
	client_t *c = &svs.clients[0];
	FILE *f;
	if (svs.maxclients > 1) {
		Con_Printf("Unable to use legacy savegame format to save multiplayer games\nUnable to use legacy save format\n");
		return;
	}
	if (argc < 2) {
		Con_Printf("%s <name>: save in the classic single player format\n", argv[0]);
		return;
	}
	if (!game_running())
		return;
	if (!save_path(argv[1], path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write the save %s\n", argv[1]);
		return;
	}
	SV_SaveSpawnParms();
	{
		int ofs_k = QC_GlobalOfs(vm, "killed_monsters"), ofs_t = QC_GlobalOfs(vm, "total_monsters");
		snprintf(comment, sizeof(comment), "%-22.22s kills:%3d/%3d", sv.mapname,
		         ofs_k >= 0 ? (int)vm->globals[ofs_k].f : 0, ofs_t >= 0 ? (int)vm->globals[ofs_t].f : 0);
		for (char *p = comment; *p; p++)
			if (*p == ' ')
				*p = '_';
	}
	fprintf(f, "%d\n%s\n", LEGACY_VERSION, comment);
	for (int i = 0; i < 16; i++)
		fprintf(f, "%f\n", i < MAX_SPAWN_PARMS ? c->spawn_parms[i] : 0.0f);
	fprintf(f, "%d\n%s\n%f\n", (int)(Cvar_Value("skill") + 0.5f), sv.mapname, sv.time);
	for (int i = 0; i < 64; i++)
		fprintf(f, "%s\n", i < SV_MAX_LIGHTSTYLES && sv.lightstyles[i] ? sv.lightstyles[i] : "m");
	write_legacy_block(f, -1);
	for (int e = 0; e < vm->num_edicts; e++)
		write_legacy_block(f, e);
	fclose(f);
	Con_Printf("saved %s (legacy format)\n", argv[1]);
}

/* loads one: the classic format is told apart by its first line, a number */
static bool load_legacy(const char *name, char *data, size_t len)
{
	const char *p = data;
	char line[256], mapname[64];
	float parms[16];
	char *styles[64] = {0};
	double time;
	qcvm_t *vm;
	int e = -1, version;
#define LINE() (p = legacy_line(p, line, sizeof(line)))
	(void)len;
	LINE();
	version = atoi(line);
	if (version != LEGACY_VERSION) {
		Con_Printf("%s is a version %d save, not %d\n", name, version, LEGACY_VERSION);
		return false;
	}
	LINE(); /* comment */
	for (int i = 0; i < 16; i++) {
		LINE();
		parms[i] = (float)atof(line);
	}
	LINE();
	Cvar_SetValue("skill", (float)atoi(line));
	LINE();
	Str_Copy(mapname, line, sizeof(mapname));
	LINE();
	time = atof(line);
	for (int i = 0; i < 64; i++) {
		LINE();
		styles[i] = Mem_Strdup(line);
	}
	Con_Printf("loading %s (%s, legacy format)\n", name, mapname);
	autosave_pending = false;
	for (int i = 0; i < svs.maxclients; i++)
		svs.clients[i].have_parms = false;
	if (!SV_SpawnServer(mapname)) {
		for (int i = 0; i < 64; i++)
			Mem_Free(styles[i]);
		return false;
	}
	vm = sv.vm;
	memcpy(svs.clients[0].spawn_parms, parms, sizeof(float) * BW_MIN(16, MAX_SPAWN_PARMS));
	svs.clients[0].have_parms = true;
	for (int i = 0; i < 64; i++) {
		if (i < SV_MAX_LIGHTSTYLES) {
			Mem_Free(sv.lightstyles[i]);
			sv.lightstyles[i] = styles[i];
		} else
			Mem_Free(styles[i]);
	}
	sv.time = time;
	QC_SetGFloat(vm, G.time, (float)sv.time);
	/* what the map spawned goes; the blocks say what exists */
	for (int k = 1; k < vm->num_edicts; k++)
		if (!vm->edicts[k].free) {
			SV_UnlinkEdict(k);
			QC_FreeEdict(vm, k, 0);
		}
	while (*p) {
		const char *q = p;
		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
			q++;
		if (*q != '{')
			break;
		if (e >= 0) {
			if (e >= vm->max_edicts) {
				Con_Printf("^3save: entity %d is past max_edicts\n", e);
				break;
			}
			if (e >= vm->num_edicts)
				vm->num_edicts = e + 1;
			QC_ClearEdict(vm, e);
		}
		{
			/* an empty block is a free slot */
			const char *body = next_line(q);
			bool empty = *body == '}';
			p = read_block(q, e);
			if (e > 0 && empty)
				QC_FreeEdict(vm, e, 0);
		}
		e++;
	}
	for (int k = 1; k < vm->num_edicts; k++)
		if (!vm->edicts[k].free)
			SV_LinkEdict(k, false);
	memset(slot_restored, 0, sizeof(slot_restored));
	restored_spawncount = svs.spawncount;
	for (int i = 0; i < svs.maxclients && i < 256; i++)
		slot_restored[i] = !vm->edicts[i + 1].free && F_CLASSNAME_SET(vm, i + 1);
#undef LINE
	return true;
}

static void Load_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: %s <name>\n", argv[0]);
		return;
	}
	SV_LoadGame(argv[1]);
}

static void Quickload_f(int argc, const char **argv)
{
	SV_LoadGame("quick");
}

/* lists saves with their comments */
static void list_save(const char *name, const char *searchpath, void *ctx)
{
	int *n = ctx;
	char base[128], comment[256] = "";
	size_t len;
	const char *slash = strrchr(name, '/');
	char *data;
	Str_Copy(base, slash ? slash + 1 : name, sizeof(base));
	if (strlen(base) > 4)
		base[strlen(base) - 4] = 0;
	if ((data = read_save(base, &len))) {
		for (const char *p = data; *p; p = next_line(p)) {
			const char *q = p;
			char word[32];
			if (get_token(&q, word, sizeof(word)) && !strcmp(word, "comment")) {
				get_token(&q, comment, sizeof(comment));
				break;
			}
			if (!strcmp(word, "game") || !strcmp(word, "engine"))
				break;
		}
		Mem_Free(data);
	}
	Con_Printf("  %-20s %s\n", base, comment);
	(*n)++;
}

static void Listsaves_f(int argc, const char **argv)
{
	int n = 0;
	FS_List("saves/*.sav", true, list_save, &n);
	Con_Printf("%d saves\n", n);
}

static void Unsavegame_f(int argc, const char **argv)
{
	char path[1024];
	if (argc < 2) {
		Con_Printf("usage: %s <name>\n", argv[0]);
		return;
	}
	if (!save_path(argv[1], path, sizeof(path)))
		return;
	if (remove(path))
		Con_Printf("no save called %s\n", argv[1]);
	else
		Con_Printf("deleted %s\n", argv[1]);
}

/* changelevel in a single player game autosaves once the player is in */
void SV_SaveLevelChanged(void)
{
	autosave_pending = sv_autosave->ivalue && svs.maxclients == 1;
}

bool SV_SaveTakeRestored(client_t *c)
{
	int slot = (int)(c - svs.clients);
	bool r = restored_spawncount == svs.spawncount && slot >= 0 && slot < 256 && slot_restored[slot] &&
	         !sv.vm->edicts[c->edict].free;
	if (slot >= 0 && slot < 256)
		slot_restored[slot] = false;
	return r;
}

void SV_SaveClientSpawned(client_t *c)
{
	if (autosave_pending && !c->bot) {
		autosave_pending = false;
		Cbuf_AddText("autosave\n");
	}
}

void SV_InitSaves(void)
{
	sv_autosave = Cvar_Get("sv_autosave", "1", CVAR_ARCHIVE, "Save to \"autosave\" after each level change in single player");
	sv_save_engine = Cvar_Get("sv_save_engine", "0", 0, "Save every global and entity field in the engine instead of asking the game (SV_PerformSave)");
	Cmd_AddD("save", Save_f, "save <name> [comment]: save the game to saves/<name>.sav");
	Cmd_AddD("savegame", Save_f, "savegame <name> [comment]: save the game");
	Cmd_AddD("savegame_legacy", SaveLegacy_f, "savegame_legacy <name>: save in the classic single player format");
	Cmd_AddD("load", Load_f, "load <name>: load saves/<name>.sav");
	Cmd_AddD("loadgame", Load_f, "loadgame <name>: load a saved game");
	Cmd_AddD("quicksave", Quicksave_f, "Save to the \"quick\" slot");
	Cmd_AddD("quickload", Quickload_f, "Load the \"quick\" slot");
	Cmd_AddD("autosave", Autosave_f, "Save to the \"autosave\" slot");
	Cmd_AddD("listsaves", Listsaves_f, "List the saved games");
	Cmd_AddD("unsavegame", Unsavegame_f, "unsavegame <name>: delete a saved game");
	Cmd_AddD("deletesave", Unsavegame_f, "deletesave <name>: delete a saved game");
}
