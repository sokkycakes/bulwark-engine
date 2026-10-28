/* Bulwark engine - the shared scripting layer: scopes, thinks, inputs,
 * queued EntFire events, game events and the QC builtins that reach them.
 * ISC licence, see LICENSE. The design follows the public description of
 * Valve's VScript; see docs/spec/scripting.md. */
#include "script.h"
#include "../server/sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

qcvm_t *script_vm;

const script_lang_t *script_langs[SCRIPT_MAX_LANGS] = {
#ifdef BW_SCRIPT_LUA
	&script_lang_lua,
#endif
#ifdef BW_SCRIPT_JS
	&script_lang_js,
#endif
	&script_lang_qc,
};

static cvar_t *script_language, *script_debug, *script_timeout;

static bool lang_ready[SCRIPT_MAX_LANGS];
static script_scope_t **scopes; /* by entity number, script_vm->max_edicts long */
static uint32_t *serials;
static int max_ents;
static double script_time;
static int current_caller; /* the entity a delivered EntFire came from */

/* --- temporary strings ------------------------------------------------ */

/* strings handed back from natives and conversions only need to live
 * until the language copies them, so a ring is enough */
#define TEMP_RING 1024
static char *temp_ring[TEMP_RING];
static int temp_next;

const char *Script_TempString(const char *s)
{
	char *copy = Mem_Strdup(s ? s : "");
	Mem_Free(temp_ring[temp_next]);
	temp_ring[temp_next] = copy;
	temp_next = (temp_next + 1) % TEMP_RING;
	return copy;
}

script_value_t Script_String(const char *s)
{
	script_value_t v = {SCRIPT_STRING, {0}};
	v.s = Script_TempString(s);
	return v;
}

static void format_number(double n, char *out, size_t size)
{
	if (n == (double)(long long)n && n > -1e15 && n < 1e15)
		snprintf(out, size, "%lld", (long long)n);
	else
		snprintf(out, size, "%g", n);
}

void Script_ValueToString(const script_value_t *v, char *out, size_t size)
{
	switch (v->type) {
	case SCRIPT_NULL: Str_Copy(out, "", size); break;
	case SCRIPT_BOOL: Str_Copy(out, v->b ? "1" : "0", size); break;
	case SCRIPT_NUMBER: format_number(v->n, out, size); break;
	case SCRIPT_STRING: Str_Copy(out, v->s, size); break;
	case SCRIPT_VECTOR: snprintf(out, size, "%g %g %g", v->v[0], v->v[1], v->v[2]); break;
	case SCRIPT_ENTITY: snprintf(out, size, "%d", v->e); break;
	case SCRIPT_TABLE: snprintf(out, size, "(table: %d keys)", v->t.count); break;
	}
}

bool Script_ValueTruth(const script_value_t *v)
{
	switch (v->type) {
	case SCRIPT_NULL: return false;
	case SCRIPT_BOOL: return v->b;
	case SCRIPT_NUMBER: return v->n != 0;
	case SCRIPT_STRING: return v->s[0] != 0;
	case SCRIPT_ENTITY: return v->e > 0;
	default: return true;
	}
}

void Script_Error(const char *lang, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("^1%s script error: %s\n", lang, buf);
}

/* --- runaway guard ------------------------------------------------------ */

static int call_depth;
static double call_start;

void Script_BeginCall(void)
{
	if (call_depth++ == 0)
		call_start = Sys_Seconds();
}

void Script_EndCall(void) { call_depth--; }

bool Script_TimedOut(void)
{
	return call_depth > 0 && script_timeout->value > 0 && Sys_Seconds() - call_start > script_timeout->value;
}

/* --- argument helpers -------------------------------------------------- */

static bool bad_arg(script_call_t *c, int i, const char *want)
{
	snprintf(c->err, sizeof(c->err), "argument %d: expected %s", i + 1, want);
	return false;
}

bool Script_ArgNumber(script_call_t *c, int i, double *out)
{
	if (i >= c->argc)
		return bad_arg(c, i, "a number");
	if (c->argv[i].type == SCRIPT_NUMBER)
		*out = c->argv[i].n;
	else if (c->argv[i].type == SCRIPT_BOOL)
		*out = c->argv[i].b;
	else if (c->argv[i].type == SCRIPT_STRING)
		*out = atof(c->argv[i].s);
	else
		return bad_arg(c, i, "a number");
	return true;
}

bool Script_ArgString(script_call_t *c, int i, const char **out)
{
	if (i >= c->argc)
		return bad_arg(c, i, "a string");
	if (c->argv[i].type == SCRIPT_STRING) {
		*out = c->argv[i].s;
	} else {
		char buf[256];
		Script_ValueToString(&c->argv[i], buf, sizeof(buf));
		*out = Script_TempString(buf);
	}
	return true;
}

bool Script_ArgVector(script_call_t *c, int i, float *out)
{
	if (i >= c->argc || c->argv[i].type != SCRIPT_VECTOR)
		return bad_arg(c, i, "a Vector");
	VectorCopy(c->argv[i].v, out);
	return true;
}

bool Script_ArgEntity(script_call_t *c, int i, int *out)
{
	if (i >= c->argc || c->argv[i].type != SCRIPT_ENTITY)
		return bad_arg(c, i, "an entity handle");
	if (!Script_EntityLive(c->argv[i].e)) {
		snprintf(c->err, sizeof(c->err), "argument %d: the entity no longer exists", i + 1);
		return false;
	}
	*out = c->argv[i].e;
	return true;
}

double Script_OptNumber(script_call_t *c, int i, double def)
{
	double n;
	if (i >= c->argc || c->argv[i].type == SCRIPT_NULL)
		return def;
	return Script_ArgNumber(c, i, &n) ? n : def;
}

const char *Script_OptString(script_call_t *c, int i, const char *def)
{
	const char *s;
	if (i >= c->argc || c->argv[i].type == SCRIPT_NULL)
		return def;
	return Script_ArgString(c, i, &s) ? s : def;
}

int Script_OptEntity(script_call_t *c, int i)
{
	if (i >= c->argc || c->argv[i].type != SCRIPT_ENTITY || !Script_EntityLive(c->argv[i].e))
		return 0;
	return c->argv[i].e;
}

/* --- languages ----------------------------------------------------------- */

int Script_LangIndex(const script_lang_t *lang)
{
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
		if (script_langs[i] == lang)
			return i;
	return -1;
}

const script_lang_t *Script_FindLang(const char *s)
{
	if (!s || !*s)
		return NULL;
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++) {
		const script_lang_t *l = script_langs[i];
		if (l && (!Str_ICmp(l->name, s) || !Str_ICmp(l->ext, s) || !Str_ICmp(l->ext + 1, s)))
			return l;
	}
	return NULL;
}

static const script_lang_t *default_lang(void)
{
	const script_lang_t *l = Script_FindLang(script_language->string);
	return l ? l : script_langs[0];
}

/* --- entities -------------------------------------------------------- */

uint32_t Script_EntitySerial(int e) { return e >= 0 && e < max_ents ? serials[e] : 0; }

bool Script_EntityLive(int e)
{
	return script_vm && e >= 0 && e < script_vm->num_edicts && !script_vm->edicts[e].free;
}

bool Script_Active(void) { return script_vm != NULL; }

static void scope_release(script_scope_t *s)
{
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
		if (s->data[i]) {
			script_langs[i]->scope_free(s, s->data[i]);
			s->data[i] = NULL;
		}
	Mem_Free(s);
}

/* the entity's slot was freed: drop its scope now, or once its running calls return */
static void edict_freed(qcvm_t *vm, int e)
{
	script_scope_t *s;
	if (vm != script_vm || e <= 0 || e >= max_ents)
		return;
	serials[e]++;
	s = scopes[e];
	if (!s)
		return;
	scopes[e] = NULL;
	if (s->running)
		s->destroyed = true;
	else
		scope_release(s);
}

script_scope_t *Script_GetScope(int e, bool create)
{
	script_scope_t *s;
	if (!script_vm || e < 0 || e >= max_ents)
		return NULL;
	s = scopes[e];
	if (s || !create || !Script_EntityLive(e))
		return s;
	s = Mem_Alloc(sizeof(*s));
	s->ent = e;
	s->serial = serials[e];
	s->inuse = true;
	scopes[e] = s;
	return s;
}

static void *scope_part(script_scope_t *s, const script_lang_t *lang)
{
	int li = Script_LangIndex(lang);
	if (li < 0 || !lang_ready[li])
		return NULL;
	if (!s->data[li]) {
		script_value_t self = Script_Entity(s->ent);
		s->data[li] = lang->scope_new(s);
		if (s->data[li])
			lang->set_var(s, s->data[li], "self", &self);
	}
	return s->data[li];
}

static void scope_enter(script_scope_t *s) { s->running++; }

static void scope_leave(script_scope_t *s)
{
	if (--s->running == 0 && s->destroyed)
		scope_release(s);
}

/* --- running code ------------------------------------------------------- */

static bool run_in(int e, const script_lang_t *lang, const char *code, size_t len, const char *chunk)
{
	script_scope_t *s = Script_GetScope(e, true);
	void *part;
	bool ok;
	if (!s)
		return false;
	if (!(part = scope_part(s, lang))) {
		Script_Error(lang->name, "%s: the language is not available", chunk);
		return false;
	}
	scope_enter(s);
	ok = lang->run(s, part, code, len, chunk);
	scope_leave(s);
	return ok;
}

bool Script_RunCode(int e, const char *langname, const char *code)
{
	const script_lang_t *lang = Script_FindLang(langname);
	if (!lang) {
		/* code for an entity goes to the language its scripts use */
		script_scope_t *s = Script_GetScope(e, false);
		for (int i = 0; s && i < SCRIPT_MAX_LANGS && !lang; i++)
			if (s->data[i] && script_langs[i] != &script_lang_qc)
				lang = script_langs[i];
		if (!lang)
			lang = default_lang();
	}
	return run_in(e, lang, code, strlen(code), "(code)");
}

/* finds a script file: scripts/vscripts/<name> first, then the name as
 * given, trying each language's extension when it has none */
static char *load_script(const char *name, const script_lang_t **lang, char *path, size_t pathsize, size_t *len)
{
	static const char *const dirs[] = {"scripts/vscripts/", ""};
	const char *dot = strrchr(name, '.');
	const script_lang_t *bydot = dot ? Script_FindLang(dot) : NULL;
	if (!FS_SafeName(name))
		return NULL;
	for (int d = 0; d < 2; d++) {
		for (int i = 0; i < SCRIPT_MAX_LANGS; i++) {
			const script_lang_t *l = bydot ? bydot : script_langs[i];
			char *data;
			if (!l)
				continue;
			snprintf(path, pathsize, "%s%s%s", dirs[d], name, bydot ? "" : l->ext);
			if ((data = FS_LoadFile(path, len))) {
				*lang = l;
				return data;
			}
			if (bydot)
				break;
		}
	}
	return NULL;
}

bool Script_RunFile(int e, const char *file)
{
	const script_lang_t *lang;
	char path[256];
	size_t len;
	char *data = load_script(file, &lang, path, sizeof(path), &len);
	bool ok;
	if (!data) {
		Script_Error("script", "couldn't find script %s", file);
		return false;
	}
	if (script_debug->ivalue)
		Con_Printf("script: running %s on entity %d\n", path, e);
	ok = run_in(e, lang, data, len, path);
	Mem_Free(data);
	return ok;
}

bool Script_CallFunction(int e, const char *name, int argc, const script_value_t *argv, script_value_t *ret)
{
	script_scope_t *s = Script_GetScope(e, false);
	bool ok = false;
	if (ret)
		*ret = Script_Null();
	if (!s)
		return false;
	scope_enter(s);
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++) {
		script_value_t r = Script_Null();
		if (!s->data[i] || !script_langs[i]->has_function(s, s->data[i], name))
			continue;
		ok = script_langs[i]->call(s, s->data[i], name, argc, argv, &r);
		if (ret)
			*ret = r;
		break;
	}
	scope_leave(s);
	return ok;
}

static bool has_function(script_scope_t *s, const char *name)
{
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
		if (s->data[i] && script_langs[i]->has_function(s, s->data[i], name))
			return true;
	return false;
}

static void set_vars(script_scope_t *s, int activator, int caller)
{
	script_value_t a = Script_Entity(activator), c = Script_Entity(caller);
	if (!activator)
		a = Script_Null();
	if (!caller)
		c = Script_Null();
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
		if (s->data[i]) {
			script_langs[i]->set_var(s, s->data[i], "activator", &a);
			script_langs[i]->set_var(s, s->data[i], "caller", &c);
		}
}

int Script_AttachScripts(int e, const char *files)
{
	char name[128];
	int count = 0;
	const char *p = files;
	script_scope_t *s;
	while ((p = COM_Parse(p, name, sizeof(name))))
		count += Script_RunFile(e, name);
	/* as in VScript, a freshly loaded scope gets Precache, and OnPostSpawn
	 * once every map entity exists */
	if (count && (s = Script_GetScope(e, false))) {
		if (has_function(s, "Precache"))
			Script_CallFunction(e, "Precache", 0, NULL, NULL);
		if (sv.spawning)
			s->postspawn = true;
		else if ((s = Script_GetScope(e, false)) && has_function(s, "OnPostSpawn"))
			Script_CallFunction(e, "OnPostSpawn", 0, NULL, NULL);
	}
	return count;
}

/* --- thinks -------------------------------------------------------------- */

#define DEFAULT_THINK 0.1

void Script_SetThink(int e, const char *name)
{
	script_scope_t *s = Script_GetScope(e, true);
	if (!s)
		return;
	Str_Copy(s->think, name ? name : "", sizeof(s->think));
	s->nextthink = script_time;
}

static void run_thinks(void)
{
	for (int e = 0; e < script_vm->num_edicts; e++) {
		script_scope_t *s = scopes[e];
		script_value_t r;
		char name[64];
		if (!s || !s->think[0] || s->nextthink > script_time)
			continue;
		Str_Copy(name, s->think, sizeof(name));
		s->nextthink = script_time + DEFAULT_THINK;
		if (!Script_CallFunction(e, name, 0, NULL, &r))
			continue;
		/* a returned number is the delay until the next think */
		if ((s = scopes[e]) && r.type == SCRIPT_NUMBER && r.n > 0)
			s->nextthink = script_time + r.n;
	}
}

/* --- inputs and EntFire -------------------------------------------------- */

bool Script_Input(int e, int activator, int caller, const char *input, const char *data)
{
	script_scope_t *s = Script_GetScope(e, false);
	if (s) {
		char hook[96];
		set_vars(s, activator, caller);
		snprintf(hook, sizeof(hook), "Input%s", input);
		/* a hook that returns false swallows the input */
		if (has_function(s, hook)) {
			script_value_t r;
			if (Script_CallFunction(e, hook, 0, NULL, &r) && r.type == SCRIPT_BOOL && !r.b)
				return true;
		}
	}
	if (!Str_ICmp(input, "RunScriptFile")) {
		Script_RunFile(e, data);
		return true;
	}
	if (!Str_ICmp(input, "RunScriptCode") || !Str_ICmp(input, "RunScriptCodeQuotable")) {
		/* Hammer can't put double quotes in a value, so the quotable form uses ' */
		char *code = Mem_Strdup(data);
		if (input[13])
			for (char *c = code; *c; c++)
				if (*c == '\'')
					*c = '"';
		if (s || (s = Script_GetScope(e, true)))
			set_vars(s, activator, caller);
		Script_RunCode(e, NULL, code);
		Mem_Free(code);
		return true;
	}
	if (!Str_ICmp(input, "CallScriptFunction")) {
		Script_CallFunction(e, data, 0, NULL, NULL);
		return true;
	}
	if (!Str_ICmp(input, "SetThinkFunction") || !Str_ICmp(input, "AddThinkFunction")) {
		Script_SetThink(e, data);
		return true;
	}
	return false;
}

typedef struct entevent_s {
	struct entevent_s *next;
	double time;
	char *target, *input, *value;
	int target_ent; /* > 0 for EntFireByHandle */
	uint32_t target_serial;
	int activator, caller;
	uint32_t activator_serial, caller_serial;
} entevent_t;

static entevent_t *events; /* sorted by time, then by order of firing */

static int keep_ent(int e, uint32_t serial) { return e > 0 && Script_EntityLive(e) && serials[e] == serial ? e : 0; }

static void queue_event(const char *target, int target_ent, const char *input, const char *value, double delay,
                        int activator, int caller)
{
	entevent_t *ev = Mem_Alloc(sizeof(*ev)), **link = &events;
	ev->time = script_time + (delay > 0 ? delay : 0);
	ev->target = Mem_Strdup(target ? target : "");
	ev->input = Mem_Strdup(input ? input : "");
	ev->value = Mem_Strdup(value ? value : "");
	ev->target_ent = target_ent;
	ev->target_serial = Script_EntitySerial(target_ent);
	ev->activator = activator;
	ev->activator_serial = Script_EntitySerial(activator);
	ev->caller = caller;
	ev->caller_serial = Script_EntitySerial(caller);
	while (*link && (*link)->time <= ev->time)
		link = &(*link)->next;
	ev->next = *link;
	*link = ev;
}

void Script_EntFire(const char *target, const char *input, const char *value, double delay, int activator,
                    int caller)
{
	queue_event(target, 0, input, value, delay, activator, caller);
}

void Script_EntFireByHandle(int ent, const char *input, const char *value, double delay, int activator,
                            int caller)
{
	queue_event(NULL, ent, input, value, delay, activator, caller);
}

/* hands an input to the game: Nuclide's sendInput when the progs has it */
static void deliver_input(int e, const char *input, const char *value, int activator, int caller)
{
	qcvm_t *vm = script_vm;
	int fn = QC_FindFunction(vm, "sendInput");
	if (!fn) {
		Script_Input(e, activator, caller, input, value);
		return;
	}
	{
		int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
		int prevcaller = current_caller;
		current_caller = caller;
		vm->globals[OFS_PARM(0)].i = e;
		vm->globals[OFS_PARM(1)].i = QC_NewString(vm, input);
		vm->globals[OFS_PARM(2)].i = QC_NewString(vm, value);
		vm->globals[OFS_PARM(3)].i = activator;
		vm->argc = 4;
		QC_Execute(vm, fn);
		current_caller = prevcaller;
		QC_SetGInt(vm, vm->gofs.self, self);
		QC_SetGInt(vm, vm->gofs.other, other);
	}
}

static bool target_matches(const char *pattern, const char *name)
{
	size_t n = strlen(pattern);
	if (!*name)
		return false;
	/* a trailing * matches any name with that prefix, as Hammer targets do */
	if (n && pattern[n - 1] == '*')
		return !Str_NICmp(pattern, name, n - 1);
	return !Str_ICmp(pattern, name);
}

static void fire_event(entevent_t *ev)
{
	qcvm_t *vm = script_vm;
	int activator = keep_ent(ev->activator, ev->activator_serial);
	int caller = keep_ent(ev->caller, ev->caller_serial);
	int tn = QC_FieldOfs(vm, "targetname");
	if (ev->target_ent) {
		int e = keep_ent(ev->target_ent, ev->target_serial);
		if (e)
			deliver_input(e, ev->input, ev->value, activator, caller);
		return;
	}
	if (!Str_ICmp(ev->target, "!self") || !Str_ICmp(ev->target, "!caller")) {
		if (caller)
			deliver_input(caller, ev->input, ev->value, activator, caller);
		return;
	}
	if (!Str_ICmp(ev->target, "!activator")) {
		if (activator)
			deliver_input(activator, ev->input, ev->value, activator, caller);
		return;
	}
	for (int e = 1; e < vm->num_edicts; e++) {
		const char *name, *cls;
		if (vm->edicts[e].free)
			continue;
		name = tn >= 0 ? QC_GetString(vm, QC_EInt(vm, e, tn)) : "";
		cls = QC_GetString(vm, QC_EInt(vm, e, vm->fofs.classname));
		if (target_matches(ev->target, name) || !Str_ICmp(ev->target, cls))
			deliver_input(e, ev->input, ev->value, activator, caller);
	}
}

static void run_events(void)
{
	/* events fired while these run wait for the next frame, even with no delay */
	double now = script_time;
	while (events && events->time <= now) {
		entevent_t *ev = events;
		events = ev->next;
		fire_event(ev);
		Mem_Free(ev->target);
		Mem_Free(ev->input);
		Mem_Free(ev->value);
		Mem_Free(ev);
		if (!script_vm)
			return; /* a map change from inside an input */
	}
}

static void clear_events(void)
{
	while (events) {
		entevent_t *ev = events;
		events = ev->next;
		Mem_Free(ev->target);
		Mem_Free(ev->input);
		Mem_Free(ev->value);
		Mem_Free(ev);
	}
}

/* --- game events --------------------------------------------------------- */

/* OnGameEvent_<name>(params) runs in every scope that defines it, as in VScript */
void Script_FireGameEvent(const char *name, const script_table_t *params)
{
	char fn[128];
	script_value_t arg = {SCRIPT_TABLE, {0}};
	if (!script_vm)
		return;
	if (params)
		arg.t = *params;
	snprintf(fn, sizeof(fn), "OnGameEvent_%s", name);
	for (int e = 0; e < script_vm->num_edicts; e++)
		if (scopes[e] && has_function(scopes[e], fn))
			Script_CallFunction(e, fn, 1, &arg, NULL);
}

/* "key value key value ..." with console quoting, values that parse as numbers become numbers */
static int parse_keyvalues(const char *text, script_table_t *t, char ***store)
{
	char key[256], value[1024];
	int cap = 0;
	const char *p = text;
	t->count = 0;
	t->keys = NULL;
	t->values = NULL;
	*store = NULL;
	while ((p = COM_Parse(p, key, sizeof(key))) && (p = COM_Parse(p, value, sizeof(value)))) {
		char *end;
		double n = strtod(value, &end);
		if (t->count == cap) {
			cap = cap ? cap * 2 : 8;
			t->keys = Mem_Realloc((void *)t->keys, sizeof(char *) * cap);
			t->values = Mem_Realloc(t->values, sizeof(script_value_t) * cap);
			*store = Mem_Realloc(*store, sizeof(char *) * cap * 2);
		}
		(*store)[t->count * 2] = Mem_Strdup(key);
		(*store)[t->count * 2 + 1] = Mem_Strdup(value);
		t->keys[t->count] = (*store)[t->count * 2];
		if (*value && !*end) {
			t->values[t->count] = Script_Number(n);
		} else {
			t->values[t->count].type = SCRIPT_STRING;
			t->values[t->count].s = (*store)[t->count * 2 + 1];
		}
		t->count++;
	}
	return t->count;
}

static void free_keyvalues(script_table_t *t, char **store)
{
	for (int i = 0; i < t->count * 2; i++)
		Mem_Free(store[i]);
	Mem_Free(store);
	Mem_Free((void *)t->keys);
	Mem_Free(t->values);
}

/* --- level lifecycle ----------------------------------------------------- */

void Script_LevelInit(qcvm_t *vm)
{
	Script_LevelShutdown();
	script_vm = vm;
	max_ents = vm->max_edicts;
	scopes = Mem_Alloc(sizeof(*scopes) * max_ents);
	serials = Mem_Alloc(sizeof(*serials) * max_ents);
	vm->free_hook = edict_freed;
	script_time = 0;
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
		lang_ready[i] = script_langs[i] && script_langs[i]->init();
}

void Script_LevelPreSpawn(void)
{
	char path[256];
	size_t len;
	const script_lang_t *lang;
	/* scripts/vscripts/mapspawn.* runs in the root scope before map entities spawn */
	for (int i = 0; i < SCRIPT_MAX_LANGS; i++) {
		char name[64];
		char *data;
		if (!script_langs[i] || script_langs[i] == &script_lang_qc)
			continue;
		snprintf(name, sizeof(name), "mapspawn%s", script_langs[i]->ext);
		if ((data = load_script(name, &lang, path, sizeof(path), &len))) {
			run_in(0, lang, data, len, path);
			Mem_Free(data);
		}
	}
}

void Script_LevelSpawned(void)
{
	for (int e = 0; e < max_ents; e++) {
		script_scope_t *s = scopes[e];
		if (!s || !s->postspawn)
			continue;
		s->postspawn = false;
		if (has_function(s, "OnPostSpawn"))
			Script_CallFunction(e, "OnPostSpawn", 0, NULL, NULL);
	}
}

void Script_LevelShutdown(void)
{
	if (!script_vm)
		return;
	clear_events();
	for (int e = 0; e < max_ents; e++)
		if (scopes[e]) {
			script_scope_t *s = scopes[e];
			scopes[e] = NULL;
			if (s->running)
				s->destroyed = true;
			else
				scope_release(s);
		}
	for (int i = SCRIPT_MAX_LANGS - 1; i >= 0; i--)
		if (lang_ready[i]) {
			script_langs[i]->shutdown();
			lang_ready[i] = false;
		}
	script_vm->free_hook = NULL;
	Mem_Free(scopes);
	Mem_Free(serials);
	scopes = NULL;
	serials = NULL;
	max_ents = 0;
	script_vm = NULL;
}

void Script_Frame(double time)
{
	if (!script_vm)
		return;
	script_time = time;
	run_events();
	if (script_vm)
		run_thinks();
}

double Script_Time(void) { return script_time; }

/* --- QC builtins ------------------------------------------------------- */

/* float(entity ent, string files) script_attach */
static void bi_script_attach(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	char *files = Mem_Strdup(QC_PSTR(vm, 1));
	QC_ReturnFloat(vm, (float)Script_AttachScripts(e, files));
	Mem_Free(files);
}

/* void(entity ent, string func) script_think */
static void bi_script_think(qcvm_t *vm) { Script_SetThink(QC_PENT(vm, 0), QC_PSTR(vm, 1)); }

/* float(entity ent, entity activator, string input, string data) script_input */
static void bi_script_input(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), activator = QC_PENT(vm, 1);
	char *input = Mem_Strdup(QC_PSTR(vm, 2)), *data = Mem_Strdup(vm->argc > 3 ? QC_PSTR(vm, 3) : "");
	/* an input the engine is delivering keeps the caller it was fired with */
	int caller = current_caller ? current_caller : (vm->argc > 4 ? QC_PENT(vm, 4) : activator);
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	bool used = Script_Input(e, activator, caller, input, data);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	Mem_Free(input);
	Mem_Free(data);
	QC_ReturnFloat(vm, used ? 1 : 0);
}

/* string(entity ent, string func, ...) script_call: up to 6 string, float or
 * entity arguments are passed as strings; the result comes back as text */
static void bi_script_call(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	char *fn = Mem_Strdup(QC_PSTR(vm, 1));
	script_value_t args[6], ret;
	char out[1024];
	int argc = BW_MIN(vm->argc - 2, 6);
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	for (int i = 0; i < argc; i++)
		args[i] = Script_String(QC_PSTR(vm, i + 2));
	Script_CallFunction(e, fn, BW_MAX(argc, 0), args, &ret);
	Script_ValueToString(&ret, out, sizeof(out));
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	Mem_Free(fn);
	QC_ReturnString(vm, out);
}

/* float(entity ent, string code, string lang) script_run; lang "" picks the entity's own */
static void bi_script_run(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	char *code = Mem_Strdup(QC_PSTR(vm, 1)), *lang = Mem_Strdup(vm->argc > 2 ? QC_PSTR(vm, 2) : "");
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	bool ok = Script_RunCode(e, lang, code);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	Mem_Free(code);
	Mem_Free(lang);
	QC_ReturnFloat(vm, ok ? 1 : 0);
}

/* void(string name, string keyvalues) script_event */
static void bi_script_event(qcvm_t *vm)
{
	char *name = Mem_Strdup(QC_PSTR(vm, 0));
	char **store;
	script_table_t t;
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	parse_keyvalues(vm->argc > 1 ? QC_PSTR(vm, 1) : "", &t, &store);
	Script_FireGameEvent(name, &t);
	free_keyvalues(&t, store);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	Mem_Free(name);
}

/* void(string target, string input, string value, float delay, entity activator) script_entfire */
static void bi_script_entfire(qcvm_t *vm)
{
	Script_EntFire(QC_PSTR(vm, 0), QC_PSTR(vm, 1), vm->argc > 2 ? QC_PSTR(vm, 2) : "",
	               vm->argc > 3 ? QC_PFLOAT(vm, 3) : 0, vm->argc > 4 ? QC_PENT(vm, 4) : 0,
	               QC_GInt(vm, vm->gofs.self));
}

const qc_builtin_t script_builtins[] = {
	{"script_attach", bi_script_attach}, {"script_think", bi_script_think}, {"script_input", bi_script_input},
	{"script_call", bi_script_call},     {"script_run", bi_script_run},     {"script_event", bi_script_event},
	{"script_entfire", bi_script_entfire}, {NULL, NULL}};

/* --- console --------------------------------------------------------- */

static bool need_level(void)
{
	if (script_vm)
		return true;
	Con_Printf("no map running\n");
	return false;
}

static void run_console(const char *lang)
{
	const char *code = Cmd_Args();
	if (!need_level())
		return;
	if (!*code) {
		Con_Printf("usage: script%s%s <code>\n", lang ? "_" : "", lang ? lang : "");
		return;
	}
	Script_RunCode(0, lang ? lang : script_language->string, code);
}

static void Script_f(int argc, const char **argv) { run_console(NULL); }
static void ScriptLua_f(int argc, const char **argv) { run_console("lua"); }
static void ScriptJS_f(int argc, const char **argv) { run_console("js"); }

static void ScriptExecute_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: script_execute <file> [entity]\n");
		return;
	}
	if (need_level())
		Script_RunFile(argc > 2 ? atoi(argv[2]) : 0, argv[1]);
}

static void ScriptAttach_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: script_attach <entity> <files>\n");
		return;
	}
	if (need_level() && Script_EntityLive(atoi(argv[1])))
		Script_AttachScripts(atoi(argv[1]), Cmd_Args() + strlen(argv[1]) + 1);
}

static void ScriptEvent_f(int argc, const char **argv)
{
	cmd_tokens_t *t;
	if (argc < 2) {
		Con_Printf("usage: script_event <name> [key value ...]\n");
		return;
	}
	if (!need_level())
		return;
	t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, Cmd_Args());
	{
		char **store;
		script_table_t tab;
		const char *rest = Cmd_Args() + (t->argc > 1 ? t->argstart[1] : strlen(Cmd_Args()));
		parse_keyvalues(rest, &tab, &store);
		Script_FireGameEvent(argv[1], &tab);
		free_keyvalues(&tab, store);
	}
	Mem_Free(t);
}

static void EntFire_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ent_fire <target> <input> [value] [delay]\n");
		return;
	}
	if (need_level())
		Script_EntFire(argv[1], argv[2], argc > 3 ? argv[3] : "", argc > 4 ? atof(argv[4]) : 0, 0, 0);
}

static void ScriptList_f(int argc, const char **argv)
{
	int n = 0;
	if (!need_level())
		return;
	for (int e = 0; e < max_ents; e++) {
		script_scope_t *s = scopes[e];
		char langs[64] = "";
		if (!s)
			continue;
		for (int i = 0; i < SCRIPT_MAX_LANGS; i++)
			if (s->data[i]) {
				Str_Cat(langs, " ", sizeof(langs));
				Str_Cat(langs, script_langs[i]->name, sizeof(langs));
			}
		Con_Printf("%4d %-24s%s%s%s\n", e, QC_GetString(script_vm, QC_EInt(script_vm, e, script_vm->fofs.classname)),
		           langs, s->think[0] ? "  think " : "", s->think);
		n++;
	}
	Con_Printf("%d scopes\n", n);
}

static void ScriptHelp_f(int argc, const char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		for (const script_func_t *f = ns->funcs; f->name; f++) {
			char full[128];
			if (!*ns->name)
				Str_Copy(full, f->name, sizeof(full));
			else if (!strcmp(ns->name, SCRIPT_METHODS))
				snprintf(full, sizeof(full), "ent:%s", f->name);
			else
				snprintf(full, sizeof(full), "%s.%s", ns->name, f->name);
			if (filter && !Str_WildMatch(va("*%s*", filter), full, true))
				continue;
			Con_Printf("^2%s^7(%s)  %s\n", full, f->args, f->help);
		}
}

void Script_Init(void)
{
	script_language = Cvar_Get("script_language", script_langs[0]->name, 0,
	                           "Language for the script command and for RunScriptCode on entities with no scripts");
	script_debug = Cvar_Get("script_debug", "0", 0, "Print each script file as it runs");
	script_timeout = Cvar_Get("script_timeout", "5", 0, "Seconds a single script call may run before it is stopped");
	Cmd_Add("script", Script_f);
	Cmd_Add("script_lua", ScriptLua_f);
	Cmd_Add("script_js", ScriptJS_f);
	Cmd_Add("script_execute", ScriptExecute_f);
	Cmd_Add("script_attach", ScriptAttach_f);
	Cmd_Add("script_event", ScriptEvent_f);
	Cmd_Add("script_list", ScriptList_f);
	Cmd_Add("script_help", ScriptHelp_f);
	Cmd_Add("ent_fire", EntFire_f);
}
