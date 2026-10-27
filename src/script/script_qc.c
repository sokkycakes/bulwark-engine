/* Bulwark engine - QuakeC backend for the scripting layer. ISC licence,
 * see LICENSE.
 *
 * QuakeC is compiled ahead of time, so a QC "script" is a progs file
 * (scripts/vscripts/<name>.dat) added to the server VM as an extra progs
 * (multiprogs, see docs/spec/progs-format.md). Its functions run with self
 * set to the scope's entity. Code strings can't be run, so RunScriptCode on
 * an entity whose only scripts are QC goes to the default language. */
#include "script.h"

#include <string.h>

typedef struct {
	int progs; /* -1 until a progs is loaded into the scope */
} qcscope_t;

static bool q_init(void) { return true; }
static void q_shutdown(void) {}

static void *q_scope_new(script_scope_t *scope)
{
	qcscope_t *s = Mem_Alloc(sizeof(*s));
	s->progs = -1;
	return s;
}

static void q_scope_free(script_scope_t *scope, void *data) { Mem_Free(data); }

static bool q_run(script_scope_t *scope, void *data, const char *code, size_t len, const char *chunkname)
{
	qcscope_t *s = data;
	int n;
	if (len < 8 || !strchr(chunkname, '.')) {
		Script_Error("qc", "%s: QuakeC can't run code text, only compiled .dat files", chunkname);
		return false;
	}
	n = QC_AddProgs(script_vm, chunkname, code, len);
	if (n < 0) {
		Script_Error("qc", "%s: couldn't load the progs", chunkname);
		return false;
	}
	s->progs = n;
	return true;
}

static int find(void *data, const char *name)
{
	qcscope_t *s = data;
	return s->progs < 0 ? 0 : QC_FindFunctionIn(script_vm, s->progs, name);
}

static bool q_has_function(script_scope_t *scope, void *data, const char *name) { return find(data, name) > 0; }

static bool q_call(script_scope_t *scope, void *data, const char *name, int argc, const script_value_t *argv,
                   script_value_t *ret)
{
	qcvm_t *vm = script_vm;
	int fn = find(data, name);
	int self = QC_GInt(vm, vm->gofs.self), other = QC_GInt(vm, vm->gofs.other);
	bool ok;
	if (fn <= 0)
		return false;
	argc = BW_MIN(argc, 8);
	for (int i = 0; i < argc; i++) {
		const script_value_t *v = &argv[i];
		qcval_t *p = &vm->globals[OFS_PARM(i)];
		char buf[1024];
		p[0].i = p[1].i = p[2].i = 0;
		switch (v->type) {
		case SCRIPT_NUMBER: p->f = (float)v->n; break;
		case SCRIPT_BOOL: p->f = v->b; break;
		case SCRIPT_ENTITY: p->i = v->e > 0 ? v->e : 0; break;
		case SCRIPT_VECTOR: p[0].f = v->v[0], p[1].f = v->v[1], p[2].f = v->v[2]; break;
		case SCRIPT_STRING: p->i = QC_NewString(vm, v->s); break;
		case SCRIPT_TABLE: {
			/* QC has no tables: event parameters arrive as "key" "value" text */
			buf[0] = 0;
			for (int k = 0; k < v->t.count; k++) {
				char val[256];
				Script_ValueToString(&v->t.values[k], val, sizeof(val));
				Str_Cat(buf, va("\"%s\" \"%s\" ", v->t.keys[k], val), sizeof(buf));
			}
			p->i = QC_NewString(vm, buf);
			break;
		}
		default: break;
		}
	}
	vm->argc = argc;
	QC_SetGInt(vm, vm->gofs.self, scope->ent);
	QC_SetGInt(vm, vm->gofs.other, 0);
	Script_BeginCall();
	ok = QC_Execute(vm, fn);
	Script_EndCall();
	/* the caller can't know the QC return type, so it gets the number */
	*ret = Script_Number(vm->globals[OFS_RETURN].f);
	QC_SetGInt(vm, vm->gofs.self, self);
	QC_SetGInt(vm, vm->gofs.other, other);
	return ok;
}

/* activator and caller become entity globals of those names, when the progs has them */
static void q_set_var(script_scope_t *scope, void *data, const char *name, const script_value_t *v)
{
	qcscope_t *s = data;
	qcdef_t *d;
	if (s->progs < 0 || !(d = QC_FindGlobalIn(script_vm, s->progs, name)))
		return;
	if ((d->type & ~DEF_SAVEGLOBAL) == ev_entity)
		QC_ProgsGlobals(script_vm, s->progs)[d->ofs].i = v->type == SCRIPT_ENTITY && v->e > 0 ? v->e : 0;
}

const script_lang_t script_lang_qc = {"qc",         ".dat",         q_init, q_shutdown, q_scope_new,
                                      q_scope_free, q_run,          q_has_function, q_call,     q_set_var};
