/* Bulwark engine - Lua 5.4 backend for the scripting layer. ISC licence,
 * see LICENSE. Lua itself is MIT licensed and lives in third_party/.
 *
 * One lua_State serves the level. The root scope is _G; an entity's scope
 * is a table whose metatable falls back to _G, used as the _ENV of every
 * chunk run in it, so globals a script defines stay in its own entity.
 * Entities are full userdata (number + slot generation) with the native
 * methods behind their metatable; Vector is a small Lua class. */
#include "script.h"

#ifdef BW_SCRIPT_LUA

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <string.h>

static lua_State *L;
static script_scope_t *cur_scope;
static int vector_mt_ref = LUA_NOREF, handles_ref = LUA_NOREF;

#define ENTITY_MT "bulwark.entity"

typedef struct {
	int ref; /* registry reference to the scope table, LUA_RIDX_GLOBALS for the root */
} luascope_t;

typedef struct {
	int e;
	uint32_t serial;
} luahandle_t;

static const char prelude[] =
	"local V = {}\n"
	"V.__index = V\n"
	"local function new(x, y, z) return setmetatable({x = x or 0, y = y or 0, z = z or 0}, V) end\n"
	"Vector = new\n"
	"V.__add = function(a, b) return new(a.x + b.x, a.y + b.y, a.z + b.z) end\n"
	"V.__sub = function(a, b) return new(a.x - b.x, a.y - b.y, a.z - b.z) end\n"
	"V.__unm = function(a) return new(-a.x, -a.y, -a.z) end\n"
	"V.__mul = function(a, b)\n"
	"  if type(a) == 'number' then return new(b.x * a, b.y * a, b.z * a) end\n"
	"  if type(b) == 'number' then return new(a.x * b, a.y * b, a.z * b) end\n"
	"  return a.x * b.x + a.y * b.y + a.z * b.z\n"
	"end\n"
	"V.__div = function(a, b) return new(a.x / b, a.y / b, a.z / b) end\n"
	"V.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end\n"
	"V.__tostring = function(v) return string.format('Vector(%g, %g, %g)', v.x, v.y, v.z) end\n"
	"function V:Length() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end\n"
	"function V:LengthSqr() return self.x * self.x + self.y * self.y + self.z * self.z end\n"
	"function V:Length2D() return math.sqrt(self.x * self.x + self.y * self.y) end\n"
	"function V:Dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end\n"
	"function V:Cross(b) return new(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z, self.x * b.y - self.y * b.x) end\n"
	"function V:Norm()\n"
	"  local l = self:Length()\n"
	"  if l > 0 then self.x, self.y, self.z = self.x / l, self.y / l, self.z / l end\n"
	"  return l\n"
	"end\n"
	"function V:Normalized() local v = new(self.x, self.y, self.z); v:Norm(); return v end\n"
	"function V:ToKVString() return string.format('%g %g %g', self.x, self.y, self.z) end\n"
	"return V\n";

/* --- conversions --------------------------------------------------------- */

/* tables converted for a native call live until it returns */
typedef struct {
	void *blocks[16];
	int count;
} arena_t;

static void *arena_alloc(arena_t *a, size_t size)
{
	if (a->count == 16)
		return NULL;
	return a->blocks[a->count++] = Mem_Alloc(size);
}

static void arena_free(arena_t *a)
{
	for (int i = 0; i < a->count; i++)
		Mem_Free(a->blocks[i]);
	a->count = 0;
}

static bool is_vector(int idx)
{
	bool yes;
	if (!lua_getmetatable(L, idx))
		return false;
	lua_rawgeti(L, LUA_REGISTRYINDEX, vector_mt_ref);
	yes = lua_rawequal(L, -1, -2);
	lua_pop(L, 2);
	return yes;
}

static float field_num(int idx, const char *k)
{
	float f;
	lua_getfield(L, idx, k);
	f = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);
	return f;
}

static script_value_t to_value(int idx, arena_t *a, bool nested)
{
	script_value_t v = Script_Null();
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
	case LUA_TBOOLEAN: v = Script_Bool(lua_toboolean(L, idx)); break;
	case LUA_TNUMBER: v = Script_Number(lua_tonumber(L, idx)); break;
	case LUA_TSTRING: v = Script_String(lua_tostring(L, idx)); break;
	case LUA_TUSERDATA: {
		luahandle_t *h = luaL_testudata(L, idx, ENTITY_MT);
		if (h)
			v = Script_Entity(Script_EntityLive(h->e) && Script_EntitySerial(h->e) == h->serial ? h->e : -1);
		break;
	}
	case LUA_TTABLE:
		if (is_vector(idx)) {
			float f[3] = {field_num(idx, "x"), field_num(idx, "y"), field_num(idx, "z")};
			v = Script_Vector(f);
		} else if (!nested && a) {
			int n = 0, i = 0;
			lua_pushnil(L);
			while (lua_next(L, idx)) {
				n += lua_type(L, -2) == LUA_TSTRING;
				lua_pop(L, 1);
			}
			v.type = SCRIPT_TABLE;
			v.t.keys = arena_alloc(a, sizeof(char *) * (n + 1));
			v.t.values = arena_alloc(a, sizeof(script_value_t) * (n + 1));
			if (!v.t.keys || !v.t.values)
				return Script_Null();
			lua_pushnil(L);
			while (lua_next(L, idx)) {
				if (lua_type(L, -2) == LUA_TSTRING && i < n) {
					v.t.keys[i] = Script_TempString(lua_tostring(L, -2));
					v.t.values[i] = to_value(-1, a, true);
					i++;
				}
				lua_pop(L, 1);
			}
			v.t.count = i;
		}
		break;
	}
	return v;
}

static void push_vector(const float *f)
{
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, f[0]);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, f[1]);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, f[2]);
	lua_setfield(L, -2, "z");
	lua_rawgeti(L, LUA_REGISTRYINDEX, vector_mt_ref);
	lua_setmetatable(L, -2);
}

/* one userdata per live entity, so handles compare equal with == */
static void push_entity(int e)
{
	uint32_t serial = Script_EntitySerial(e);
	luahandle_t *h;
	lua_rawgeti(L, LUA_REGISTRYINDEX, handles_ref);
	lua_rawgeti(L, -1, e);
	h = luaL_testudata(L, -1, ENTITY_MT);
	if (h && h->serial == serial) {
		lua_remove(L, -2);
		return;
	}
	lua_pop(L, 1);
	h = lua_newuserdatauv(L, sizeof(*h), 0);
	h->e = e;
	h->serial = serial;
	luaL_setmetatable(L, ENTITY_MT);
	lua_pushvalue(L, -1);
	lua_rawseti(L, -3, e);
	lua_remove(L, -2);
}

static void push_value(const script_value_t *v)
{
	switch (v->type) {
	case SCRIPT_BOOL: lua_pushboolean(L, v->b); break;
	case SCRIPT_NUMBER: lua_pushnumber(L, v->n); break;
	case SCRIPT_STRING: lua_pushstring(L, v->s); break;
	case SCRIPT_VECTOR: push_vector(v->v); break;
	case SCRIPT_ENTITY:
		if (v->e >= 0 && Script_EntityLive(v->e))
			push_entity(v->e);
		else
			lua_pushnil(L);
		break;
	case SCRIPT_TABLE:
		lua_createtable(L, 0, v->t.count);
		for (int i = 0; i < v->t.count; i++) {
			push_value(&v->t.values[i]);
			lua_setfield(L, -2, v->t.keys[i]);
		}
		break;
	default: lua_pushnil(L); break;
	}
}

/* --- natives --------------------------------------------------------- */

static int native(lua_State *l)
{
	const script_func_t *f = lua_touserdata(l, lua_upvalueindex(1));
	int argc = lua_gettop(l);
	script_value_t argv[16];
	script_call_t c;
	arena_t a = {{0}, 0};
	bool ok;
	if (argc > 16)
		argc = 16;
	for (int i = 0; i < argc; i++)
		argv[i] = to_value(i + 1, &a, false);
	memset(&c, 0, sizeof(c));
	c.argc = argc;
	c.argv = argv;
	c.scope = cur_scope;
	ok = f->fn(&c);
	arena_free(&a);
	if (!ok)
		return luaL_error(l, "%s: %s", f->name, c.err);
	if (c.ret.type == SCRIPT_NULL)
		return 0;
	push_value(&c.ret);
	return 1;
}

static void register_funcs(const script_func_t *funcs)
{
	for (const script_func_t *f = funcs; f->name; f++) {
		lua_pushlightuserdata(L, (void *)f);
		lua_pushcclosure(L, native, 1);
		lua_setfield(L, -2, f->name);
	}
}

static int entity_tostring(lua_State *l)
{
	luahandle_t *h = luaL_checkudata(l, 1, ENTITY_MT);
	if (Script_EntityLive(h->e) && Script_EntitySerial(h->e) == h->serial)
		lua_pushfstring(l, "([%d] %s)", h->e,
		                QC_GetString(script_vm, QC_EInt(script_vm, h->e, script_vm->fofs.classname)));
	else
		lua_pushfstring(l, "([%d] removed)", h->e);
	return 1;
}

static int entity_eq(lua_State *l)
{
	luahandle_t *a = luaL_checkudata(l, 1, ENTITY_MT), *b = luaL_checkudata(l, 2, ENTITY_MT);
	lua_pushboolean(l, a->e == b->e && a->serial == b->serial);
	return 1;
}

static void timeout_hook(lua_State *l, lua_Debug *ar)
{
	(void)ar;
	if (Script_TimedOut())
		luaL_error(l, "script ran longer than script_timeout");
}

/* --- the backend --------------------------------------------------------- */

static int traceback(lua_State *l)
{
	const char *msg = lua_tostring(l, 1);
	luaL_traceback(l, l, msg ? msg : "(error object is not a string)", 1);
	return 1;
}

static bool pcall(int nargs, int nret)
{
	int base = lua_gettop(L) - nargs;
	int status;
	lua_pushcfunction(L, traceback);
	lua_insert(L, base);
	Script_BeginCall();
	status = lua_pcall(L, nargs, nret, base);
	Script_EndCall();
	lua_remove(L, base);
	if (status != LUA_OK) {
		Script_Error("lua", "%s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return true;
}

static bool l_init(void)
{
	static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},        {LUA_COLIBNAME, luaopen_coroutine},
	                                {LUA_TABLIBNAME, luaopen_table},  {LUA_STRLIBNAME, luaopen_string},
	                                {LUA_MATHLIBNAME, luaopen_math},  {LUA_UTF8LIBNAME, luaopen_utf8},
	                                {NULL, NULL}};
	L = luaL_newstate();
	if (!L)
		return false;
	for (const luaL_Reg *lib = libs; lib->func; lib++) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);
	}
	/* scripts read files through IncludeScript and the engine's filesystem only */
	lua_pushnil(L);
	lua_setglobal(L, "dofile");
	lua_pushnil(L);
	lua_setglobal(L, "loadfile");
	lua_sethook(L, timeout_hook, LUA_MASKCOUNT, 10000);

	/* entity handles */
	luaL_newmetatable(L, ENTITY_MT);
	lua_newtable(L);
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		if (!strcmp(ns->name, SCRIPT_METHODS))
			register_funcs(ns->funcs);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, entity_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushcfunction(L, entity_eq);
	lua_setfield(L, -2, "__eq");
	lua_pop(L, 1);

	lua_newtable(L); /* entity number -> handle, weak so dead handles go */
	lua_newtable(L);
	lua_pushstring(L, "v");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L, -2);
	handles_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	/* globals and namespace tables */
	for (const script_namespace_t *ns = script_api; ns->name; ns++) {
		if (!strcmp(ns->name, SCRIPT_METHODS))
			continue;
		if (!*ns->name) {
			lua_pushglobaltable(L);
			register_funcs(ns->funcs);
			lua_pop(L, 1);
		} else {
			lua_newtable(L);
			register_funcs(ns->funcs);
			lua_setglobal(L, ns->name);
		}
	}

	if (luaL_loadbufferx(L, prelude, sizeof(prelude) - 1, "=prelude", "t") != LUA_OK || !pcall(0, 1)) {
		lua_close(L);
		L = NULL;
		return false;
	}
	vector_mt_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return true;
}

static void l_shutdown(void)
{
	if (L)
		lua_close(L);
	L = NULL;
	vector_mt_ref = handles_ref = LUA_NOREF;
}

static void *l_scope_new(script_scope_t *scope)
{
	luascope_t *s = Mem_Alloc(sizeof(*s));
	if (scope->ent == 0) {
		s->ref = LUA_RIDX_GLOBALS;
		return s;
	}
	lua_newtable(L);
	lua_newtable(L);
	lua_pushglobaltable(L);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	s->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return s;
}

static void l_scope_free(script_scope_t *scope, void *data)
{
	luascope_t *s = data;
	if (L && s->ref != LUA_RIDX_GLOBALS)
		luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
	Mem_Free(s);
}

static void push_scope(void *data) { lua_rawgeti(L, LUA_REGISTRYINDEX, ((luascope_t *)data)->ref); }

static bool l_run(script_scope_t *scope, void *data, const char *code, size_t len, const char *chunkname)
{
	script_scope_t *prev = cur_scope;
	char name[256];
	bool ok;
	snprintf(name, sizeof(name), "@%s", chunkname);
	if (luaL_loadbufferx(L, code, len, name, "t") != LUA_OK) {
		Script_Error("lua", "%s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	push_scope(data);
	if (!lua_setupvalue(L, -2, 1))
		lua_pop(L, 1);
	cur_scope = scope;
	ok = pcall(0, 0);
	cur_scope = prev;
	return ok;
}

/* looks only in the scope itself: a root function is not every entity's hook */
static bool get_function(void *data, const char *name)
{
	push_scope(data);
	lua_pushstring(L, name);
	lua_rawget(L, -2);
	lua_remove(L, -2);
	if (lua_isfunction(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

static bool l_has_function(script_scope_t *scope, void *data, const char *name)
{
	bool yes = get_function(data, name);
	if (yes)
		lua_pop(L, 1);
	return yes;
}

static bool l_call_fn(script_scope_t *scope, void *data, const char *name, int argc, const script_value_t *argv,
                        script_value_t *ret)
{
	script_scope_t *prev = cur_scope;
	bool ok;
	if (!get_function(data, name))
		return false;
	for (int i = 0; i < argc; i++)
		push_value(&argv[i]);
	cur_scope = scope;
	ok = pcall(argc, 1);
	cur_scope = prev;
	if (!ok)
		return false;
	*ret = to_value(-1, NULL, true);
	lua_pop(L, 1);
	return true;
}

static void l_set_var(script_scope_t *scope, void *data, const char *name, const script_value_t *v)
{
	push_scope(data);
	lua_pushstring(L, name);
	push_value(v);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

const script_lang_t script_lang_lua = {"lua",        ".lua",         l_init,    l_shutdown, l_scope_new,
                                       l_scope_free, l_run,          l_has_function, l_call_fn,  l_set_var};

#endif
