/* Bulwark engine - console variables. ISC licence, see LICENSE. */
#include "cvar.h"
#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>

static cvar_t *cvar_list;
static int cvar_generation;

cvar_t *Cvar_First(void) { return cvar_list; }
int Cvar_Generation(void) { return cvar_generation; }

cvar_t *Cvar_Find(const char *name)
{
	for (cvar_t *v = cvar_list; v; v = v->next)
		if (!Str_ICmp(v->name, name))
			return v;
	return NULL;
}

static void Cvar_Assign(cvar_t *v, const char *value)
{
	char *old = v->string;
	v->string = Mem_Strdup(value);
	v->value = (float)atof(value);
	v->ivalue = atoi(value);
	v->modified++;
	cvar_generation++;
	Mem_Free(old);
}

cvar_t *Cvar_Get(const char *name, const char *defvalue, int flags, const char *description)
{
	cvar_t *v = Cvar_Find(name);
	if (v) {
		/* an engine registration takes over a user-created cvar */
		if (!(flags & CVAR_USERCREATED) && (v->flags & CVAR_USERCREATED)) {
			v->flags = (v->flags & ~CVAR_USERCREATED) | flags;
			Mem_Free(v->defstring);
			v->defstring = Mem_Strdup(defvalue ? defvalue : "");
		}
		if (description && !v->description)
			v->description = Mem_Strdup(description);
		return v;
	}
	v = Mem_Alloc(sizeof(*v));
	v->name = Mem_Strdup(name);
	v->defstring = Mem_Strdup(defvalue ? defvalue : "");
	v->description = description ? Mem_Strdup(description) : NULL;
	v->flags = flags;
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
	Cvar_SetDirect(v, value);
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
		Con_Printf("\"%s\" is \"%s\" (default \"%s\")\n", v->name, v->string, v->defstring);
		return true;
	}
	Cvar_Set(v->name, argv[1]);
	return true;
}

static void Cvar_Set_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: %s <name> <value>\n", argv[0]);
		return;
	}
	if (!Cvar_Find(argv[1]))
		Cvar_Get(argv[1], "", CVAR_USERCREATED | (!strcmp(argv[0], "seta") ? CVAR_ARCHIVE : 0), NULL);
	Cvar_Set(argv[1], argv[2]);
}

static void Cvar_List_f(int argc, const char **argv)
{
	int n = 0;
	for (cvar_t *v = cvar_list; v; v = v->next) {
		if (argc > 1 && !Str_WildMatch(argv[1], v->name, true))
			continue;
		Con_Printf("%s \"%s\"\n", v->name, v->string);
		n++;
	}
	Con_Printf("%d cvars\n", n);
}

void Cvar_Init(void)
{
	Cmd_Add("set", Cvar_Set_f);
	Cmd_Add("seta", Cvar_Set_f);
	Cmd_Add("cvarlist", Cvar_List_f);
}
