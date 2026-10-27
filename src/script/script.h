/* Bulwark engine - the shared scripting layer. ISC licence, see LICENSE.
 *
 * Map and game scripts in Lua, JavaScript or QuakeC all go through this
 * layer, which follows the shape of Valve's VScript as its public wiki
 * documents it: every entity can own a script scope, loaded from its
 * "vscripts" key; inputs such as RunScriptCode reach that scope; a think
 * function runs on a timer; and one native API (EntFire, Entities, entity
 * handles, QC interop) is bound once and offered to every language.
 * See docs/spec/scripting.md. */
#ifndef BW_SCRIPT_H
#define BW_SCRIPT_H

#include "../qcvm/qcvm.h"

/* --- values passed between the layer and a language ------------------ */

typedef enum {
	SCRIPT_NULL,
	SCRIPT_BOOL,
	SCRIPT_NUMBER,
	SCRIPT_STRING,
	SCRIPT_VECTOR,
	SCRIPT_ENTITY,
	SCRIPT_TABLE /* string keys only, values are not tables themselves */
} script_type_t;

typedef struct script_value_s script_value_t;

typedef struct {
	int count;
	const char **keys;
	script_value_t *values;
} script_table_t;

struct script_value_s {
	script_type_t type;
	union {
		bool b;
		double n;
		const char *s; /* owned by whoever made the value, valid for the call */
		float v[3];
		int e; /* entity number; a stale handle arrives as -1 */
		script_table_t t;
	};
};

static inline script_value_t Script_Null(void) { script_value_t v = {SCRIPT_NULL, {0}}; return v; }
static inline script_value_t Script_Number(double n) { script_value_t v = {SCRIPT_NUMBER, {0}}; v.n = n; return v; }
static inline script_value_t Script_Bool(bool b) { script_value_t v = {SCRIPT_BOOL, {0}}; v.b = b; return v; }
static inline script_value_t Script_Entity(int e) { script_value_t v = {SCRIPT_ENTITY, {0}}; v.e = e; return v; }
static inline script_value_t Script_Vector(const float *f)
{
	script_value_t v = {SCRIPT_VECTOR, {0}};
	v.v[0] = f[0], v.v[1] = f[1], v.v[2] = f[2];
	return v;
}
/* the string is copied into the layer's temporary pool, so it may come from a stack buffer */
script_value_t Script_String(const char *s);
const char *Script_TempString(const char *s);
/* a value as text, for QC and for printing */
void Script_ValueToString(const script_value_t *v, char *out, size_t size);
bool Script_ValueTruth(const script_value_t *v);

/* --- the native API ---------------------------------------------------- */

typedef struct script_scope_s script_scope_t;

typedef struct {
	int argc;
	const script_value_t *argv;
	script_value_t ret;
	script_scope_t *scope; /* the scope whose code made the call */
	char err[256]; /* set by a native that fails */
} script_call_t;

typedef bool (*script_native_fn)(script_call_t *c);

typedef struct {
	const char *name;
	script_native_fn fn;
	const char *args; /* human readable, for script_help */
	const char *help;
} script_func_t;

/* a group of natives: globals (name ""), a namespace object such as
 * "Entities", or the methods of entity handles ("CBaseEntity"). Methods
 * get the handle as argv[0]. */
typedef struct {
	const char *name;
	const script_func_t *funcs;
} script_namespace_t;

#define SCRIPT_METHODS "CBaseEntity"
extern const script_namespace_t script_api[]; /* NULL-terminated */

/* argument helpers for natives; each sets c->err and returns false on a bad argument */
bool Script_ArgNumber(script_call_t *c, int i, double *out);
bool Script_ArgString(script_call_t *c, int i, const char **out);
bool Script_ArgVector(script_call_t *c, int i, float *out);
bool Script_ArgEntity(script_call_t *c, int i, int *out); /* must be a live entity */
/* optional forms: return the default when the argument is missing or null */
double Script_OptNumber(script_call_t *c, int i, double def);
const char *Script_OptString(script_call_t *c, int i, const char *def);
int Script_OptEntity(script_call_t *c, int i); /* 0 when absent */

/* --- languages --------------------------------------------------------- */

typedef struct script_lang_s {
	const char *name; /* "lua", "js", "qc" */
	const char *ext;  /* ".lua" */
	bool (*init)(void);
	void (*shutdown)(void);
	/* make the language's half of a scope; ent 0 is the root scope */
	void *(*scope_new)(script_scope_t *scope);
	void (*scope_free)(script_scope_t *scope, void *data);
	/* run source text in a scope; prints its own errors */
	bool (*run)(script_scope_t *scope, void *data, const char *code, size_t len, const char *chunkname);
	bool (*has_function)(script_scope_t *scope, void *data, const char *name);
	/* call a function by name; false if it is missing or raised an error */
	bool (*call)(script_scope_t *scope, void *data, const char *name, int argc, const script_value_t *argv,
	             script_value_t *ret);
	/* set a variable in the scope (self, activator, caller) */
	void (*set_var)(script_scope_t *scope, void *data, const char *name, const script_value_t *v);
} script_lang_t;

#define SCRIPT_MAX_LANGS 4

/* one entity's scripts: a part per language that has code in it */
struct script_scope_s {
	int ent;
	uint32_t serial; /* the entity slot's generation when the scope was made */
	void *data[SCRIPT_MAX_LANGS];
	char think[64]; /* think function name, "" for none */
	double nextthink;
	bool inuse;
	bool postspawn; /* OnPostSpawn is due once the map has spawned */
	bool destroyed; /* freed while one of its functions was running */
	int running; /* calls into this scope in progress */
};

extern const script_lang_t *script_langs[SCRIPT_MAX_LANGS]; /* index = language id */
int Script_LangIndex(const script_lang_t *lang);
const script_lang_t *Script_FindLang(const char *name_or_ext);

/* errors raised from inside a script go to the console with the chunk name */
void Script_Error(const char *lang, const char *fmt, ...) BW_PRINTF(2, 3);

/* --- the host side ----------------------------------------------------- */

/* the server VM is the world scripts act on */
extern qcvm_t *script_vm;

void Script_Init(void); /* console commands, cvars; once at startup */
void Script_LevelInit(qcvm_t *vm); /* after the server progs load */
void Script_LevelPreSpawn(void); /* just before the map's entities spawn: runs mapspawn */
void Script_LevelSpawned(void); /* after they have: OnPostSpawn */
void Script_LevelShutdown(void); /* before the server progs are freed */
void Script_Frame(double time); /* thinks and delayed EntFire events */
bool Script_Active(void);

/* entity handles: a slot number is only valid while its generation matches */
uint32_t Script_EntitySerial(int e);
bool Script_EntityLive(int e);

script_scope_t *Script_GetScope(int e, bool create);
/* load a space separated list of script files into an entity's scope */
int Script_AttachScripts(int e, const char *files);
bool Script_RunCode(int e, const char *lang, const char *code);
bool Script_RunFile(int e, const char *file);
/* call a function in whichever language of the entity's scope defines it */
bool Script_CallFunction(int e, const char *name, int argc, const script_value_t *argv, script_value_t *ret);
void Script_SetThink(int e, const char *name);
/* returns true when the input was consumed by the script layer */
bool Script_Input(int e, int activator, int caller, const char *input, const char *data);
void Script_FireGameEvent(const char *name, const script_table_t *params);
/* inputs are queued and delivered at the start of a later frame, even with no delay */
void Script_EntFire(const char *target, const char *input, const char *value, double delay, int activator,
                    int caller);
void Script_EntFireByHandle(int ent, const char *input, const char *value, double delay, int activator,
                            int caller);
double Script_Time(void);

/* runaway guard for the backends: Begin/End bracket a call from the engine
 * into a language, and TimedOut reports when script_timeout has passed */
void Script_BeginCall(void);
void Script_EndCall(void);
bool Script_TimedOut(void);

extern const qc_builtin_t script_builtins[];

/* language backends; Lua and JS are optional at build time (BW_SCRIPT_LUA, BW_SCRIPT_JS) */
extern const script_lang_t script_lang_lua;
extern const script_lang_t script_lang_js;
extern const script_lang_t script_lang_qc;

#endif
