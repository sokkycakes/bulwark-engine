/* Bulwark engine - QuakeC virtual machine: loading, entities, memory.
 * The interpreter loop lives in qc_exec.c. ISC licence, see LICENSE. */
#include "qcvm.h"
#include "qc_internal.h"
#include "../common/cvar.h"

#include <stdio.h>
#include <stdlib.h>

static qcvm_t *vm_list[16];
static int vm_count;

int QC_VMCount(void) { return vm_count; }
qcvm_t *QC_VMAt(int i) { return vm_list[i]; }

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static const char *progs_string(const qcprogs_t *p, int32_t ofs)
{
	if (ofs <= 0 || ofs >= p->stringsize)
		return "";
	return p->strings + ofs;
}

/* a static string value: an offset into its progs' table, tagged with the progs number */
const char *QC_ProgsString(qcvm_t *vm, int32_t ofs)
{
	int pn = QC_FUNC_PROGS(ofs);
	if (pn >= vm->numprogs)
		return "";
	return progs_string(&vm->progs[pn], QC_FUNC_INDEX(ofs));
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

static const char *gdef_name(void *ctx, int i) { return ((qcprogs_t *)ctx)->globaldefs[i].name; }
static const char *fdef_name(void *ctx, int i) { return ((qcprogs_t *)ctx)->fielddefs[i].name; }
static const char *func_name(void *ctx, int i) { return ((qcprogs_t *)ctx)->functions[i].name; }

int QC_FindFunctionIn(qcvm_t *vm, int pn, const char *name)
{
	qcprogs_t *p;
	int i;
	if (pn < 0 || pn >= vm->numprogs)
		return 0;
	p = &vm->progs[pn];
	i = find_hash(&p->funchash, name, func_name, p);
	return i > 0 ? (i | (pn << QC_PROGS_SHIFT)) : 0;
}

qcdef_t *QC_FindGlobalIn(qcvm_t *vm, int pn, const char *name)
{
	qcprogs_t *p;
	int i;
	if (pn < 0 || pn >= vm->numprogs)
		return NULL;
	p = &vm->progs[pn];
	i = find_hash(&p->globalhash, name, gdef_name, p);
	return i < 0 ? NULL : &p->globaldefs[i];
}

qcval_t *QC_ProgsGlobals(qcvm_t *vm, int pn) { return pn == vm->curprogs ? vm->globals : vm->progs[pn].globals; }

int QC_FindFunction(qcvm_t *vm, const char *name) { return QC_FindFunctionIn(vm, vm->curprogs, name); }

qcdef_t *QC_FindGlobal(qcvm_t *vm, const char *name) { return QC_FindGlobalIn(vm, vm->curprogs, name); }

qcdef_t *QC_FindField(qcvm_t *vm, const char *name)
{
	qcprogs_t *p = &vm->progs[vm->curprogs];
	int i = find_hash(&p->fieldhash, name, fdef_name, p);
	return i < 0 ? NULL : &p->fielddefs[i];
}

qcfunc_t *QC_Function(qcvm_t *vm, func_t f)
{
	int pn = QC_FUNC_PROGS(f), i = QC_FUNC_INDEX(f);
	if (pn >= vm->numprogs || i <= 0 || i >= vm->progs[pn].numfunctions)
		return NULL;
	return &vm->progs[pn].functions[i];
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
	qcfunc_t *fn = QC_Function(vm, f);
	return fn ? fn->name : "(null)";
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

static void resolve_fields(qcvm_t *vm)
{
	qcfieldofs_t *f = &vm->fofs;
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
}

/* engine-known globals of the running progs */
static void resolve_globals(qcvm_t *vm)
{
	qcglobalofs_t *g = &vm->gofs;
	qcdef_t *end = QC_FindGlobal(vm, "end_sys_globals");
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
	/* without the marker only the return value and parameters are shared */
	vm->sysglobals = end ? (int)end->ofs : RESERVED_OFS;
	if (vm->sysglobals > vm->numglobals)
		vm->sysglobals = vm->numglobals;
}

static qc_builtin_fn lookup_builtin(const qc_builtin_t *const *tables, const char *name)
{
	for (int t = 0; tables && tables[t]; t++)
		for (const qc_builtin_t *b = tables[t]; b->name; b++)
			if (!strcmp(b->name, name))
				return b->fn;
	for (const qc_builtin_t *b = qc_builtins_skel; b->name; b++)
		if (!strcmp(b->name, name))
			return b->fn;
	for (const qc_builtin_t *b = qc_builtins_modelevents; b->name; b++)
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


static void unpack_defs(qcprogs_t *p, int32_t ofs, int count, bool wide, qcdef_t **out)
{
	qcdef_t *defs = Mem_Alloc(sizeof(qcdef_t) * (count ? count : 1));
	const uint8_t *d = p->progsdata + ofs;
	for (int i = 0; i < count; i++) {
		if (wide) {
			defs[i].type = (uint32_t)rd32(d);
			defs[i].ofs = (uint32_t)rd32(d + 4);
			defs[i].name = progs_string(p, rd32(d + 8));
			d += 12;
		} else {
			defs[i].type = d[0] | (d[1] << 8);
			defs[i].ofs = d[2] | (d[3] << 8);
			defs[i].name = progs_string(p, rd32(d + 4));
			d += 8;
		}
	}
	*out = defs;
}

static void free_progs(qcprogs_t *p)
{
	Mem_Free(p->progsdata);
	Mem_Free(p->statements);
	Mem_Free(p->functions);
	Mem_Free(p->globaldefs);
	Mem_Free(p->fielddefs);
	Mem_Free(p->strings);
	Mem_Free(p->missing_reported);
	Mem_Free(p->globalhash.slots);
	Mem_Free(p->fieldhash.slots);
	Mem_Free(p->funchash.slots);
	Mem_Free(p->autocvars);
	memset(p, 0, sizeof(*p));
}

/* reads a progs file image into p: code, defs, strings, functions and the
 * size of its globals; the globals themselves are placed by the caller */
static bool parse_progs(qcprogs_t *p, const char *name, const void *data, size_t size)
{
	progs_header_t *h = &p->hdr;
	int stmt_bytes, def_bytes;

	if (size < PROGS_HEADER_V6_SIZE) {
		Con_Printf("%s: too small to be a progs file\n", name);
		return false;
	}
	Str_Copy(p->name, name, sizeof(p->name));
	p->progsdata = Mem_Alloc(size);
	memcpy(p->progsdata, data, size);
	p->progssize = size;
	for (int i = 0; i < 14; i++)
		((int32_t *)h)[i] = rd32(p->progsdata + i * 4);
	if (h->version != PROGS_VERSION_QUAKE && h->version != PROGS_VERSION_EXTENDED) {
		Con_Printf("%s: unsupported progs version %d\n", name, h->version);
		return false;
	}
	h->entityfields = 0;
	if (h->version == PROGS_VERSION_EXTENDED && size >= PROGS_HEADER_V6_SIZE + 4)
		h->entityfields = rd32(p->progsdata + PROGS_HEADER_V6_SIZE);

	stmt_bytes = h->numstatements ? (section_end(h, h->ofs_statements, size) - h->ofs_statements) / h->numstatements : 8;
	def_bytes = h->numglobaldefs ? (section_end(h, h->ofs_globaldefs, size) - h->ofs_globaldefs) / h->numglobaldefs : 8;
	p->stmt32 = stmt_bytes >= 16;
	if (!section_ok(size, h->ofs_statements, h->numstatements, p->stmt32 ? 16 : 8) ||
	    !section_ok(size, h->ofs_globaldefs, h->numglobaldefs, def_bytes >= 12 ? 12 : 8) ||
	    !section_ok(size, h->ofs_fielddefs, h->numfielddefs, def_bytes >= 12 ? 12 : 8) ||
	    !section_ok(size, h->ofs_functions, h->numfunctions, sizeof(dfunction_t)) ||
	    !section_ok(size, h->ofs_strings, h->numstrings, 1) ||
	    !section_ok(size, h->ofs_globals, h->numglobals, 4) || h->numfunctions < 1) {
		Con_Printf("%s: corrupt progs header\n", name);
		return false;
	}
	if (h->numstrings > QC_INDEX_MASK || h->numfunctions > QC_INDEX_MASK) {
		Con_Printf("%s: string table or function list too large\n", name);
		return false;
	}

	/* string table, NUL terminated defensively */
	p->stringsize = h->numstrings;
	p->strings = Mem_Alloc((size_t)h->numstrings + 1);
	memcpy(p->strings, p->progsdata + h->ofs_strings, (size_t)h->numstrings);

	/* statements */
	p->numstatements = h->numstatements;
	p->statements = Mem_Alloc(sizeof(dstatement32_t) * (h->numstatements + 1));
	for (int i = 0; i < h->numstatements; i++) {
		const uint8_t *d = p->progsdata + h->ofs_statements + (size_t)i * (p->stmt32 ? 16 : 8);
		dstatement32_t *st = &p->statements[i];
		if (p->stmt32) {
			st->op = (uint32_t)rd32(d);
			st->a = (uint32_t)rd32(d + 4);
			st->b = (uint32_t)rd32(d + 8);
			st->c = (uint32_t)rd32(d + 12);
		} else {
			st->op = d[0] | (d[1] << 8);
			st->a = d[2] | (d[3] << 8);
			st->b = d[4] | (d[5] << 8);
			st->c = d[6] | (d[7] << 8);
		}
	}

	unpack_defs(p, h->ofs_globaldefs, h->numglobaldefs, def_bytes >= 12, &p->globaldefs);
	p->numglobaldefs = h->numglobaldefs;
	unpack_defs(p, h->ofs_fielddefs, h->numfielddefs, def_bytes >= 12, &p->fielddefs);
	p->numfielddefs = h->numfielddefs;

	/* functions */
	p->numfunctions = h->numfunctions;
	p->functions = Mem_Alloc(sizeof(qcfunc_t) * h->numfunctions);
	for (int i = 0; i < h->numfunctions; i++) {
		const uint8_t *d = p->progsdata + h->ofs_functions + (size_t)i * sizeof(dfunction_t);
		qcfunc_t *f = &p->functions[i];
		f->d.first_statement = rd32(d);
		f->d.parm_start = rd32(d + 4);
		f->d.locals = rd32(d + 8);
		f->d.profile = rd32(d + 12);
		f->d.s_name = rd32(d + 16);
		f->d.s_file = rd32(d + 20);
		f->d.numparms = rd32(d + 24);
		memcpy(f->d.parm_size, d + 28, 8);
		f->name = progs_string(p, f->d.s_name);
		f->file = progs_string(p, f->d.s_file);
		if (f->d.first_statement >= h->numstatements || f->d.locals < 0 || f->d.parm_start < 0) {
			Con_Printf("%s: function %s has a bad header\n", name, f->name);
			return false;
		}
	}

	build_hash(&p->globalhash, p->numglobaldefs, gdef_name, p);
	build_hash(&p->fieldhash, p->numfielddefs, fdef_name, p);
	build_hash(&p->funchash, p->numfunctions, func_name, p);

	/* Globals are given room for every index a statement can encode, so
	 * operand decoding never needs a range check: 16-bit statements can only
	 * name the first 65536 words. 32-bit ones are validated below. */
	p->numglobals = h->numglobals;
	p->globalslots = p->stmt32 ? h->numglobals + 4 : BW_MAX(h->numglobals + 4, 65536 + 4);
	for (int i = 0; i < p->numfunctions; i++) {
		int end = p->functions[i].d.parm_start + p->functions[i].d.locals;
		if (end > p->globalslots) {
			Con_Printf("%s: function %s locals exceed the globals\n", name, p->functions[i].name);
			return false;
		}
	}
	if (p->stmt32) {
		/* TODO: 32-bit statements can name any global, so operands need a
		 * per-opcode range check before this format can be run safely. */
		Con_Printf("%s: 32-bit statement progs are not supported yet\n", name);
		return false;
	}
	return true;
}

/* makes progs n the running one, without any sharing of globals */
static void activate(qcvm_t *vm, int n)
{
	qcprogs_t *p = &vm->progs[n];
	vm->progsdata = p->progsdata;
	vm->progssize = p->progssize;
	vm->hdr = p->hdr;
	vm->stmt32 = p->stmt32;
	vm->statements = p->statements;
	vm->numstatements = p->numstatements;
	vm->functions = p->functions;
	vm->numfunctions = p->numfunctions;
	vm->globaldefs = p->globaldefs;
	vm->numglobaldefs = p->numglobaldefs;
	vm->fielddefs = p->fielddefs;
	vm->numfielddefs = p->numfielddefs;
	vm->strings = p->strings;
	vm->stringsize = p->stringsize;
	vm->globals = p->globals;
	vm->globals_addr = p->globals_addr;
	vm->numglobals = p->numglobals;
	vm->globalslots = p->globalslots;
	vm->sysglobals = p->sysglobals;
	vm->gofs = p->gofs;
	memcpy(vm->extparm_ofs, p->extparm_ofs, sizeof(vm->extparm_ofs));
	vm->missing_reported = p->missing_reported;
	vm->curprogs = n;
}

/* the engine-derived parts of the running progs, kept for later switches */
static void store_derived(qcvm_t *vm, int n)
{
	qcprogs_t *p = &vm->progs[n];
	p->sysglobals = vm->sysglobals;
	p->gofs = vm->gofs;
	memcpy(p->extparm_ofs, vm->extparm_ofs, sizeof(p->extparm_ofs));
	p->missing_reported = vm->missing_reported;
}

void QC_SwitchProgs(qcvm_t *vm, int n)
{
	qcval_t *from;
	int shared, oldext[QC_MAX_EXTPARMS];
	if (n == vm->curprogs || n < 0 || n >= vm->numprogs)
		return;
	from = vm->globals;
	shared = vm->sysglobals;
	memcpy(oldext, vm->extparm_ofs, sizeof(oldext));
	activate(vm, n);
	/* the system globals (self, other, time, parameters, return value...)
	 * behave as one set: hand them over to the progs taking control */
	if (shared > vm->sysglobals)
		shared = vm->sysglobals;
	memcpy(vm->globals, from, sizeof(qcval_t) * shared);
	for (int i = 0; i < QC_MAX_EXTPARMS && i + MAX_PARMS < vm->argc; i++)
		if (oldext[i] >= 0 && vm->extparm_ofs[i] >= 0)
			memcpy(vm->globals + vm->extparm_ofs[i], from + oldext[i], sizeof(qcval_t) * 3);
}

int QC_ResolveProgs(qcvm_t *vm, float handle, const char *name, bool function)
{
	int h = (int)handle;
	if (h == -1)
		return vm->curprogs;
	if (h == -2) {
		for (int i = 0; i < vm->numprogs; i++)
			if (function ? QC_FindFunctionIn(vm, i, name) != 0 : QC_FindGlobalIn(vm, i, name) != NULL)
				return i;
		return -1;
	}
	return h >= 0 && h < vm->numprogs ? h : -1;
}

/* tags the function and static string values in progs n's initial globals
 * with its number, and maps its field offsets onto the main progs' layout */
static bool link_progs(qcvm_t *vm, int n)
{
	qcprogs_t *p = &vm->progs[n], *main = &vm->progs[0];
	int words = BW_MAX(p->hdr.entityfields, 1);
	int *map;
	for (int i = 0; i < p->numfielddefs; i++) {
		int t = p->fielddefs[i].type & ~DEF_SAVEGLOBAL;
		int end = (int)p->fielddefs[i].ofs + (t == ev_vector ? 3 : (t == ev_int64 || t == ev_uint64 || t == ev_double) ? 2 : 1);
		if (end > words)
			words = end;
	}
	map = Mem_Alloc(sizeof(int) * words);
	for (int i = 0; i < words; i++)
		map[i] = -1;
	/* by name first; vectors before their _x/_y/_z components */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < p->numfielddefs; i++) {
			qcdef_t *d = &p->fielddefs[i];
			int t = d->type & ~DEF_SAVEGLOBAL;
			int len = t == ev_vector ? 3 : (t == ev_int64 || t == ev_uint64 || t == ev_double) ? 2 : 1;
			int k = find_hash(&main->fieldhash, d->name, fdef_name, main);
			if ((pass == 0) != (t == ev_vector) || !d->name[0])
				continue;
			if (k >= 0) {
				for (int j = 0; j < len && d->ofs + j < (uint32_t)words; j++)
					if (map[d->ofs + j] < 0)
						map[d->ofs + j] = (int)main->fielddefs[k].ofs + j;
			}
		}
	/* anything left gets room from the spare words */
	for (int i = 0; i < words; i++)
		if (map[i] < 0) {
			if (vm->edict_words_used >= vm->edict_words) {
				Con_Printf("%s: no room left for its own entity fields\n", p->name);
				Mem_Free(map);
				return false;
			}
			map[i] = vm->edict_words_used++;
		}
	for (int i = 0; i < p->numfielddefs; i++)
		if (p->fielddefs[i].ofs < (uint32_t)words)
			p->fielddefs[i].ofs = (uint32_t)map[p->fielddefs[i].ofs];
	for (int i = 0; i < p->numglobaldefs; i++) {
		qcdef_t *d = &p->globaldefs[i];
		int t = d->type & ~DEF_SAVEGLOBAL;
		qcval_t *v = p->globals + d->ofs;
		if (d->ofs >= (uint32_t)p->numglobals)
			continue;
		if (t == ev_field && v->i >= 0 && v->i < words)
			v->i = map[v->i];
		else if (t == ev_function && v->i > 0 && v->i < p->numfunctions)
			v->i |= n << QC_PROGS_SHIFT;
		else if (t == ev_string && v->i > 0 && v->i < p->stringsize)
			v->i |= n << QC_PROGS_SHIFT;
	}
	Mem_Free(map);
	return true;
}

/* Globals named autocvar_<name> mirror cvar <name>. A missing cvar is
 * created with the global's initial value as its default; an existing one
 * (from the command line or a config) keeps its value and overrides the
 * global. Runs with progs n active. */
static void bind_autocvars(qcvm_t *vm, int n)
{
	qcprogs_t *p = &vm->progs[n];
	char def[128];

	for (int i = 0; i < p->numglobaldefs; i++) {
		const qcdef_t *d = &p->globaldefs[i];
		int t = d->type & ~DEF_SAVEGLOBAL;
		const qcval_t *g = p->globals + d->ofs;
		if (!d->name || strncmp(d->name, "autocvar_", 9) || !d->name[9])
			continue;
		switch (t) {
		case ev_float: snprintf(def, sizeof(def), "%g", g->f); break;
		case ev_integer: case ev_uint: case ev_bool: snprintf(def, sizeof(def), "%d", g->i); break;
		case ev_vector: snprintf(def, sizeof(def), "%g %g %g", g[0].f, g[1].f, g[2].f); break;
		case ev_string: Str_Copy(def, QC_GetString(vm, g->i), sizeof(def)); break;
		default: continue;
		}
		p->autocvars = Mem_Realloc(p->autocvars, sizeof(qcautocvar_t) * (p->numautocvars + 1));
		p->autocvars[p->numautocvars].var = Cvar_Get(d->name + 9, def, CVAR_USERCREATED, NULL);
		p->autocvars[p->numautocvars].ofs = (int)d->ofs;
		p->autocvars[p->numautocvars].type = t;
		p->numautocvars++;
	}
	vm->autocvar_gen = Cvar_Generation() - 1; /* force a copy on the next call */
}

void QC_SyncAutocvars(qcvm_t *vm)
{
	if (vm->autocvar_gen == Cvar_Generation())
		return;
	vm->autocvar_gen = Cvar_Generation();
	for (int n = 0; n < vm->numprogs; n++) {
		qcprogs_t *p = &vm->progs[n];
		for (int i = 0; i < p->numautocvars; i++) {
			const qcautocvar_t *a = &p->autocvars[i];
			qcval_t *g = p->globals + a->ofs;
			switch (a->type) {
			case ev_float: g->f = a->var->value; break;
			case ev_integer: case ev_uint: case ev_bool: g->i = a->var->ivalue; break;
			case ev_vector: {
				float v[3] = {0, 0, 0};
				sscanf(a->var->string, "%f %f %f", &v[0], &v[1], &v[2]);
				g[0].f = v[0]; g[1].f = v[1]; g[2].f = v[2];
				break;
			}
			case ev_string:
				if (strcmp(QC_GetString(vm, g->i), a->var->string))
					g->i = QC_NewString(vm, a->var->string);
				break;
			}
		}
	}
}

qcvm_t *QC_Load(const char *name, const void *data, size_t size, qcvm_kind_t kind,
                const qc_builtin_t *const *tables, int max_edicts, size_t heapsize)
{
	qcvm_t *vm;
	qcprogs_t *p;
	size_t globals_bytes, edict_bytes;

	if (vm_count >= (int)BW_ARRAY_COUNT(vm_list)) {
		Con_Printf("%s: too many progs loaded\n", name);
		return NULL;
	}

	vm = Mem_Alloc(sizeof(*vm));
	vm->kind = kind;
	vm->tables = tables;
	Str_Copy(vm->name, name, sizeof(vm->name));
	vm->extra = Mem_Alloc(sizeof(*vm->extra));
	QC_ExtraInit(vm);
	p = &vm->progs[0];
	vm->numprogs = 1;
	if (!parse_progs(p, name, data, size)) {
		QC_Free(vm);
		return NULL;
	}

	/* entity layout: the progs' declared size, grown to cover every field
	 * def, plus spare words for fields that added progs bring */
	vm->edict_words = p->hdr.entityfields;
	for (int i = 0; i < p->numfielddefs; i++) {
		int t = p->fielddefs[i].type & ~DEF_SAVEGLOBAL;
		int end = (int)p->fielddefs[i].ofs + (t == ev_vector ? 3 : (t == ev_int64 || t == ev_uint64 || t == ev_double) ? 2 : 1);
		if (end > vm->edict_words)
			vm->edict_words = end;
	}
	if (vm->edict_words < 1)
		vm->edict_words = 1;
	vm->edict_words_used = vm->edict_words;
	vm->edict_words += 64;

	vm->max_edicts = max_edicts > 0 ? max_edicts : 1;
	globals_bytes = ((size_t)p->globalslots * 4 + 63) & ~(size_t)63;
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
	p->globals = (qcval_t *)vm->mem;
	p->globals_addr = 0;
	for (int i = 0; i < p->numglobals; i++)
		p->globals[i].i = rd32(p->progsdata + p->hdr.ofs_globals + (size_t)i * 4);
	vm->edicts = Mem_Alloc(sizeof(qcedict_t) * vm->max_edicts);
	for (int i = 0; i < vm->max_edicts; i++)
		vm->edicts[i].free = true;
	vm->edicts[0].free = false; /* world */
	vm->num_edicts = 1;
	QC_HeapInit(vm);

	vm->localstack_size = 1 << 16;
	vm->localstack = Mem_Alloc(sizeof(qcval_t) * vm->localstack_size);
	vm->rng = 0x9e3779b97f4a7c15ull ^ (uint64_t)size;

	activate(vm, 0);
	resolve_fields(vm);
	resolve_globals(vm);
	bind_builtins(vm, tables);
	store_derived(vm, 0);
	bind_autocvars(vm, 0);

	vm_list[vm_count++] = vm;
	Con_DPrintf("%s: %d functions, %d statements, %d globals, %d entity words\n", name,
	            vm->numfunctions, vm->numstatements, vm->numglobals, vm->edict_words_used);
	return vm;
}

int QC_AddProgs(qcvm_t *vm, const char *name, const void *data, size_t size)
{
	int n = vm->numprogs, prev = vm->curprogs;
	qcprogs_t *p;
	uint32_t addr;
	int f;
	for (int i = 0; i < vm->numprogs; i++)
		if (!Str_ICmp(vm->progs[i].name, name))
			return i;
	if (n >= QC_MAX_PROGS) {
		Con_Printf("%s: too many progs in %s\n", name, vm->name);
		return -1;
	}
	p = &vm->progs[n];
	if (!parse_progs(p, name, data, size)) {
		free_progs(p);
		return -1;
	}
	/* its globals live in the shared heap, so pointers to them work everywhere */
	addr = QC_HeapAlloc(vm, (size_t)p->globalslots * 4);
	if (!addr) {
		Con_Printf("%s: no heap left for its globals\n", name);
		free_progs(p);
		return -1;
	}
	p->globals_addr = addr;
	p->globals = (qcval_t *)(vm->mem + addr);
	memset(p->globals, 0, (size_t)p->globalslots * 4);
	for (int i = 0; i < p->numglobals; i++)
		p->globals[i].i = rd32(p->progsdata + p->hdr.ofs_globals + (size_t)i * 4);
	vm->numprogs++;
	if (!link_progs(vm, n)) {
		QC_HeapFree(vm, addr);
		free_progs(p);
		vm->numprogs--;
		return -1;
	}

	/* engine-side setup happens with the new progs running */
	activate(vm, n);
	resolve_globals(vm);
	bind_builtins(vm, vm->tables);
	store_derived(vm, n);
	bind_autocvars(vm, n);
	activate(vm, prev);
	Con_DPrintf("%s: added to %s as progs %d, %d functions\n", name, vm->name, n, p->numfunctions);

	/* "init(float prevprogs)" runs as soon as a progs is loaded */
	f = QC_FindFunctionIn(vm, n, "init");
	if (f) {
		QC_Parm(vm, 0)->f = (float)prev;
		vm->argc = 1;
		QC_Execute(vm, f);
	}
	return n;
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
	QC_SkelFreeVM(vm);
	if (vm->extra)
		QC_ExtraFree(vm);
	Mem_Free(vm->extra);
	for (int i = 0; i < vm->numprogs; i++)
		free_progs(&vm->progs[i]);
	Mem_Free(vm->mem);
	Mem_Free(vm->edicts);
	Mem_Free(vm->localstack);
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
	if (vm->num_edicts < vm->reserved_edicts + 1)
		vm->num_edicts = vm->reserved_edicts + 1;
	for (e = vm->reserved_edicts + 1; e < vm->num_edicts; e++) {
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
	qcfunc_t *f = QC_Function(vm, fnum);
	return f && f->builtin != bi_missing;
}
