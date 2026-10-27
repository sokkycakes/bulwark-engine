/* Bulwark engine - autocvars: a global named autocvar_<name> reads the cvar
 * <name>, created with the global's initial value as its default when it
 * doesn't exist yet. Globals follow later changes to the cvar; writes from
 * QC stay local. Behaviour from the fteextensions.qc description of
 * autocvars. ISC licence, see LICENSE. */
#include "qc_internal.h"
#include "../common/cvar.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct qc_autocvar_s {
	cvar_t *cvar;
	int progs;
	uint32_t ofs;
	int type;
	int modified;
} qc_autocvar_t;

static void store(qcvm_t *vm, const qc_autocvar_t *a)
{
	qcval_t *g = QC_ProgsGlobals(vm, a->progs) + a->ofs;
	switch (a->type) {
	case ev_float: g->f = a->cvar->value; break;
	case ev_integer:
	case ev_uint: g->i = a->cvar->ivalue; break;
	case ev_vector: {
		float v[3] = {0, 0, 0};
		sscanf(a->cvar->string, "%f %f %f", &v[0], &v[1], &v[2]);
		g[0].f = v[0], g[1].f = v[1], g[2].f = v[2];
		break;
	}
	case ev_string: g->i = (int32_t)QC_NewString(vm, a->cvar->string); break;
	default: break;
	}
}

void QC_BindAutocvars(qcvm_t *vm, int progs)
{
	qcprogs_t *p = &vm->progs[progs];
	qcval_t *g = QC_ProgsGlobals(vm, progs);
	for (int i = 0; i < p->numglobaldefs; i++) {
		qcdef_t *d = &p->globaldefs[i];
		int type = (int)(d->type & ~DEF_SAVEGLOBAL);
		char def[128];
		qc_autocvar_t *a;
		if (!d->name || strncmp(d->name, "autocvar_", 9) || !d->name[9])
			continue;
		switch (type) {
		case ev_float: snprintf(def, sizeof(def), "%g", g[d->ofs].f); break;
		case ev_integer:
		case ev_uint: snprintf(def, sizeof(def), "%d", g[d->ofs].i); break;
		case ev_vector: snprintf(def, sizeof(def), "%g %g %g", g[d->ofs].f, g[d->ofs + 1].f, g[d->ofs + 2].f); break;
		case ev_string: {
			const char *s = QC_GetString(vm, (string_t)g[d->ofs].i);
			Str_Copy(def, s ? s : "", sizeof(def));
			break;
		}
		default: continue;
		}
		vm->autocvars = Mem_Realloc(vm->autocvars, sizeof(qc_autocvar_t) * (vm->numautocvars + 1));
		a = &vm->autocvars[vm->numautocvars++];
		a->cvar = Cvar_Get(d->name + 9, def, CVAR_USERCREATED, NULL);
		a->progs = progs;
		a->ofs = d->ofs;
		a->type = type;
		a->modified = a->cvar->modified;
		store(vm, a);
	}
	vm->autocvar_gen = cvar_generation;
}

void QC_SyncAutocvars(qcvm_t *vm)
{
	if (vm->autocvar_gen == cvar_generation)
		return;
	vm->autocvar_gen = cvar_generation;
	for (int i = 0; i < vm->numautocvars; i++) {
		qc_autocvar_t *a = &vm->autocvars[i];
		if (a->modified != a->cvar->modified) {
			a->modified = a->cvar->modified;
			store(vm, a);
		}
	}
}
