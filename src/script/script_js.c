/* Bulwark engine - JavaScript backend for the scripting layer, on the
 * QuickJS that PanoramaEngine already vendors (MIT). ISC licence, see
 * LICENSE.
 *
 * One JSRuntime serves the level and every scope is its own JSContext, so
 * the globals a script declares belong to its entity, as in VScript. The
 * root scope is the first context. Entity handles are objects of one
 * class whose prototype holds the native methods; Vector is a JS class. */
#include "script.h"

#ifdef BW_SCRIPT_JS

#include "quickjs.h"

#include <string.h>

static JSRuntime *rt;
static JSClassID entity_class;
static const script_func_t **natives; /* by magic number */
static int numnatives;

typedef struct {
	JSContext *ctx;
	script_scope_t *scope;
	JSValue vector_ctor;
	JSValue handles; /* entity number -> handle object */
} jsscope_t;

typedef struct {
	int e;
	uint32_t serial;
} jshandle_t;

static const char prelude[] =
	"class Vector {\n"
	"  constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; }\n"
	"  Add(b) { return new Vector(this.x + b.x, this.y + b.y, this.z + b.z); }\n"
	"  Sub(b) { return new Vector(this.x - b.x, this.y - b.y, this.z - b.z); }\n"
	"  Scale(s) { return new Vector(this.x * s, this.y * s, this.z * s); }\n"
	"  Dot(b) { return this.x * b.x + this.y * b.y + this.z * b.z; }\n"
	"  Cross(b) { return new Vector(this.y * b.z - this.z * b.y, this.z * b.x - this.x * b.z, this.x * b.y - this.y * b.x); }\n"
	"  Length() { return Math.sqrt(this.Dot(this)); }\n"
	"  LengthSqr() { return this.Dot(this); }\n"
	"  Length2D() { return Math.sqrt(this.x * this.x + this.y * this.y); }\n"
	"  Norm() { const l = this.Length(); if (l > 0) { this.x /= l; this.y /= l; this.z /= l; } return l; }\n"
	"  Normalized() { const v = new Vector(this.x, this.y, this.z); v.Norm(); return v; }\n"
	"  ToKVString() { return `${this.x} ${this.y} ${this.z}`; }\n"
	"  toString() { return `Vector(${this.x}, ${this.y}, ${this.z})`; }\n"
	"}\n"
	"globalThis.Vector = Vector;\n"
	"Vector;\n";

static jsscope_t *scope_of(JSContext *ctx) { return JS_GetContextOpaque(ctx); }

/* --- errors -------------------------------------------------------------- */

static void report_exception(JSContext *ctx)
{
	JSValue exc = JS_GetException(ctx);
	const char *msg = JS_ToCString(ctx, exc);
	const char *stack = NULL;
	JSValue st = JS_UNDEFINED;
	if (JS_IsObject(exc)) {
		st = JS_GetPropertyStr(ctx, exc, "stack");
		if (JS_IsString(st))
			stack = JS_ToCString(ctx, st);
	}
	Script_Error("js", "%s%s%s", msg ? msg : "(exception)", stack ? "\n" : "", stack ? stack : "");
	if (stack)
		JS_FreeCString(ctx, stack);
	if (msg)
		JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, st);
	JS_FreeValue(ctx, exc);
}

/* --- conversions --------------------------------------------------------- */

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

static float num_prop(JSContext *ctx, JSValueConst obj, const char *name)
{
	JSValue v = JS_GetPropertyStr(ctx, obj, name);
	double d = 0;
	JS_ToFloat64(ctx, &d, v);
	JS_FreeValue(ctx, v);
	return (float)d;
}

static script_value_t to_value(JSContext *ctx, JSValueConst v, arena_t *a, bool nested)
{
	jsscope_t *s = scope_of(ctx);
	jshandle_t *h;
	if (JS_IsBool(v))
		return Script_Bool(JS_ToBool(ctx, v));
	if (JS_IsNumber(v)) {
		double d = 0;
		JS_ToFloat64(ctx, &d, v);
		return Script_Number(d);
	}
	if (JS_IsString(v)) {
		const char *str = JS_ToCString(ctx, v);
		script_value_t r = Script_String(str);
		JS_FreeCString(ctx, str);
		return r;
	}
	if (!JS_IsObject(v))
		return Script_Null();
	if ((h = JS_GetOpaque(v, entity_class)))
		return Script_Entity(Script_EntityLive(h->e) && Script_EntitySerial(h->e) == h->serial ? h->e : -1);
	if (JS_IsInstanceOf(ctx, v, s->vector_ctor) > 0 || JS_IsArray(v)) {
		float f[3];
		if (JS_IsArray(v)) {
			for (int i = 0; i < 3; i++) {
				JSValue e = JS_GetPropertyUint32(ctx, v, i);
				double d = 0;
				JS_ToFloat64(ctx, &d, e);
				JS_FreeValue(ctx, e);
				f[i] = (float)d;
			}
		} else {
			f[0] = num_prop(ctx, v, "x"), f[1] = num_prop(ctx, v, "y"), f[2] = num_prop(ctx, v, "z");
		}
		return Script_Vector(f);
	}
	if (!nested && a && !JS_IsFunction(ctx, v)) {
		JSPropertyEnum *tab;
		uint32_t len;
		script_value_t r = Script_Null();
		if (JS_GetOwnPropertyNames(ctx, &tab, &len, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
			return r;
		r.type = SCRIPT_TABLE;
		r.t.keys = arena_alloc(a, sizeof(char *) * (len + 1));
		r.t.values = arena_alloc(a, sizeof(script_value_t) * (len + 1));
		r.t.count = 0;
		if (r.t.keys && r.t.values)
			for (uint32_t i = 0; i < len; i++) {
				const char *key = JS_AtomToCString(ctx, tab[i].atom);
				JSValue val = JS_GetProperty(ctx, v, tab[i].atom);
				r.t.keys[r.t.count] = Script_TempString(key);
				r.t.values[r.t.count] = to_value(ctx, val, a, true);
				r.t.count++;
				JS_FreeValue(ctx, val);
				JS_FreeCString(ctx, key);
			}
		JS_FreePropertyEnum(ctx, tab, len);
		if (!r.t.keys || !r.t.values)
			return Script_Null();
		return r;
	}
	return Script_Null();
}

static JSValue new_entity(JSContext *ctx, int e)
{
	jsscope_t *s = scope_of(ctx);
	uint32_t serial = Script_EntitySerial(e);
	JSValue cached = JS_GetPropertyUint32(ctx, s->handles, (uint32_t)e);
	jshandle_t *h = JS_IsObject(cached) ? JS_GetOpaque(cached, entity_class) : NULL;
	JSValue obj;
	if (h && h->serial == serial)
		return cached;
	JS_FreeValue(ctx, cached);
	obj = JS_NewObjectClass(ctx, (int)entity_class);
	h = Mem_Alloc(sizeof(*h));
	h->e = e;
	h->serial = serial;
	JS_SetOpaque(obj, h);
	JS_SetPropertyUint32(ctx, s->handles, (uint32_t)e, JS_DupValue(ctx, obj));
	return obj;
}

static JSValue from_value(JSContext *ctx, const script_value_t *v)
{
	switch (v->type) {
	case SCRIPT_BOOL: return JS_NewBool(ctx, v->b);
	case SCRIPT_NUMBER: return JS_NewNumber(ctx, v->n);
	case SCRIPT_STRING: return JS_NewString(ctx, v->s);
	case SCRIPT_VECTOR: {
		JSValue args[3] = {JS_NewNumber(ctx, v->v[0]), JS_NewNumber(ctx, v->v[1]), JS_NewNumber(ctx, v->v[2])};
		return JS_CallConstructor(ctx, scope_of(ctx)->vector_ctor, 3, args);
	}
	case SCRIPT_ENTITY:
		return v->e >= 0 && Script_EntityLive(v->e) ? new_entity(ctx, v->e) : JS_NULL;
	case SCRIPT_TABLE: {
		JSValue obj = JS_NewObject(ctx);
		for (int i = 0; i < v->t.count; i++)
			JS_SetPropertyStr(ctx, obj, v->t.keys[i], from_value(ctx, &v->t.values[i]));
		return obj;
	}
	default: return JS_NULL;
	}
}

/* --- natives --------------------------------------------------------- */

static JSValue call_native(JSContext *ctx, JSValueConst this_val, bool method, int argc, JSValueConst *argv,
                           int magic)
{
	const script_func_t *f = natives[magic];
	script_value_t args[16];
	script_call_t c;
	arena_t a = {{0}, 0};
	int n = 0;
	bool ok;
	if (method)
		args[n++] = to_value(ctx, this_val, NULL, true);
	for (int i = 0; i < argc && n < 16; i++)
		args[n++] = to_value(ctx, argv[i], &a, false);
	memset(&c, 0, sizeof(c));
	c.argc = n;
	c.argv = args;
	c.scope = scope_of(ctx)->scope;
	ok = f->fn(&c);
	arena_free(&a);
	if (!ok)
		return JS_ThrowTypeError(ctx, "%s: %s", f->name, c.err);
	return c.ret.type == SCRIPT_NULL ? JS_UNDEFINED : from_value(ctx, &c.ret);
}

static JSValue js_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
	return call_native(ctx, this_val, false, argc, argv, magic);
}

static JSValue js_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
	return call_native(ctx, this_val, true, argc, argv, magic);
}

static JSValue js_entity_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	jshandle_t *h = JS_GetOpaque(this_val, entity_class);
	if (!h)
		return JS_NewString(ctx, "(not an entity)");
	if (Script_EntityLive(h->e) && Script_EntitySerial(h->e) == h->serial)
		return JS_NewString(ctx, va("([%d] %s)", h->e,
		                            QC_GetString(script_vm, QC_EInt(script_vm, h->e, script_vm->fofs.classname))));
	return JS_NewString(ctx, va("([%d] removed)", h->e));
}

static void entity_finalizer(JSRuntime *r, JSValueConst val) { Mem_Free(JS_GetOpaque(val, entity_class)); }

static int interrupt(JSRuntime *r, void *opaque) { return Script_TimedOut(); }

static void add_funcs(JSContext *ctx, JSValueConst obj, const script_func_t *funcs, bool method)
{
	for (const script_func_t *f = funcs; f->name; f++) {
		int magic = -1;
		for (int i = 0; i < numnatives; i++)
			if (natives[i] == f)
				magic = i;
		JS_SetPropertyStr(ctx, obj, f->name,
		                  JS_NewCFunctionMagic(ctx, method ? js_method : js_native, f->name, 0,
		                                       JS_CFUNC_generic_magic, magic));
	}
}

/* --- the backend --------------------------------------------------------- */

static bool j_init(void)
{
	static const JSClassDef def = {"CBaseEntity", entity_finalizer, NULL, NULL, NULL};
	rt = JS_NewRuntime();
	if (!rt)
		return false;
	JS_SetMaxStackSize(rt, 1024 * 1024);
	JS_SetInterruptHandler(rt, interrupt, NULL);
	entity_class = 0;
	JS_NewClassID(rt, &entity_class);
	JS_NewClass(rt, entity_class, &def);
	numnatives = 0;
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		for (const script_func_t *f = ns->funcs; f->name; f++)
			numnatives++;
	natives = Mem_Alloc(sizeof(*natives) * numnatives);
	numnatives = 0;
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		for (const script_func_t *f = ns->funcs; f->name; f++)
			natives[numnatives++] = f;
	return true;
}

static void j_shutdown(void)
{
	if (rt) {
		JS_RunGC(rt);
		JS_FreeRuntime(rt);
	}
	rt = NULL;
	Mem_Free(natives);
	natives = NULL;
	numnatives = 0;
}

static void *j_scope_new(script_scope_t *scope)
{
	jsscope_t *s = Mem_Alloc(sizeof(*s));
	JSContext *ctx = JS_NewContext(rt);
	JSValue global, proto, v;
	if (!ctx) {
		Mem_Free(s);
		return NULL;
	}
	s->ctx = ctx;
	s->scope = scope;
	s->vector_ctor = JS_UNDEFINED;
	s->handles = JS_NewObject(ctx);
	JS_SetContextOpaque(ctx, s);

	proto = JS_NewObject(ctx);
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		if (!strcmp(ns->name, SCRIPT_METHODS))
			add_funcs(ctx, proto, ns->funcs, true);
	JS_SetPropertyStr(ctx, proto, "toString", JS_NewCFunction(ctx, js_entity_tostring, "toString", 0));
	JS_SetClassProto(ctx, entity_class, proto);

	global = JS_GetGlobalObject(ctx);
	for (const script_namespace_t *ns = script_api; ns->name; ns++) {
		if (!strcmp(ns->name, SCRIPT_METHODS))
			continue;
		if (!*ns->name) {
			add_funcs(ctx, global, ns->funcs, false);
		} else {
			JSValue obj = JS_NewObject(ctx);
			add_funcs(ctx, obj, ns->funcs, false);
			JS_SetPropertyStr(ctx, global, ns->name, obj);
		}
	}
	JS_FreeValue(ctx, global);

	v = JS_Eval(ctx, prelude, sizeof(prelude) - 1, "<prelude>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v)) {
		report_exception(ctx);
		JS_FreeValue(ctx, s->handles);
		JS_FreeContext(ctx);
		Mem_Free(s);
		return NULL;
	}
	s->vector_ctor = v;
	return s;
}

static void j_scope_free(script_scope_t *scope, void *data)
{
	jsscope_t *s = data;
	JS_FreeValue(s->ctx, s->vector_ctor);
	JS_FreeValue(s->ctx, s->handles);
	JS_FreeContext(s->ctx);
	Mem_Free(s);
}

static bool j_run(script_scope_t *scope, void *data, const char *code, size_t len, const char *chunkname)
{
	jsscope_t *s = data;
	/* QuickJS wants the text NUL terminated */
	char *text = Mem_Alloc(len + 1);
	JSValue v;
	memcpy(text, code, len);
	Script_BeginCall();
	v = JS_Eval(s->ctx, text, len, chunkname, JS_EVAL_TYPE_GLOBAL);
	Script_EndCall();
	Mem_Free(text);
	if (JS_IsException(v)) {
		report_exception(s->ctx);
		return false;
	}
	JS_FreeValue(s->ctx, v);
	return true;
}

static JSValue get_function(jsscope_t *s, const char *name)
{
	JSValue global = JS_GetGlobalObject(s->ctx);
	JSValue fn = JS_GetPropertyStr(s->ctx, global, name);
	JS_FreeValue(s->ctx, global);
	if (!JS_IsFunction(s->ctx, fn)) {
		JS_FreeValue(s->ctx, fn);
		return JS_UNDEFINED;
	}
	return fn;
}

/* natives exist in every context, so they don't count as the scope's functions */
static bool is_native(JSContext *ctx, const char *name)
{
	for (const script_namespace_t *ns = script_api; ns->name; ns++)
		if (!*ns->name)
			for (const script_func_t *f = ns->funcs; f->name; f++)
				if (!strcmp(f->name, name))
					return true;
	return false;
}

static bool j_has_function(script_scope_t *scope, void *data, const char *name)
{
	jsscope_t *s = data;
	JSValue fn;
	if (is_native(s->ctx, name))
		return false;
	fn = get_function(s, name);
	if (JS_IsUndefined(fn))
		return false;
	JS_FreeValue(s->ctx, fn);
	return true;
}

static bool j_call(script_scope_t *scope, void *data, const char *name, int argc, const script_value_t *argv,
                   script_value_t *ret)
{
	jsscope_t *s = data;
	JSValue fn = get_function(s, name), args[16], r;
	argc = BW_MIN(argc, 16);
	if (JS_IsUndefined(fn))
		return false;
	for (int i = 0; i < argc; i++)
		args[i] = from_value(s->ctx, &argv[i]);
	Script_BeginCall();
	r = JS_Call(s->ctx, fn, JS_UNDEFINED, argc, args);
	Script_EndCall();
	for (int i = 0; i < argc; i++)
		JS_FreeValue(s->ctx, args[i]);
	JS_FreeValue(s->ctx, fn);
	if (JS_IsException(r)) {
		report_exception(s->ctx);
		return false;
	}
	*ret = to_value(s->ctx, r, NULL, true);
	JS_FreeValue(s->ctx, r);
	return true;
}

static void j_set_var(script_scope_t *scope, void *data, const char *name, const script_value_t *v)
{
	jsscope_t *s = data;
	JSValue global = JS_GetGlobalObject(s->ctx);
	JS_SetPropertyStr(s->ctx, global, name, from_value(s->ctx, v));
	JS_FreeValue(s->ctx, global);
}

const script_lang_t script_lang_js = {"js",         ".js",  j_init,         j_shutdown, j_scope_new,
                                      j_scope_free, j_run,  j_has_function, j_call,     j_set_var};

#endif
