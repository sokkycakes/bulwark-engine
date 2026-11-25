/* Bulwark engine - console variables. ISC licence, see LICENSE. */
#include "cvar.h"
#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>

static cvar_t *cvar_list;

cvar_t *Cvar_First(void) { return cvar_list; }

cvar_t *Cvar_Find(const char *name)
{
	for (cvar_t *v = cvar_list; v; v = v->next)
		if (!Str_ICmp(v->name, name))
			return v;
	return NULL;
}

int cvar_generation;
int Cvar_Generation(void) { return cvar_generation; }
int cfg_modified;
static cvar_t *developer_cvar, *sv_cheats;

static void Cvar_Assign(cvar_t *v, const char *value)
{
	cvar_generation++;
	char *old = v->string;
	if (v->flags & CVAR_WATCHED)
		Con_Printf("cvarwatch: %s changed from \"%s\" to \"%s\"\n", v->name, old ? old : "", value);
	if (v->flags & CVAR_ARCHIVE)
		cfg_modified++;
	v->string = Mem_Strdup(value);
	v->value = (float)atof(value);
	v->ivalue = atoi(value);
	v->modified++;
	v->changed_at = cvar_generation;
	Mem_Free(old);
	if (v == developer_cvar)
		developer = v->ivalue;
}

cvar_t *Cvar_Get(const char *name, const char *defvalue, int flags, const char *description)
{
	cvar_t *v = Cvar_Find(name);
	if (v) {
		/* an engine registration takes over a user-created cvar */
		if (!(flags & CVAR_USERCREATED) && (v->flags & CVAR_USERCREATED)) {
			v->flags = (v->flags & ~CVAR_USERCREATED) | flags;
			Mem_Free(v->defstring);
			Mem_Free(v->engdefault);
			v->defstring = Mem_Strdup(defvalue ? defvalue : "");
			v->engdefault = Mem_Strdup(v->defstring);
		}
		if (description && !v->description)
			v->description = Mem_Strdup(description);
		return v;
	}
	v = Mem_Alloc(sizeof(*v));
	v->name = Mem_Strdup(name);
	v->defstring = Mem_Strdup(defvalue ? defvalue : "");
	v->engdefault = Mem_Strdup(v->defstring);
	v->description = description ? Mem_Strdup(description) : NULL;
	v->flags = flags;
	v->setlevel = RESTRICT_MAX;
	Cvar_Assign(v, v->defstring);
	v->modified = 0;
	v->next = cvar_list;
	cvar_list = v;
	return v;
}

void Cvar_SetDirect(cvar_t *v, const char *value)
{
	if (!strcmp(v->string, value))
		return;
	Cvar_Assign(v, value);
}

void Cvar_Set(const char *name, const char *value)
{
	cvar_t *v = Cvar_Find(name);
	if (!v)
		v = Cvar_Get(name, "", CVAR_USERCREATED, NULL);
	if (v->flags & CVAR_READONLY) {
		Con_Printf("%s is read-only\n", name);
		return;
	}
	if (v->restriction > Cmd_ExecLevel() && strcmp(v->string, value)) {
		Con_Printf("%s was restricted (level %d, you have %d)\n", name, v->restriction, Cmd_ExecLevel());
		return;
	}
	if ((v->flags & CVAR_CHEAT) && sv_cheats && !sv_cheats->ivalue && strcmp(v->string, value)) {
		Con_Printf("%s is cheat protected, set sv_cheats 1 first\n", name);
		return;
	}
	Cvar_SetDirect(v, value);
	v->setlevel = Cmd_ExecLevel();
}

void Cvar_SetValue(const char *name, float value)
{
	char buf[64];
	if (value == (int)value)
		snprintf(buf, sizeof(buf), "%d", (int)value);
	else
		snprintf(buf, sizeof(buf), "%g", value);
	Cvar_Set(name, buf);
}

float Cvar_Value(const char *name)
{
	cvar_t *v = Cvar_Find(name);
	return v ? v->value : 0;
}

const char *Cvar_String(const char *name)
{
	cvar_t *v = Cvar_Find(name);
	return v ? v->string : "";
}

bool Cvar_Command(int argc, const char **argv)
{
	cvar_t *v = Cvar_Find(argv[0]);
	if (!v)
		return false;
	if (argc == 1) {
		Con_Printf("\"%s\" is \"%s\" (default \"%s\")%s%s\n", v->name, v->string, v->defstring,
		           v->description ? " - " : "", v->description ? v->description : "");
		return true;
	}
	Cvar_Set(v->name, argv[1]);
	return true;
}

void Cvar_Reset(cvar_t *v)
{
	if (!(v->flags & CVAR_READONLY))
		Cvar_SetDirect(v, v->defstring);
}

const char *Cvar_FlagNames(int flags)
{
	static char buf[128];
	static const struct {
		int flag;
		const char *name;
	} names[] = {{CVAR_ARCHIVE, "archive"}, {CVAR_SERVERINFO, "serverinfo"}, {CVAR_USERINFO, "userinfo"},
	             {CVAR_READONLY, "readonly"}, {CVAR_USERCREATED, "user"}, {CVAR_CHEAT, "cheat"},
	             {CVAR_WATCHED, "watched"}, {CVAR_NOSAVE, "nosave"}};
	buf[0] = 0;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(names); i++)
		if (flags & names[i].flag) {
			if (buf[0])
				Str_Cat(buf, " ", sizeof(buf));
			Str_Cat(buf, names[i].name, sizeof(buf));
		}
	return buf;
}

/* a value as a quoted console token */
static void write_quoted(strbuf_t *sb, const char *s)
{
	SB_AppendStr(sb, "\"");
	for (; *s; s++)
		if (*s != '"' && *s != '\n')
			SB_Append(sb, s, 1);
	SB_AppendStr(sb, "\"");
}

void Cvar_WriteArchived(strbuf_t *sb)
{
	/* the list is newest first; write oldest first so a diff stays stable */
	int n = 0, i = 0;
	cvar_t **all;
	for (cvar_t *v = cvar_list; v; v = v->next)
		n++;
	all = Mem_Alloc(sizeof(*all) * (n ? n : 1));
	for (cvar_t *v = cvar_list; v; v = v->next)
		all[i++] = v;
	for (i = n - 1; i >= 0; i--) {
		cvar_t *v = all[i];
		if (!(v->flags & CVAR_ARCHIVE) || (v->flags & (CVAR_READONLY | CVAR_NOSAVE)))
			continue;
		SB_Printf(sb, "seta %s ", v->name);
		write_quoted(sb, v->string);
		SB_AppendStr(sb, "\n");
	}
	Mem_Free(all);
}

/* whether the running text may add these flags to the cvar (and so set it):
 * publishing an engine cvar in userinfo or serverinfo sends its value to the
 * server, which only the player's own text may choose to do */
bool Cvar_MayFlag(cvar_t *v, int flags)
{
	int level = Cmd_ExecLevel();
	if (v->restriction > level ||
	    (level < RESTRICT_MAX && (flags & ~v->flags & (CVAR_USERINFO | CVAR_SERVERINFO)) && !(v->flags & CVAR_USERCREATED))) {
		Con_Printf("%s was restricted (level %d, you have %d)\n", v->name, v->restriction > level ? v->restriction : RESTRICT_MAX, level);
		return false;
	}
	return true;
}

static void Cvar_Set_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: %s <name> <value>\n", argv[0]);
		return;
	}
	{
		/* set name value [description]; seta archives, sets/setu also
		 * publish the value in serverinfo/userinfo like Quake's */
		int flag = 0;
		cvar_t *v;
		if (!Str_ICmp(argv[0], "seta"))
			flag = CVAR_ARCHIVE;
		else if (!Str_ICmp(argv[0], "sets"))
			flag = CVAR_SERVERINFO;
		else if (!Str_ICmp(argv[0], "setu"))
			flag = CVAR_USERINFO;
		v = Cvar_Find(argv[1]);
		if (!v)
			v = Cvar_Get(argv[1], "", CVAR_USERCREATED, argc > 3 ? argv[3] : NULL);
		if (!Cvar_MayFlag(v, flag))
			return;
		if (flag && !(v->flags & flag)) {
			v->flags |= flag;
			if (flag == CVAR_ARCHIVE)
				cfg_modified++;
		}
		Cvar_Set(argv[1], argv[2]);
	}
}

/* cvarlist [-c changed] [-d defaults] [-F flags] [-a archived] [-r raw] [pattern] */
static void Cvar_List_f(int argc, const char **argv)
{
	int n = 0;
	bool changed = false, defaults = false, flags = false, archived = false, raw = false;
	const char *pattern = NULL;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1]) {
			for (const char *f = argv[i] + 1; *f; f++) {
				if (*f == 'c')
					changed = true;
				else if (*f == 'd')
					defaults = true;
				else if (*f == 'F')
					flags = true;
				else if (*f == 'a')
					archived = true;
				else if (*f == 'r')
					raw = true;
				else if (*f == 'h') {
					Con_Printf("usage: cvarlist [-c] [-d] [-F] [-a] [-r] [pattern]\n"
					           "  -c only cvars changed from their defaults\n  -d show defaults\n"
					           "  -F show flags\n  -a only archived cvars\n  -r no total line\n");
					return;
				}
			}
		} else {
			pattern = argv[i];
		}
	}
	for (cvar_t *v = cvar_list; v; v = v->next) {
		if (pattern && !Str_WildMatch(pattern, v->name, true) && !Str_WildMatch(va("%s*", pattern), v->name, true))
			continue;
		if (changed && !strcmp(v->string, v->defstring))
			continue;
		if (archived && !(v->flags & CVAR_ARCHIVE))
			continue;
		Con_Printf("%s \"%s\"", v->name, v->string);
		if (defaults)
			Con_Printf(", default \"%s\"", v->defstring);
		if (flags && v->flags)
			Con_Printf(" [%s]", Cvar_FlagNames(v->flags));
		Con_Printf("\n");
		n++;
	}
	if (!raw)
		Con_Printf("%d cvars\n", n);
}

void Cvar_Init(void)
{
	Cmd_Add("set", Cvar_Set_f);
	Cmd_Add("seta", Cvar_Set_f);
	Cmd_Add("sets", Cvar_Set_f);
	Cmd_Add("setu", Cvar_Set_f);
	developer_cvar = Cvar_Get("developer", developer ? "1" : "0", 0, "Print developer messages (2 and up for more)");
	sv_cheats = Cvar_Get("sv_cheats", "0", CVAR_SERVERINFO, "Allow cheat commands and cheat-protected cvars");
	Cmd_Add("cvarlist", Cvar_List_f);
}

int Cvar_Restriction(const char *name)
{
	cvar_t *v = Cvar_Find(name);
	return v ? v->restriction : -1;
}
