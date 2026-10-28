/* Bulwark engine - the native API every script language sees. ISC licence,
 * see LICENSE. Names follow the functions Valve's public VScript reference
 * lists, so map scripts read the same; each one is written here from that
 * description against Bulwark's own server. See docs/spec/scripting.md. */
#include "script.h"
#include "../server/sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define VM script_vm
#define F script_vm->fofs

static const char *ent_string(int e, int ofs);

static bool fail(script_call_t *c, const char *msg)
{
	Str_Copy(c->err, msg, sizeof(c->err));
	return false;
}

/* --- QC interop -------------------------------------------------------- */

/* sets up parameters from script values and runs a QC function or builtin */
static bool qc_invoke(script_call_t *c, int first, const char *name)
{
	qcvm_t *vm = VM;
	int fn = QC_FindFunction(vm, name);
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	int argc = c->argc - first;
	bool ok;
	if (fn <= 0)
		return fail(c, va("no QC function named %s", name));
	if (argc > 8)
		return fail(c, "QC functions take at most 8 arguments from scripts");
	for (int i = 0; i < argc; i++) {
		const script_value_t *v = &c->argv[first + i];
		qcval_t *p = &vm->globals[OFS_PARM(i)];
		p[0].i = p[1].i = p[2].i = 0;
		switch (v->type) {
		case SCRIPT_NUMBER: p->f = (float)v->n; break;
		case SCRIPT_BOOL: p->f = v->b; break;
		case SCRIPT_STRING: p->i = QC_NewString(vm, v->s); break;
		case SCRIPT_ENTITY: p->i = v->e > 0 ? v->e : 0; break;
		case SCRIPT_VECTOR: p[0].f = v->v[0], p[1].f = v->v[1], p[2].f = v->v[2]; break;
		default: break;
		}
	}
	vm->argc = argc;
	/* the self of a call made from an entity's scope is that entity */
	QC_SetGInt(vm, vm->gofs.self, c->scope ? c->scope->ent : 0);
	ok = QC_Execute(vm, fn);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	return ok ? true : fail(c, va("QC function %s failed", name));
}

static script_value_t qc_value(uint32_t type, const qcval_t *p)
{
	switch (type & ~DEF_SAVEGLOBAL) {
	case ev_string: return Script_String(QC_GetString(VM, p->i));
	case ev_vector: return Script_Vector(&p->f);
	case ev_entity: return p->i > 0 ? Script_Entity(p->i) : (p->i == 0 ? Script_Entity(0) : Script_Null());
	case ev_integer: case ev_uint: return Script_Number(p->i);
	case ev_function: return Script_String(QC_FunctionName(VM, p->i));
	default: return Script_Number(p->f);
	}
}

static bool qc_store(script_call_t *c, uint32_t type, qcval_t *p, const script_value_t *v)
{
	double n;
	switch (type & ~DEF_SAVEGLOBAL) {
	case ev_string: {
		char buf[1024];
		Script_ValueToString(v, buf, sizeof(buf));
		p->i = QC_NewString(VM, buf);
		return true;
	}
	case ev_vector:
		if (v->type != SCRIPT_VECTOR)
			return fail(c, "this field holds a Vector");
		p[0].f = v->v[0], p[1].f = v->v[1], p[2].f = v->v[2];
		return true;
	case ev_entity:
		if (v->type == SCRIPT_NULL)
			p->i = 0;
		else if (v->type == SCRIPT_ENTITY && v->e >= 0)
			p->i = v->e;
		else
			return fail(c, "this field holds an entity");
		return true;
	case ev_function: {
		int fn;
		if (v->type == SCRIPT_NULL)
			fn = 0;
		else if (v->type != SCRIPT_STRING || (fn = QC_FindFunction(VM, v->s)) <= 0)
			return fail(c, "this field holds a QC function, set it by name");
		p->i = fn;
		return true;
	}
	case ev_integer: case ev_uint:
		if (v->type == SCRIPT_NUMBER) n = v->n;
		else if (v->type == SCRIPT_BOOL) n = v->b;
		else return fail(c, "this field holds a number");
		p->i = (int32_t)n;
		return true;
	default:
		if (v->type == SCRIPT_NUMBER) n = v->n;
		else if (v->type == SCRIPT_BOOL) n = v->b;
		else if (v->type == SCRIPT_NULL) n = 0;
		else return fail(c, "this field holds a number");
		p->f = (float)n;
		return true;
	}
}

static bool qc_call(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name) || !qc_invoke(c, 1, name))
		return false;
	c->ret = Script_Number(VM->globals[OFS_RETURN].f);
	return true;
}

static bool qc_callstring(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name) || !qc_invoke(c, 1, name))
		return false;
	c->ret = Script_String(QC_GetString(VM, VM->globals[OFS_RETURN].i));
	return true;
}

static bool qc_callentity(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name) || !qc_invoke(c, 1, name))
		return false;
	c->ret = qc_value(ev_entity, &VM->globals[OFS_RETURN]);
	return true;
}

static bool qc_callvector(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name) || !qc_invoke(c, 1, name))
		return false;
	c->ret = Script_Vector(&VM->globals[OFS_RETURN].f);
	return true;
}

static bool qc_hasfunction(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name))
		return false;
	c->ret = Script_Bool(QC_FindFunction(VM, name) > 0);
	return true;
}

static bool qc_get(script_call_t *c)
{
	const char *name;
	qcdef_t *d;
	if (!Script_ArgString(c, 0, &name))
		return false;
	if (!(d = QC_FindGlobal(VM, name)))
		return fail(c, va("no QC global named %s", name));
	c->ret = qc_value(d->type, &VM->globals[d->ofs]);
	return true;
}

static bool qc_set(script_call_t *c)
{
	const char *name;
	qcdef_t *d;
	if (!Script_ArgString(c, 0, &name))
		return false;
	if (c->argc < 2)
		return fail(c, "QC.Set needs a value");
	if (!(d = QC_FindGlobal(VM, name)))
		return fail(c, va("no QC global named %s", name));
	return qc_store(c, d->type, &VM->globals[d->ofs], &c->argv[1]);
}

static const script_func_t qc_funcs[] = {
	{"Call", qc_call, "name, ...", "call a QC function or builtin; returns its result as a number"},
	{"CallString", qc_callstring, "name, ...", "call a QC function that returns a string"},
	{"CallEntity", qc_callentity, "name, ...", "call a QC function that returns an entity"},
	{"CallVector", qc_callvector, "name, ...", "call a QC function that returns a vector"},
	{"HasFunction", qc_hasfunction, "name", "true if the server progs defines the function"},
	{"Get", qc_get, "name", "read a QC global, typed by its definition"},
	{"Set", qc_set, "name, value", "write a QC global"},
	{NULL, NULL, NULL, NULL}};

/* --- globals ------------------------------------------------------------- */

static void join_args(script_call_t *c, char *out, size_t size)
{
	out[0] = 0;
	for (int i = 0; i < c->argc; i++) {
		char buf[1024];
		if (c->argv[i].type == SCRIPT_NULL)
			Str_Copy(buf, "null", sizeof(buf));
		else if (c->argv[i].type == SCRIPT_ENTITY)
			snprintf(buf, sizeof(buf), Script_EntityLive(c->argv[i].e) ? "([%d] %s)" : "([%d] removed)", c->argv[i].e,
			         Script_EntityLive(c->argv[i].e) ? ent_string(c->argv[i].e, F.classname) : "");
		else
			Script_ValueToString(&c->argv[i], buf, sizeof(buf));
		if (i)
			Str_Cat(out, " ", size);
		Str_Cat(out, buf, size);
	}
}

static bool g_print(script_call_t *c)
{
	char buf[4096];
	join_args(c, buf, sizeof(buf));
	Con_Print(buf);
	return true;
}

static bool g_printl(script_call_t *c)
{
	char buf[4096];
	join_args(c, buf, sizeof(buf));
	Con_Printf("%s\n", buf);
	return true;
}

static bool g_time(script_call_t *c)
{
	c->ret = Script_Number(sv.time);
	return true;
}

static bool g_frametime(script_call_t *c)
{
	c->ret = Script_Number(sv.frametime);
	return true;
}

static bool g_mapname(script_call_t *c)
{
	c->ret = Script_String(sv.mapname);
	return true;
}

static bool g_randomint(script_call_t *c)
{
	double lo, hi;
	if (!Script_ArgNumber(c, 0, &lo) || !Script_ArgNumber(c, 1, &hi))
		return false;
	if (hi < lo) {
		double t = lo;
		lo = hi, hi = t;
	}
	c->ret = Script_Number(floor(lo + QC_Random(VM) * (floor(hi) - ceil(lo) + 1)));
	if (c->ret.n > hi)
		c->ret.n = floor(hi);
	return true;
}

static bool g_randomfloat(script_call_t *c)
{
	double lo, hi;
	if (!Script_ArgNumber(c, 0, &lo) || !Script_ArgNumber(c, 1, &hi))
		return false;
	c->ret = Script_Number(lo + QC_Random(VM) * (hi - lo));
	return true;
}

static int caller_ent(script_call_t *c) { return c->scope ? c->scope->ent : 0; }

static bool g_entfire(script_call_t *c)
{
	const char *target, *input;
	if (!Script_ArgString(c, 0, &target) || !Script_ArgString(c, 1, &input))
		return false;
	Script_EntFire(target, input, Script_OptString(c, 2, ""), Script_OptNumber(c, 3, 0), Script_OptEntity(c, 4),
	               caller_ent(c));
	return true;
}

static bool g_entfirebyhandle(script_call_t *c)
{
	int e;
	const char *input;
	if (!Script_ArgEntity(c, 0, &e) || !Script_ArgString(c, 1, &input))
		return false;
	Script_EntFireByHandle(e, input, Script_OptString(c, 2, ""), Script_OptNumber(c, 3, 0), Script_OptEntity(c, 4),
	                       c->argc > 5 ? Script_OptEntity(c, 5) : caller_ent(c));
	return true;
}

static bool g_includescript(script_call_t *c)
{
	const char *file;
	int e = caller_ent(c);
	if (!Script_ArgString(c, 0, &file))
		return false;
	if (c->argc > 1 && c->argv[1].type == SCRIPT_ENTITY && !Script_ArgEntity(c, 1, &e))
		return false;
	c->ret = Script_Bool(Script_RunFile(e, file));
	return true;
}

static bool g_addthinktoent(script_call_t *c)
{
	int e;
	if (!Script_ArgEntity(c, 0, &e))
		return false;
	Script_SetThink(e, Script_OptString(c, 1, ""));
	return true;
}

static bool g_firegameevent(script_call_t *c)
{
	const char *name;
	script_table_t empty = {0, NULL, NULL};
	if (!Script_ArgString(c, 0, &name))
		return false;
	Script_FireGameEvent(name, c->argc > 1 && c->argv[1].type == SCRIPT_TABLE ? &c->argv[1].t : &empty);
	return true;
}

static bool g_sendtoconsole(script_call_t *c)
{
	const char *cmd;
	if (!Script_ArgString(c, 0, &cmd))
		return false;
	Cbuf_AddText(cmd);
	Cbuf_AddText("\n");
	return true;
}

static bool g_traceline(script_call_t *c)
{
	float start[3], end[3], zero[3] = {0, 0, 0};
	svtrace_t tr;
	if (!Script_ArgVector(c, 0, start) || !Script_ArgVector(c, 1, end))
		return false;
	SV_Move(start, zero, zero, end, MOVE_NORMAL, Script_OptEntity(c, 2), &tr);
	c->ret = Script_Number(tr.fraction);
	return true;
}

static bool g_precachemodel(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name))
		return false;
	c->ret = Script_Number(SV_ModelIndex(name, true));
	return true;
}

static bool g_precachesound(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 0, &name))
		return false;
	c->ret = Script_Number(SV_SoundIndex(name, true));
	return true;
}

static const script_func_t global_funcs[] = {
	{"print", g_print, "...", "print the arguments to the console"},
	{"printl", g_printl, "...", "print the arguments and a newline"},
	{"Msg", g_print, "...", "same as print"},
	{"Time", g_time, "", "server time in seconds"},
	{"FrameTime", g_frametime, "", "length of a server frame in seconds"},
	{"GetMapName", g_mapname, "", "the running map's name"},
	{"RandomInt", g_randomint, "lo, hi", "random integer in [lo, hi]"},
	{"RandomFloat", g_randomfloat, "lo, hi", "random number in [lo, hi)"},
	{"EntFire", g_entfire, "target, input, value?, delay?, activator?",
	 "queue an input to every entity with that targetname (or classname, !self, !activator)"},
	{"EntFireByHandle", g_entfirebyhandle, "ent, input, value?, delay?, activator?, caller?",
	 "queue an input to one entity"},
	{"DoEntFire", g_entfire, "target, input, value?, delay?, activator?", "same as EntFire"},
	{"IncludeScript", g_includescript, "file, ent?", "run a script file in this scope or in ent's"},
	{"DoIncludeScript", g_includescript, "file, ent?", "same as IncludeScript"},
	{"AddThinkToEnt", g_addthinktoent, "ent, funcname", "run funcname in ent's scope every 0.1s or as it returns"},
	{"FireGameEvent", g_firegameevent, "name, params?", "call OnGameEvent_<name>(params) in every scope"},
	{"SendToConsole", g_sendtoconsole, "command", "queue a console command"},
	{"SendToServerConsole", g_sendtoconsole, "command", "same as SendToConsole"},
	{"TraceLine", g_traceline, "start, end, ignore?", "fraction of the line that is clear"},
	{"PrecacheModel", g_precachemodel, "name", "precache a model; returns its index"},
	{"PrecacheSound", g_precachesound, "name", "precache a sound; returns its index"},
	{NULL, NULL, NULL, NULL}};

/* --- Entities ------------------------------------------------------------ */

static int start_after(script_call_t *c, int i)
{
	return i < c->argc && c->argv[i].type == SCRIPT_ENTITY && c->argv[i].e > 0 ? c->argv[i].e : 0;
}

static const char *ent_string(int e, int ofs) { return ofs < 0 ? "" : QC_GetString(VM, QC_EInt(VM, e, ofs)); }

static void ret_ent(script_call_t *c, int e) { c->ret = e > 0 ? Script_Entity(e) : Script_Null(); }

static int find_next(int after, int field, const char *value)
{
	for (int e = after + 1; e < VM->num_edicts; e++) {
		if (VM->edicts[e].free)
			continue;
		if (field < 0 || !Str_ICmp(ent_string(e, field), value))
			return e;
		/* names ending in * match by prefix */
		{
			size_t n = strlen(value);
			if (n && value[n - 1] == '*' && !Str_NICmp(ent_string(e, field), value, n - 1))
				return e;
		}
	}
	return 0;
}

static bool e_first(script_call_t *c)
{
	ret_ent(c, find_next(0, -1, NULL));
	return true;
}

static bool e_next(script_call_t *c)
{
	ret_ent(c, find_next(start_after(c, 0), -1, NULL));
	return true;
}

static bool e_findbyname(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 1, &name))
		return false;
	ret_ent(c, find_next(start_after(c, 0), QC_FieldOfs(VM, "targetname"), name));
	return true;
}

static bool e_findbyclassname(script_call_t *c)
{
	const char *name;
	if (!Script_ArgString(c, 1, &name))
		return false;
	ret_ent(c, find_next(start_after(c, 0), F.classname, name));
	return true;
}

static float dist2(int e, const float *org)
{
	const float *o = &QC_EdictFields(VM, e)[F.origin].f;
	float d[3] = {o[0] - org[0], o[1] - org[1], o[2] - org[2]};
	return DotProduct(d, d);
}

static bool e_findinsphere(script_call_t *c)
{
	float org[3];
	double r;
	if (!Script_ArgVector(c, 1, org) || !Script_ArgNumber(c, 2, &r))
		return false;
	for (int e = start_after(c, 0) + 1; e < VM->num_edicts; e++)
		if (!VM->edicts[e].free && dist2(e, org) <= r * r) {
			ret_ent(c, e);
			return true;
		}
	c->ret = Script_Null();
	return true;
}

static bool nearest(script_call_t *c, int field)
{
	const char *name;
	float org[3];
	double r;
	int best = 0;
	float bestd = 0;
	if (!Script_ArgString(c, 0, &name) || !Script_ArgVector(c, 1, org) || !Script_ArgNumber(c, 2, &r))
		return false;
	for (int e = find_next(0, field, name); e; e = find_next(e, field, name)) {
		float d = dist2(e, org);
		if (d <= r * r && (!best || d < bestd))
			best = e, bestd = d;
	}
	ret_ent(c, best);
	return true;
}

static bool e_findbyclassnamenearest(script_call_t *c) { return nearest(c, F.classname); }
static bool e_findbynamenearest(script_call_t *c) { return nearest(c, QC_FieldOfs(VM, "targetname")); }

/* spawns an entity and runs its class's spawn function, as map loading does */
static bool e_createbyclassname(script_call_t *c)
{
	qcvm_t *vm = VM;
	const char *cls;
	char sfname[128];
	int e, fn, self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	if (!Script_ArgString(c, 0, &cls))
		return false;
	snprintf(sfname, sizeof(sfname), "spawnfunc_%s", cls);
	if (!(fn = QC_FindFunction(vm, sfname)) && !(fn = QC_FindFunction(vm, cls)))
		return fail(c, va("no spawn function for %s", cls));
	e = SV_Spawn();
	QC_EdictFields(vm, e)[F.classname].i = QC_NewString(vm, cls);
	QC_SetGInt(vm, vm->gofs.self, e);
	QC_SetGInt(vm, vm->gofs.other, 0);
	QC_Execute(vm, fn);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	ret_ent(c, Script_EntityLive(e) ? e : 0);
	return true;
}

static const script_func_t entities_funcs[] = {
	{"First", e_first, "", "the first entity after the world"},
	{"Next", e_next, "prev", "the entity after prev"},
	{"FindByName", e_findbyname, "prev, name", "next entity with that targetname (a trailing * matches a prefix)"},
	{"FindByClassname", e_findbyclassname, "prev, classname", "next entity of that class"},
	{"FindInSphere", e_findinsphere, "prev, origin, radius", "next entity whose origin is within radius"},
	{"FindByClassnameNearest", e_findbyclassnamenearest, "classname, origin, radius", "closest entity of a class"},
	{"FindByNameNearest", e_findbynamenearest, "name, origin, radius", "closest entity with a targetname"},
	{"CreateByClassname", e_createbyclassname, "classname", "spawn an entity and run its spawn function"},
	{NULL, NULL, NULL, NULL}};

/* --- Convars ------------------------------------------------------------- */

static bool cv_getstr(script_call_t *c)
{
	const char *name;
	cvar_t *v;
	if (!Script_ArgString(c, 0, &name))
		return false;
	v = Cvar_Find(name);
	c->ret = v ? Script_String(v->string) : Script_Null();
	return true;
}

static bool cv_getfloat(script_call_t *c)
{
	const char *name;
	cvar_t *v;
	if (!Script_ArgString(c, 0, &name))
		return false;
	v = Cvar_Find(name);
	c->ret = v ? Script_Number(v->value) : Script_Null();
	return true;
}

static bool cv_getbool(script_call_t *c)
{
	const char *name;
	cvar_t *v;
	if (!Script_ArgString(c, 0, &name))
		return false;
	v = Cvar_Find(name);
	c->ret = Script_Bool(v && v->value != 0);
	return true;
}

static bool cv_setvalue(script_call_t *c)
{
	const char *name, *value;
	if (!Script_ArgString(c, 0, &name) || !Script_ArgString(c, 1, &value))
		return false;
	Cvar_Set(name, value);
	return true;
}

static const script_func_t convars_funcs[] = {
	{"GetStr", cv_getstr, "name", "a cvar's value as a string, null if there is no such cvar"},
	{"GetFloat", cv_getfloat, "name", "a cvar's value as a number"},
	{"GetInt", cv_getfloat, "name", "same as GetFloat"},
	{"GetBool", cv_getbool, "name", "true if the cvar is set and not 0"},
	{"SetValue", cv_setvalue, "name, value", "set a cvar"},
	{NULL, NULL, NULL, NULL}};

/* --- entity handle methods ------------------------------------------------ */

#define SELF(c, e)                                                                                             \
	int e;                                                                                                   \
	if (!Script_ArgEntity(c, 0, &e))                                                                         \
		return false

static bool get_vec(script_call_t *c, int ofs)
{
	SELF(c, e);
	if (ofs < 0)
		return fail(c, "the game has no such field");
	c->ret = Script_Vector(&QC_EdictFields(VM, e)[ofs].f);
	return true;
}

static bool set_vec(script_call_t *c, int ofs, bool relink)
{
	float v[3];
	SELF(c, e);
	if (!Script_ArgVector(c, 1, v))
		return false;
	if (ofs < 0)
		return fail(c, "the game has no such field");
	VectorCopy(v, &QC_EdictFields(VM, e)[ofs].f);
	if (relink)
		SV_LinkEdict(e, false);
	return true;
}

static bool get_num(script_call_t *c, int ofs)
{
	SELF(c, e);
	c->ret = Script_Number(QC_EFloat(VM, e, ofs));
	return true;
}

static bool set_num(script_call_t *c, int ofs)
{
	double n;
	SELF(c, e);
	if (!Script_ArgNumber(c, 1, &n))
		return false;
	QC_SetEFloat(VM, e, ofs, (float)n);
	return true;
}

static bool m_getorigin(script_call_t *c) { return get_vec(c, F.origin); }
static bool m_setorigin(script_call_t *c) { return set_vec(c, F.origin, true); }
static bool m_getangles(script_call_t *c) { return get_vec(c, F.angles); }
static bool m_setangles(script_call_t *c) { return set_vec(c, F.angles, false); }
static bool m_getvelocity(script_call_t *c) { return get_vec(c, F.velocity); }
static bool m_setvelocity(script_call_t *c) { return set_vec(c, F.velocity, false); }
static bool m_getmins(script_call_t *c) { return get_vec(c, F.mins); }
static bool m_getmaxs(script_call_t *c) { return get_vec(c, F.maxs); }
static bool m_gethealth(script_call_t *c) { return get_num(c, F.health); }
static bool m_sethealth(script_call_t *c) { return set_num(c, F.health); }
static bool m_getmaxhealth(script_call_t *c) { return get_num(c, QC_FieldOfs(VM, "max_health")); }
static bool m_setmaxhealth(script_call_t *c) { return set_num(c, QC_FieldOfs(VM, "max_health")); }

static bool m_getclassname(script_call_t *c)
{
	SELF(c, e);
	c->ret = Script_String(ent_string(e, F.classname));
	return true;
}

static bool m_getname(script_call_t *c)
{
	SELF(c, e);
	c->ret = Script_String(ent_string(e, QC_FieldOfs(VM, "targetname")));
	return true;
}

static bool m_setname(script_call_t *c)
{
	const char *name;
	int ofs = QC_FieldOfs(VM, "targetname");
	SELF(c, e);
	if (!Script_ArgString(c, 1, &name))
		return false;
	if (ofs >= 0)
		QC_EdictFields(VM, e)[ofs].i = QC_NewString(VM, name);
	return true;
}

static bool m_getmodelname(script_call_t *c)
{
	SELF(c, e);
	c->ret = Script_String(ent_string(e, F.model));
	return true;
}

static bool m_getowner(script_call_t *c)
{
	SELF(c, e);
	ret_ent(c, QC_EInt(VM, e, F.owner));
	return true;
}

static bool m_setowner(script_call_t *c)
{
	SELF(c, e);
	QC_SetEInt(VM, e, F.owner, Script_OptEntity(c, 1));
	return true;
}

static bool direction(script_call_t *c, int which)
{
	float dirs[3][3];
	SELF(c, e);
	AngleVectors(&QC_EdictFields(VM, e)[F.angles].f, dirs[0], dirs[1], dirs[2]);
	if (which == 1) /* left is minus right */
		for (int i = 0; i < 3; i++)
			dirs[1][i] = -dirs[1][i];
	c->ret = Script_Vector(dirs[which]);
	return true;
}

static bool m_forward(script_call_t *c) { return direction(c, 0); }
static bool m_left(script_call_t *c) { return direction(c, 1); }
static bool m_up(script_call_t *c) { return direction(c, 2); }

static bool m_isvalid(script_call_t *c)
{
	c->ret = Script_Bool(c->argc > 0 && c->argv[0].type == SCRIPT_ENTITY && Script_EntityLive(c->argv[0].e));
	return true;
}

static bool m_entindex(script_call_t *c)
{
	SELF(c, e);
	c->ret = Script_Number(e);
	return true;
}

static bool m_destroy(script_call_t *c)
{
	SELF(c, e);
	if (e == 0)
		return fail(c, "the world can't be destroyed");
	SV_UnlinkEdict(e);
	QC_FreeEdict(VM, e, (float)sv.time);
	return true;
}

static bool m_getfield(script_call_t *c)
{
	const char *name;
	qcdef_t *d;
	SELF(c, e);
	if (!Script_ArgString(c, 1, &name))
		return false;
	if (!(d = QC_FindField(VM, name))) {
		c->ret = Script_Null();
		return true;
	}
	c->ret = qc_value(d->type, &QC_EdictFields(VM, e)[d->ofs]);
	return true;
}

static bool m_setfield(script_call_t *c)
{
	const char *name;
	qcdef_t *d;
	SELF(c, e);
	if (!Script_ArgString(c, 1, &name))
		return false;
	if (c->argc < 3)
		return fail(c, "SetField needs a value");
	if (!(d = QC_FindField(VM, name)))
		return fail(c, va("the game has no field named %s", name));
	if (!qc_store(c, d->type, &QC_EdictFields(VM, e)[d->ofs], &c->argv[2]))
		return false;
	if (d->ofs == (uint32_t)F.origin)
		SV_LinkEdict(e, false);
	return true;
}

static bool m_callscriptfunction(script_call_t *c)
{
	const char *name;
	SELF(c, e);
	if (!Script_ArgString(c, 1, &name))
		return false;
	if (!Script_CallFunction(e, name, c->argc - 2, c->argv + 2, &c->ret))
		c->ret = Script_Null();
	return true;
}

static bool m_validatescriptscope(script_call_t *c)
{
	SELF(c, e);
	c->ret = Script_Bool(Script_GetScope(e, true) != NULL);
	return true;
}

static bool m_fireinput(script_call_t *c)
{
	const char *input;
	SELF(c, e);
	if (!Script_ArgString(c, 1, &input))
		return false;
	Script_EntFireByHandle(e, input, Script_OptString(c, 2, ""), Script_OptNumber(c, 3, 0), Script_OptEntity(c, 4),
	                       caller_ent(c));
	return true;
}

static const script_func_t method_funcs[] = {
	{"GetOrigin", m_getorigin, "", "position"},
	{"GetAbsOrigin", m_getorigin, "", "same as GetOrigin"},
	{"SetOrigin", m_setorigin, "vec", "move the entity and relink it"},
	{"SetAbsOrigin", m_setorigin, "vec", "same as SetOrigin"},
	{"GetAngles", m_getangles, "", "pitch yaw roll"},
	{"SetAngles", m_setangles, "vec", "set pitch yaw roll"},
	{"GetVelocity", m_getvelocity, "", "velocity"},
	{"SetVelocity", m_setvelocity, "vec", "set velocity"},
	{"GetBoundingMins", m_getmins, "", "mins"},
	{"GetBoundingMaxs", m_getmaxs, "", "maxs"},
	{"GetHealth", m_gethealth, "", "health"},
	{"SetHealth", m_sethealth, "n", "set health"},
	{"GetMaxHealth", m_getmaxhealth, "", "max_health"},
	{"SetMaxHealth", m_setmaxhealth, "n", "set max_health"},
	{"GetClassname", m_getclassname, "", "classname"},
	{"GetName", m_getname, "", "targetname"},
	{"SetName", m_setname, "name", "set targetname"},
	{"GetModelName", m_getmodelname, "", "model"},
	{"GetOwner", m_getowner, "", "owner, or null"},
	{"SetOwner", m_setowner, "ent", "set owner"},
	{"GetForwardVector", m_forward, "", "unit vector the entity faces"},
	{"GetLeftVector", m_left, "", "unit vector to its left"},
	{"GetUpVector", m_up, "", "unit vector up from it"},
	{"IsValid", m_isvalid, "", "false once the entity is gone"},
	{"entindex", m_entindex, "", "entity number"},
	{"Destroy", m_destroy, "", "remove the entity now"},
	{"Kill", m_destroy, "", "same as Destroy"},
	{"GetField", m_getfield, "name", "read any QC field, typed by its definition; null if there is none"},
	{"SetField", m_setfield, "name, value", "write any QC field"},
	{"CallScriptFunction", m_callscriptfunction, "name, ...",
	 "call a function in this entity's scope, whatever language it is in"},
	{"ValidateScriptScope", m_validatescriptscope, "", "make sure the entity has a scope"},
	{"FireInput", m_fireinput, "input, value?, delay?, activator?", "queue an input to this entity"},
	{NULL, NULL, NULL, NULL}};

const script_namespace_t script_api[] = {
	{"", global_funcs},
	{"Entities", entities_funcs},
	{"Convars", convars_funcs},
	{"QC", qc_funcs},
	{SCRIPT_METHODS, method_funcs},
	{NULL, NULL}};
