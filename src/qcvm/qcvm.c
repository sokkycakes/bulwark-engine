/* Bulwark engine - QuakeC virtual machine: loading, entities, memory.
 * The interpreter loop lives in qc_exec.c. ISC licence, see LICENSE. */
#include "qcvm.h"
#include "qc_internal.h"

#include <stdio.h>
#include <stdlib.h>

static qcvm_t *vm_list[16];
static int vm_count;

int QC_VMCount(void) { return vm_count; }
qcvm_t *QC_VMAt(int i) { return vm_list[i]; }

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

const char *QC_ProgsString(qcvm_t *vm, int32_t ofs)
{
	if (ofs <= 0 || ofs >= vm->stringsize)
		return "";
	return vm->strings + ofs;
}

/* --- lookups --------------------------------------------------------- */

static uint32_t hash_name(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

static void build_hash(qc_namehash_t *h, int count, const char *(*getname)(void *, int), void *ctx)
{
	int size = 16;
	while (size < count * 2)
		size *= 2;
	h->size = size;
	h->slots = Mem_Alloc(sizeof(int) * size);
	for (int i = 0; i < size; i++)
		h->slots[i] = -1;
	/* insert in order so the first definition with a name wins */
	for (int i = 0; i < count; i++) {
		const char *n = getname(ctx, i);
		uint32_t k;
		if (!n || !*n)
			continue;
		k = hash_name(n) & (size - 1);
		while (h->slots[k] != -1) {
			if (!strcmp(getname(ctx, h->slots[k]), n))
				break;
			k = (k + 1) & (size - 1);
		}
		if (h->slots[k] == -1)
			h->slots[k] = i;
	}
}

static int find_hash(const qc_namehash_t *h, const char *name, const char *(*getname)(void *, int), void *ctx)
{
	uint32_t k;
	if (!h->size)
		return -1;
	k = hash_name(name) & (h->size - 1);
	while (h->slots[k] != -1) {
		if (!strcmp(getname(ctx, h->slots[k]), name))
			return h->slots[k];
		k = (k + 1) & (h->size - 1);
	}
	return -1;
}

static const char *gdef_name(void *ctx, int i) { return ((qcvm_t *)ctx)->globaldefs[i].name; }
static const char *fdef_name(void *ctx, int i) { return ((qcvm_t *)ctx)->fielddefs[i].name; }
static const char *func_name(void *ctx, int i) { return ((qcvm_t *)ctx)->functions[i].name; }

int QC_FindFunction(qcvm_t *vm, const char *name)
{
	int i = find_hash(&vm->extra->funchash, name, func_name, vm);
	return i > 0 ? i : 0;
}

qcdef_t *QC_FindGlobal(qcvm_t *vm, const char *name)
{
	int i = find_hash(&vm->extra->globalhash, name, gdef_name, vm);
	return i < 0 ? NULL : &vm->globaldefs[i];
}

qcdef_t *QC_FindField(qcvm_t *vm, const char *name)
{
	int i = find_hash(&vm->extra->fieldhash, name, fdef_name, vm);
	return i < 0 ? NULL : &vm->fielddefs[i];
}

int QC_GlobalOfs(qcvm_t *vm, const char *name)
{
	qcdef_t *d = QC_FindGlobal(vm, name);
	return d ? (int)d->ofs : -1;
}

int QC_FieldOfs(qcvm_t *vm, const char *name)
{
	qcdef_t *d = QC_FindField(vm, name);
	return d ? (int)d->ofs : -1;
}

const char *QC_FunctionName(qcvm_t *vm, func_t f)
{
	if (f <= 0 || f >= vm->numfunctions)
		return "(null)";
	return vm->functions[f].name;
}

/* --- loading --------------------------------------------------------- */

static bool section_ok(size_t filesize, int32_t ofs, int32_t count, size_t elemsize)
{
	if (ofs < 0 || count < 0)
		return false;
	return (uint64_t)ofs + (uint64_t)count * elemsize <= filesize;
}

/* Works out the statement and def encoding from the section sizes. The
 * extended format either keeps the 16-bit records of version 6 or doubles
 * them to 32 bits; the gap between consecutive sections tells which. */
static int section_end(const progs_header_t *h, int32_t ofs, size_t filesize)
{
	int32_t offsets[6] = {h->ofs_statements, h->ofs_globaldefs, h->ofs_fielddefs,
	                      h->ofs_functions, h->ofs_strings, h->ofs_globals};
	int32_t best = (int32_t)filesize;
	for (int i = 0; i < 6; i++)
		if (offsets[i] > ofs && offsets[i] < best)
			best = offsets[i];
	return best;
}

static void resolve_offsets(qcvm_t *vm)
{
	qcfieldofs_t *f = &vm->fofs;
	qcglobalofs_t *g = &vm->gofs;
#define FLD(x) f->x = QC_FieldOfs(vm, #x)
	FLD(classname); FLD(model); FLD(modelindex); FLD(origin); FLD(angles); FLD(velocity);
	FLD(avelocity); FLD(mins); FLD(maxs); FLD(size); FLD(absmin); FLD(absmax); FLD(solid);
	FLD(movetype); FLD(nextthink); FLD(think); FLD(touch); FLD(use); FLD(blocked); FLD(frame);
	FLD(skin); FLD(flags); FLD(owner); FLD(chain); FLD(health); FLD(ltime); FLD(effects);
	FLD(colormap); FLD(gravity); FLD(groundentity); FLD(customphysics); FLD(predraw);
	FLD(drawmask); FLD(renderflags); FLD(alpha); FLD(scale); FLD(pvsflags); FLD(dimension_solid);
	FLD(dimension_hit); FLD(hitcontentsmaski); FLD(v_angle); FLD(fixangle); FLD(button0);
	FLD(button2); FLD(impulse); FLD(waterlevel); FLD(watertype); FLD(view_ofs); FLD(netname);
#undef FLD
	f->sendentity = QC_FieldOfs(vm, "SendEntity");
	f->sendflags = QC_FieldOfs(vm, "SendFlags");
	f->nextthink_lerp = -1;
#define GLB(x) g->x = QC_GlobalOfs(vm, #x)
	GLB(self); GLB(other); GLB(world); GLB(time); GLB(frametime); GLB(mapname); GLB(serverflags);
	GLB(force_retouch); GLB(v_forward); GLB(v_up); GLB(v_right); GLB(trace_allsolid);
	GLB(trace_startsolid); GLB(trace_fraction); GLB(trace_endpos); GLB(trace_plane_normal);
	GLB(trace_plane_dist); GLB(trace_ent); GLB(trace_inopen); GLB(trace_inwater);
	GLB(trace_endcontentsi); GLB(trace_surfaceflagsi); GLB(trace_surfacename); GLB(trace_brush_id);
	GLB(trace_brush_faceid); GLB(trace_surface_id); GLB(trace_bone_id); GLB(trace_triangle_id);
	GLB(msg_entity); GLB(parm1); GLB(cltime); GLB(thisprogs); GLB(deathmatch); GLB(coop);
#undef GLB
	for (int i = 0; i < QC_MAX_EXTPARMS; i++) {
		char name[16];
		snprintf(name, sizeof(name), "$parm%d", i + MAX_PARMS);
		vm->extparm_ofs[i] = QC_GlobalOfs(vm, name);
	}
}

static qc_builtin_fn lookup_builtin(const qc_builtin_t *const *tables, const char *name)
{
	for (int t = 0; tables && tables[t]; t++)
		for (const qc_builtin_t *b = tables[t]; b->name; b++)
			if (!strcmp(b->name, name))
				return b->fn;
	return NULL;
}

static void bi_missing(qcvm_t *vm)
{
	qcfunc_t *f = &vm->functions[vm->extra->calling_builtin];
	if (!vm->missing_reported[vm->extra->calling_builtin]) {
		vm->missing_reported[vm->extra->calling_builtin] = 1;
		Con_Printf("^3%s: builtin %s (#%d) is not implemented, returning 0\n", vm->name, f->name, f->builtin_num);
	}
	vm->globals[OFS_RETURN].i = 0;
	vm->globals[OFS_RETURN + 1].i = 0;
	vm->globals[OFS_RETURN + 2].i = 0;
}

/* builtin definitions are bound by the function's name, so the numbering
 * used by any particular progs does not matter to this engine. A definition
 * redeclared with the 'prior' mechanism is named "prior*name". */
static void bind_builtins(qcvm_t *vm, const qc_builtin_t *const *tables)
{
	vm->missing_reported = Mem_Alloc(vm->numfunctions);
	for (int i = 1; i < vm->numfunctions; i++) {
		qcfunc_t *f = &vm->functions[i];
		const char *name;
		if (f->d.first_statement > 0)
			continue;
		name = f->name;
		if (!strncmp(name, "prior*", 6))
			name += 6;
		f->builtin_num = -f->d.first_statement;
		f->builtin = lookup_builtin(tables, name);
		if (!f->builtin)
			f->builtin = bi_missing;
	}
}

static void unpack_defs(qcvm_t *vm, int32_t ofs, int count, bool wide, qcdef_t **out)
{
	qcdef_t *defs = Mem_Alloc(sizeof(qcdef_t) * (count ? count : 1));
	const uint8_t *p = vm->progsdata + ofs;
	for (int i = 0; i < count; i++) {
		if (wide) {
			defs[i].type = (uint32_t)rd32(p);
			defs[i].ofs = (uint32_t)rd32(p + 4);
			defs[i].name = QC_ProgsString(vm, rd32(p + 8));
			p += 12;
		} else {
			defs[i].type = p[0] | (p[1] << 8);
			defs[i].ofs = p[2] | (p[3] << 8);
			defs[i].name = QC_ProgsString(vm, rd32(p + 4));
			p += 8;
		}
	}
	*out = defs;
}

qcvm_t *QC_Load(const char *name, const void *data, size_t size, qcvm_kind_t kind,
                const qc_builtin_t *const *tables, int max_edicts, size_t heapsize)
{
	qcvm_t *vm;
	progs_header_t *h;
	int stmt_bytes, def_bytes;
	size_t globals_bytes, edict_bytes;

	if (size < PROGS_HEADER_V6_SIZE) {
		Con_Printf("%s: too small to be a progs file\n", name);
		return NULL;
	}
	if (vm_count >= (int)BW_ARRAY_COUNT(vm_list)) {
		Con_Printf("%s: too many progs loaded\n", name);
		return NULL;
	}

	vm = Mem_Alloc(sizeof(*vm));
	vm->kind = kind;
	Str_Copy(vm->name, name, sizeof(vm->name));
	vm->progsdata = Mem_Alloc(size);
	memcpy(vm->progsdata, data, size);
	vm->progssize = size;
	h = &vm->hdr;
	for (int i = 0; i < 14; i++)
		((int32_t *)h)[i] = rd32(vm->progsdata + i * 4);
	if (h->version != PROGS_VERSION_QUAKE && h->version != PROGS_VERSION_EXTENDED) {
		Con_Printf("%s: unsupported progs version %d\n", name, h->version);
		QC_Free(vm);
		return NULL;
	}
	h->entityfields = 0;
	if (h->version == PROGS_VERSION_EXTENDED && size >= PROGS_HEADER_V6_SIZE + 4)
		h->entityfields = rd32(vm->progsdata + PROGS_HEADER_V6_SIZE);

	stmt_bytes = h->numstatements ? (section_end(h, h->ofs_statements, size) - h->ofs_statements) / h->numstatements : 8;
	def_bytes = h->numglobaldefs ? (section_end(h, h->ofs_globaldefs, size) - h->ofs_globaldefs) / h->numglobaldefs : 8;
	vm->stmt32 = stmt_bytes >= 16;
	if (!section_ok(size, h->ofs_statements, h->numstatements, vm->stmt32 ? 16 : 8) ||
	    !section_ok(size, h->ofs_globaldefs, h->numglobaldefs, def_bytes >= 12 ? 12 : 8) ||
	    !section_ok(size, h->ofs_fielddefs, h->numfielddefs, def_bytes >= 12 ? 12 : 8) ||
	    !section_ok(size, h->ofs_functions, h->numfunctions, sizeof(dfunction_t)) ||
	    !section_ok(size, h->ofs_strings, h->numstrings, 1) ||
	    !section_ok(size, h->ofs_globals, h->numglobals, 4) || h->numfunctions < 1) {
		Con_Printf("%s: corrupt progs header\n", name);
		QC_Free(vm);
		return NULL;
	}

	/* string table, NUL terminated defensively */
	vm->stringsize = h->numstrings;
	vm->strings = Mem_Alloc((size_t)h->numstrings + 1);
	memcpy(vm->strings, vm->progsdata + h->ofs_strings, (size_t)h->numstrings);

	vm->extra = Mem_Alloc(sizeof(*vm->extra));
	QC_ExtraInit(vm);

	/* statements */
	vm->numstatements = h->numstatements;
	vm->statements = Mem_Alloc(sizeof(dstatement32_t) * (h->numstatements + 1));
	for (int i = 0; i < h->numstatements; i++) {
		const uint8_t *p = vm->progsdata + h->ofs_statements + (size_t)i * (vm->stmt32 ? 16 : 8);
		dstatement32_t *s = &vm->statements[i];
		if (vm->stmt32) {
			s->op = (uint32_t)rd32(p);
			s->a = (uint32_t)rd32(p + 4);
			s->b = (uint32_t)rd32(p + 8);
			s->c = (uint32_t)rd32(p + 12);
		} else {
			s->op = p[0] | (p[1] << 8);
			s->a = p[2] | (p[3] << 8);
			s->b = p[4] | (p[5] << 8);
			s->c = p[6] | (p[7] << 8);
		}
	}

	unpack_defs(vm, h->ofs_globaldefs, h->numglobaldefs, def_bytes >= 12, &vm->globaldefs);
	vm->numglobaldefs = h->numglobaldefs;
	unpack_defs(vm, h->ofs_fielddefs, h->numfielddefs, def_bytes >= 12, &vm->fielddefs);
	vm->numfielddefs = h->numfielddefs;

	/* functions */
	vm->numfunctions = h->numfunctions;
	vm->functions = Mem_Alloc(sizeof(qcfunc_t) * h->numfunctions);
	for (int i = 0; i < h->numfunctions; i++) {
		const uint8_t *p = vm->progsdata + h->ofs_functions + (size_t)i * sizeof(dfunction_t);
		qcfunc_t *f = &vm->functions[i];
		f->d.first_statement = rd32(p);
		f->d.parm_start = rd32(p + 4);
		f->d.locals = rd32(p + 8);
		f->d.profile = rd32(p + 12);
		f->d.s_name = rd32(p + 16);
		f->d.s_file = rd32(p + 20);
		f->d.numparms = rd32(p + 24);
		memcpy(f->d.parm_size, p + 28, 8);
		f->name = QC_ProgsString(vm, f->d.s_name);
		f->file = QC_ProgsString(vm, f->d.s_file);
		if (f->d.first_statement >= h->numstatements || f->d.locals < 0 || f->d.parm_start < 0) {
			Con_Printf("%s: function %s has a bad header\n", name, f->name);
			QC_Free(vm);
			return NULL;
		}
	}

	build_hash(&vm->extra->globalhash, vm->numglobaldefs, gdef_name, vm);
	build_hash(&vm->extra->fieldhash, vm->numfielddefs, fdef_name, vm);
	build_hash(&vm->extra->funchash, vm->numfunctions, func_name, vm);

	/* Globals are given room for every index a statement can encode, so
	 * operand decoding never needs a range check: 16-bit statements can only
	 * name the first 65536 words. 32-bit ones are validated below. */
	vm->numglobals = h->numglobals;
	vm->globalslots = vm->stmt32 ? h->numglobals + 4 : BW_MAX(h->numglobals + 4, 65536 + 4);
	for (int i = 0; i < vm->numfunctions; i++) {
		int end = vm->functions[i].d.parm_start + vm->functions[i].d.locals;
		if (end > vm->globalslots) {
			Con_Printf("%s: function %s locals exceed the globals\n", name, vm->functions[i].name);
			QC_Free(vm);
			return NULL;
		}
	}
	if (vm->stmt32) {
		/* TODO: 32-bit statements can name any global, so operands need a
		 * per-opcode range check before this format can be run safely. */
		Con_Printf("%s: 32-bit statement progs are not supported yet\n", name);
		QC_Free(vm);
		return NULL;
	}

	/* entity layout: the progs' declared size, grown to cover every field def */
	vm->edict_words = h->entityfields;
	for (int i = 0; i < vm->numfielddefs; i++) {
		int t = vm->fielddefs[i].type & ~DEF_SAVEGLOBAL;
		int end = (int)vm->fielddefs[i].ofs + (t == ev_vector ? 3 : (t == ev_int64 || t == ev_uint64 || t == ev_double) ? 2 : 1);
		if (end > vm->edict_words)
			vm->edict_words = end;
	}
	if (vm->edict_words < 1)
		vm->edict_words = 1;

	vm->max_edicts = max_edicts > 0 ? max_edicts : 1;
	globals_bytes = ((size_t)vm->globalslots * 4 + 63) & ~(size_t)63;
	edict_bytes = (size_t)vm->edict_words * 4 * vm->max_edicts;
	vm->edicts_ofs = globals_bytes;
	vm->heap_ofs = (globals_bytes + edict_bytes + 63) & ~(size_t)63;
	vm->heap_size = heapsize;
	vm->memsize = vm->heap_ofs + heapsize;
	if (vm->memsize > 0x7fffffff) {
		Con_Printf("%s: address space too large\n", name);
		QC_Free(vm);
		return NULL;
	}
	vm->mem = Mem_Alloc(vm->memsize);
	vm->globals = (qcval_t *)vm->mem;
	for (int i = 0; i < h->numglobals; i++)
		vm->globals[i].i = rd32(vm->progsdata + h->ofs_globals + (size_t)i * 4);
	vm->edicts = Mem_Alloc(sizeof(qcedict_t) * vm->max_edicts);
	for (int i = 0; i < vm->max_edicts; i++)
		vm->edicts[i].free = true;
	vm->edicts[0].free = false; /* world */
	vm->num_edicts = 1;
	QC_HeapInit(vm);

	vm->localstack_size = 1 << 16;
	vm->localstack = Mem_Alloc(sizeof(qcval_t) * vm->localstack_size);
	vm->rng = 0x9e3779b97f4a7c15ull ^ (uint64_t)size;

	resolve_offsets(vm);
	bind_builtins(vm, tables);

	vm_list[vm_count++] = vm;
	Con_DPrintf("%s: %d functions, %d statements, %d globals, %d entity words\n", name,
	            vm->numfunctions, vm->numstatements, vm->numglobals, vm->edict_words);
	return vm;
}

void QC_Free(qcvm_t *vm)
{
	if (!vm)
		return;
	for (int i = 0; i < vm_count; i++)
		if (vm_list[i] == vm) {
			vm_list[i] = vm_list[--vm_count];
			break;
		}
	if (vm->extra)
		QC_ExtraFree(vm);
	Mem_Free(vm->extra);
	Mem_Free(vm->progsdata);
	Mem_Free(vm->statements);
	Mem_Free(vm->functions);
	Mem_Free(vm->globaldefs);
	Mem_Free(vm->fielddefs);
	Mem_Free(vm->strings);
	Mem_Free(vm->mem);
	Mem_Free(vm->edicts);
	Mem_Free(vm->localstack);
	Mem_Free(vm->missing_reported);
	Mem_Free(vm);
}

/* --- parameters ------------------------------------------------------ */

qcval_t *QC_Parm(qcvm_t *vm, int n)
{
	static qcval_t zero[3];
	if (n < MAX_PARMS)
		return &vm->globals[OFS_PARM(n)];
	n -= MAX_PARMS;
	if (n < QC_MAX_EXTPARMS && vm->extparm_ofs[n] >= 0)
		return &vm->globals[vm->extparm_ofs[n]];
	memset(zero, 0, sizeof(zero));
	return zero;
}

/* --- entities -------------------------------------------------------- */

bool QC_ValidEdict(qcvm_t *vm, int e) { return e >= 0 && e < vm->max_edicts; }

qcval_t *QC_EdictFields(qcvm_t *vm, int e)
{
	if (e < 0 || e >= vm->max_edicts)
		QC_RunError(vm, "entity %d out of range", e);
	return (qcval_t *)(vm->mem + vm->edicts_ofs + (size_t)e * vm->edict_words * 4);
}

void QC_ClearEdict(qcvm_t *vm, int e)
{
	memset(QC_EdictFields(vm, e), 0, (size_t)vm->edict_words * 4);
	vm->edicts[e].free = false;
}

int QC_SpawnEdict(qcvm_t *vm)
{
	float now = QC_GFloat(vm, vm->gofs.time);
	int e;
	/* reuse a slot only after it has been free for a while, so stale
	 * references can still detect the removal with wasfreed() */
	for (e = 1; e < vm->num_edicts; e++) {
		qcedict_t *ed = &vm->edicts[e];
		if (ed->free && (ed->freetime < 2 || now - ed->freetime > 0.5f)) {
			QC_ClearEdict(vm, e);
			return e;
		}
	}
	if (e >= vm->max_edicts)
		QC_RunError(vm, "no free entities (max_edicts %d)", vm->max_edicts);
	vm->num_edicts = e + 1;
	QC_ClearEdict(vm, e);
	return e;
}

void QC_FreeEdict(qcvm_t *vm, int e, float now)
{
	qcedict_t *ed;
	if (e <= 0 || e >= vm->max_edicts)
		return;
	ed = &vm->edicts[e];
	if (ed->free)
		return;
	ed->free = true;
	ed->freetime = now;
	/* clear the few fields that mark an entity as live, as documented for remove() */
	QC_SetEInt(vm, e, vm->fofs.model, 0);
	QC_SetEFloat(vm, e, vm->fofs.modelindex, 0);
	QC_SetEFloat(vm, e, vm->fofs.solid, 0);
	QC_SetEInt(vm, e, vm->fofs.classname, 0);
	QC_SetEFloat(vm, e, vm->fofs.nextthink, -1);
	QC_SetEInt(vm, e, vm->fofs.think, 0);
	QC_SetEInt(vm, e, vm->fofs.touch, 0);
}

bool QC_SetFieldFromString(qcvm_t *vm, int e, qcdef_t *field, const char *value)
{
	qcval_t *v = QC_EdictFields(vm, e) + field->ofs;
	switch (field->type & ~DEF_SAVEGLOBAL) {
	case ev_string:
		v->i = QC_NewString(vm, value);
		break;
	case ev_float:
		v->f = (float)atof(value);
		break;
	case ev_integer:
	case ev_uint:
		v->i = (int32_t)strtol(value, NULL, 0);
		break;
	case ev_vector: {
		float x = 0, y = 0, z = 0;
		const char *p = value;
		if (*p == '\'')
			p++;
		sscanf(p, "%f %f %f", &x, &y, &z);
		v[0].f = x;
		v[1].f = y;
		v[2].f = z;
		break;
	}
	case ev_entity:
		v->i = atoi(value);
		break;
	case ev_field: {
		qcdef_t *d = QC_FindField(vm, value);
		if (!d)
			return false;
		v->i = (int32_t)d->ofs;
		break;
	}
	case ev_function: {
		int f = QC_FindFunction(vm, value);
		if (!f && *value)
			return false;
		v->i = f;
		break;
	}
	default:
		return false;
	}
	return true;
}

void QC_FieldToString(qcvm_t *vm, int e, qcdef_t *field, char *out, size_t outsize)
{
	qcval_t *v = QC_EdictFields(vm, e) + field->ofs;
	switch (field->type & ~DEF_SAVEGLOBAL) {
	case ev_string:
		Str_Copy(out, QC_GetString(vm, v->i), outsize);
		break;
	case ev_float:
		snprintf(out, outsize, "%g", v->f);
		break;
	case ev_integer:
		snprintf(out, outsize, "%d", v->i);
		break;
	case ev_uint:
		snprintf(out, outsize, "%u", v->u);
		break;
	case ev_vector:
		snprintf(out, outsize, "%g %g %g", v[0].f, v[1].f, v[2].f);
		break;
	case ev_entity:
		snprintf(out, outsize, "%d", v->i);
		break;
	case ev_function:
		Str_Copy(out, v->i ? QC_FunctionName(vm, v->i) : "", outsize);
		break;
	case ev_field: {
		const char *n = "";
		for (int i = 0; i < vm->numfielddefs; i++)
			if ((int)vm->fielddefs[i].ofs == v->i) {
				n = vm->fielddefs[i].name;
				break;
			}
		Str_Copy(out, n, outsize);
		break;
	}
	default:
		snprintf(out, outsize, "%d", v->i);
		break;
	}
}

const char *QC_ParseEdict(qcvm_t *vm, int e, const char *data, bool *ok)
{
	char key[256], value[4096];
	*ok = true;
	data = COM_Parse(data, key, sizeof(key));
	if (!data || strcmp(key, "{")) {
		*ok = false;
		return data;
	}
	for (;;) {
		qcdef_t *d;
		data = COM_Parse(data, key, sizeof(key));
		if (!data) {
			*ok = false;
			return NULL;
		}
		if (!strcmp(key, "}"))
			break;
		data = COM_Parse(data, value, sizeof(value));
		if (!data) {
			*ok = false;
			return NULL;
		}
		/* "angle" is shorthand for a yaw-only angles vector */
		if (!strcmp(key, "angle") && !QC_FindField(vm, "angle")) {
			char tmp[64];
			snprintf(tmp, sizeof(tmp), "0 %.32s 0", value);
			Str_Copy(key, "angles", sizeof(key));
			Str_Copy(value, tmp, sizeof(value));
		}
		if (key[0] == '_')
			continue; /* editor-only keys */
		d = QC_FindField(vm, key);
		if (!d)
			continue; /* unknown keys are normal; the game may read them with getentitytoken */
		QC_SetFieldFromString(vm, e, d, value);
	}
	return data;
}

/* --- arena pointers and heap ----------------------------------------- */

void *QC_Pointer(qcvm_t *vm, uint32_t addr, size_t bytes)
{
	if ((uint64_t)addr + bytes > vm->memsize || addr == 0)
		QC_RunError(vm, "bad pointer 0x%x (+%zu)", addr, bytes);
	return vm->mem + addr;
}

/* A simple first-fit allocator inside the arena. Each block starts with a
 * 16-byte header {size, used, prev_size, magic}; blocks are kept in address
 * order so neighbours can be merged on free. */
typedef struct {
	uint32_t size; /* bytes including header */
	uint32_t used;
	uint32_t prev;
	uint32_t magic;
} heaphdr_t;
#define HEAP_MAGIC 0x4b4c5742u

static heaphdr_t *heap_block(qcvm_t *vm, size_t ofs) { return (heaphdr_t *)(vm->mem + vm->heap_ofs + ofs); }

void QC_HeapInit(qcvm_t *vm)
{
	heaphdr_t *h;
	if (vm->heap_size < 64)
		return;
	h = heap_block(vm, 0);
	h->size = (uint32_t)(vm->heap_size & ~(size_t)15);
	h->used = 0;
	h->prev = 0;
	h->magic = HEAP_MAGIC;
}

uint32_t QC_HeapAlloc(qcvm_t *vm, size_t bytes)
{
	size_t need = ((bytes + 15) & ~(size_t)15) + sizeof(heaphdr_t);
	size_t ofs = 0;
	if (vm->heap_size < 64 || bytes > vm->heap_size)
		return 0;
	while (ofs < vm->heap_size) {
		heaphdr_t *h = heap_block(vm, ofs);
		if (h->magic != HEAP_MAGIC || !h->size)
			QC_RunError(vm, "memalloc heap corrupted");
		if (!h->used && h->size >= need) {
			if (h->size - need >= sizeof(heaphdr_t) + 16) {
				heaphdr_t *rest = heap_block(vm, ofs + need);
				rest->size = h->size - (uint32_t)need;
				rest->used = 0;
				rest->prev = (uint32_t)need;
				rest->magic = HEAP_MAGIC;
				if (ofs + need + rest->size < vm->heap_size)
					heap_block(vm, ofs + need + rest->size)->prev = rest->size;
				h->size = (uint32_t)need;
			}
			h->used = 1;
			memset(h + 1, 0, h->size - sizeof(heaphdr_t));
			return (uint32_t)(vm->heap_ofs + ofs + sizeof(heaphdr_t));
		}
		ofs += h->size;
	}
	return 0;
}

static heaphdr_t *heap_header_for(qcvm_t *vm, uint32_t addr)
{
	heaphdr_t *h;
	if (addr < vm->heap_ofs + sizeof(heaphdr_t) || addr >= vm->heap_ofs + vm->heap_size)
		return NULL;
	h = (heaphdr_t *)(vm->mem + addr) - 1;
	if (h->magic != HEAP_MAGIC || !h->used)
		return NULL;
	return h;
}

void QC_HeapFree(qcvm_t *vm, uint32_t addr)
{
	heaphdr_t *h;
	size_t ofs;
	if (!addr)
		return;
	h = heap_header_for(vm, addr);
	if (!h)
		QC_RunError(vm, "memfree of invalid pointer 0x%x", addr);
	h->used = 0;
	ofs = (uint8_t *)h - (vm->mem + vm->heap_ofs);
	/* merge with the next block */
	if (ofs + h->size < vm->heap_size) {
		heaphdr_t *n = heap_block(vm, ofs + h->size);
		if (!n->used) {
			h->size += n->size;
			n->magic = 0;
		}
	}
	/* and with the previous one */
	if (ofs > 0) {
		heaphdr_t *p = heap_block(vm, ofs - h->prev);
		if (!p->used) {
			p->size += h->size;
			h->magic = 0;
			h = p;
			ofs = (uint8_t *)p - (vm->mem + vm->heap_ofs);
		}
	}
	if (ofs + h->size < vm->heap_size)
		heap_block(vm, ofs + h->size)->prev = h->size;
}

uint32_t QC_HeapRealloc(qcvm_t *vm, uint32_t addr, size_t bytes)
{
	heaphdr_t *h;
	uint32_t n;
	size_t old;
	if (!addr)
		return QC_HeapAlloc(vm, bytes);
	h = heap_header_for(vm, addr);
	if (!h)
		QC_RunError(vm, "memrealloc of invalid pointer 0x%x", addr);
	old = h->size - sizeof(heaphdr_t);
	if (old >= bytes)
		return addr;
	n = QC_HeapAlloc(vm, bytes);
	if (!n)
		return 0;
	memcpy(vm->mem + n, vm->mem + addr, old);
	QC_HeapFree(vm, addr);
	return n;
}

bool QC_HeapBlockUsed(qcvm_t *vm, size_t ofs, size_t *size)
{
	heaphdr_t *h = heap_block(vm, ofs);
	*size = h->size;
	return h->used;
}

float QC_Random(qcvm_t *vm)
{
	/* xorshift64*, 24 bits of mantissa so the result is always < 1 */
	uint64_t x = vm->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	vm->rng = x;
	return (float)((x * 0x2545F4914F6CDD1Dull) >> 40) / 16777216.0f;
}

bool QC_BuiltinImplemented(qcvm_t *vm, int fnum)
{
	if (fnum <= 0 || fnum >= vm->numfunctions)
		return false;
	return vm->functions[fnum].builtin != bi_missing;
}
