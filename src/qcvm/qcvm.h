/* Bulwark engine - QuakeC virtual machine. ISC licence, see LICENSE.
 *
 * Address space: every progs instance owns one byte arena laid out as
 *   [ globals | entity field storage | memalloc heap ]
 * Pointers seen by QC are byte offsets into that arena, so &global, &ent.field
 * and memalloc results can all be dereferenced by the same LOADP/STOREP code.
 * Entity values are entity numbers (world is 0). Field values are word offsets
 * within one entity. Strings are 32-bit handles, see qc_strings.c.
 */
#ifndef BW_QCVM_H
#define BW_QCVM_H

#include "../common/common.h"
#include "progs.h"

typedef int32_t string_t;
typedef int32_t func_t;

typedef union {
	float f;
	int32_t i;
	uint32_t u;
} qcval_t;

typedef struct qcvm_s qcvm_t;
typedef void (*qc_builtin_fn)(qcvm_t *vm);

typedef struct {
	const char *name;
	qc_builtin_fn fn;
} qc_builtin_t;

typedef struct {
	dfunction_t d;
	const char *name;
	const char *file;
	qc_builtin_fn builtin; /* non-NULL for builtins */
	int builtin_num;
} qcfunc_t;

typedef struct {
	uint32_t type;
	uint32_t ofs;
	const char *name;
} qcdef_t;

/* engine-side bookkeeping for each entity slot */
typedef struct {
	bool free;
	float freetime;
	void *host; /* server or client side data, owned by the host */
} qcedict_t;

typedef enum { QCVM_SERVER, QCVM_CLIENT, QCVM_MENU, QCVM_TEST } qcvm_kind_t;

/* fields and globals the engine reads or writes by name; -1 when the progs lacks one */
typedef struct {
	int classname, model, modelindex, origin, angles, velocity, avelocity, mins, maxs, size;
	int absmin, absmax, solid, movetype, nextthink, think, touch, use, blocked, frame, skin;
	int flags, owner, chain, health, ltime, effects, colormap, gravity, groundentity;
	int customphysics, predraw, drawmask, renderflags, alpha, scale, sendentity, sendflags;
	int pvsflags, dimension_solid, dimension_hit, hitcontentsmaski, nextthink_lerp;
	int v_angle, fixangle, button0, button2, impulse, waterlevel, watertype, view_ofs, netname;
} qcfieldofs_t;

typedef struct {
	int self, other, world, time, frametime, mapname, serverflags, force_retouch;
	int v_forward, v_up, v_right;
	int trace_allsolid, trace_startsolid, trace_fraction, trace_endpos, trace_plane_normal;
	int trace_plane_dist, trace_ent, trace_inopen, trace_inwater, trace_endcontentsi;
	int trace_surfaceflagsi, trace_surfacename, trace_brush_id, trace_brush_faceid;
	int trace_surface_id, trace_bone_id, trace_triangle_id;
	int msg_entity, parm1, cltime, thisprogs, deathmatch, coop;
} qcglobalofs_t;

#define QC_MAX_DEPTH 1024
#define QC_MAX_EXTPARMS 24

typedef struct {
	int s; /* statement to resume at */
	int f; /* function index */
	int progs; /* which progs f belongs to */
	int localbase; /* where the callee's saved locals start on the local stack */
} qcframe_t;

typedef struct {
	int *slots;
	int size;
} qc_namehash_t;

/* Several progs can share one VM (FTE_MULTIPROGS): they share the entity
 * space, the heap and the system globals, and each keeps its own code,
 * strings and other globals. Function and static string values carry their
 * progs number in bits 24-30 so they stay valid wherever they are stored;
 * the main progs is number 0, so its values are plain indices. Fields of
 * an added progs are mapped onto the main progs' layout by name. */
#define QC_MAX_PROGS 16
#define QC_PROGS_SHIFT 24
#define QC_INDEX_MASK 0x00ffffff
#define QC_FUNC_PROGS(f) ((int)(((uint32_t)(f) >> QC_PROGS_SHIFT) & 0x7f))
#define QC_FUNC_INDEX(f) ((int)((uint32_t)(f) & QC_INDEX_MASK))

/* the parts of a VM that belong to one progs; the VM's own copies of these
 * fields are those of the progs that is running now */
/* a global named autocvar_<name>, kept equal to cvar <name> */
typedef struct {
	struct cvar_s *var;
	int ofs;
	int type;
} qcautocvar_t;

typedef struct {
	char name[64];
	uint8_t *progsdata;
	size_t progssize;
	progs_header_t hdr;
	bool stmt32;
	dstatement32_t *statements;
	int numstatements;
	qcfunc_t *functions;
	int numfunctions;
	qcdef_t *globaldefs;
	int numglobaldefs;
	qcdef_t *fielddefs;
	int numfielddefs;
	char *strings;
	int stringsize;
	qcval_t *globals;
	uint32_t globals_addr;
	int numglobals;
	int globalslots;
	int sysglobals;
	qcglobalofs_t gofs;
	int extparm_ofs[QC_MAX_EXTPARMS];
	uint8_t *missing_reported;
	qc_namehash_t globalhash, fieldhash, funchash;
	qcautocvar_t *autocvars;
	int numautocvars;
} qcprogs_t;


struct qcvm_s {
	qcvm_kind_t kind;
	char name[64];
	void *host;

	uint8_t *progsdata; /* file image, kept for string table and statements */
	size_t progssize;
	progs_header_t hdr;
	bool stmt32;

	/* statements unpacked into 32-bit form */
	dstatement32_t *statements;
	int numstatements;

	qcfunc_t *functions;
	int numfunctions;

	qcdef_t *globaldefs;
	int numglobaldefs;
	qcdef_t *fielddefs;
	int numfielddefs;

	char *strings; /* progs string table (mutable copy) */
	int stringsize;

	/* the arena */
	uint8_t *mem;
	size_t memsize;
	qcval_t *globals; /* the running progs' globals: mem for the main progs */
	uint32_t globals_addr; /* their byte address in the arena */
	int numglobals; /* as declared by the progs */
	int globalslots; /* words reserved for globals in the arena */
	int sysglobals; /* leading globals shared by every progs (up to end_sys_globals) */
	size_t edicts_ofs; /* byte offset of entity 0 */
	int edict_words; /* field storage per entity in words */
	int edict_words_used; /* the rest is spare room for fields added progs declare */
	int max_edicts;
	int num_edicts; /* highest slot in use + 1 */
	int reserved_edicts; /* slots 1..reserved are never handed out by spawn() (player slots) */
	qcedict_t *edicts;
	size_t heap_ofs, heap_size;

	qcfieldofs_t fofs;
	qcglobalofs_t gofs;
	int extparm_ofs[QC_MAX_EXTPARMS]; /* $parm8 onwards, -1 if absent */

	/* execution state */
	int argc;
	int xfunction; /* currently executing function */
	int xstatement;
	qcframe_t stack[QC_MAX_DEPTH];
	int depth;
	qcval_t *localstack;
	int localstack_used, localstack_size;
	int exec_depth; /* nested QC_Execute calls (builtins calling back into QC) */
	void *abort_jmp;
	bool aborting;
	qcval_t abort_value;
	uint32_t switch_value[3];
	int switch_op;

	/* tokenize/argv state */
	struct qc_tokens_s *tokens;

	/* per-module helper tables (files, hash tables, json) */
	struct qc_extra_s *extra;

	/* random */
	uint64_t rng;

	/* builtins that could not be bound, reported once each when called */
	uint8_t *missing_reported;

	/* multiprogs */
	qcprogs_t progs[QC_MAX_PROGS];
	int numprogs;
	int curprogs;
	int autocvar_gen; /* Cvar_Generation() when the autocvars were last copied */
	const qc_builtin_t *const *tables;

	/* the model name behind a modelindex, for skeletons and tags */
	const char *(*modelname)(qcvm_t *vm, int modelindex);
};

/* string handles: 0 is the empty/null string, small values index the progs
 * string table, values with the top bit set are engine-made strings */
#define QC_STRING_RUNTIME 0x80000000u

/* lifecycle */
qcvm_t *QC_Load(const char *name, const void *data, size_t size, qcvm_kind_t kind,
                const qc_builtin_t *const *tables, int max_edicts, size_t heapsize);
void QC_Free(qcvm_t *vm);

/* multiprogs: adds a progs to the VM and runs its init(); returns its number or -1 */
int QC_AddProgs(qcvm_t *vm, const char *name, const void *data, size_t size);
void QC_SwitchProgs(qcvm_t *vm, int progs);
/* the FTE_MULTIPROGS handle convention: 0 main, -1 the running progs, -2
 * whichever progs defines name first; returns -1 when there is no match */
int QC_ResolveProgs(qcvm_t *vm, float handle, const char *name, bool function);
int QC_FindFunctionIn(qcvm_t *vm, int progs, const char *name);
qcdef_t *QC_FindGlobalIn(qcvm_t *vm, int progs, const char *name);
qcval_t *QC_ProgsGlobals(qcvm_t *vm, int progs);
/* the definition of a (possibly progs-tagged) function value, or NULL */
qcfunc_t *QC_Function(qcvm_t *vm, func_t f);

/* lookup in the running progs; function results are progs-tagged */
int QC_FindFunction(qcvm_t *vm, const char *name);
qcdef_t *QC_FindGlobal(qcvm_t *vm, const char *name);
qcdef_t *QC_FindField(qcvm_t *vm, const char *name);
int QC_GlobalOfs(qcvm_t *vm, const char *name);
int QC_FieldOfs(qcvm_t *vm, const char *name);
const char *QC_FunctionName(qcvm_t *vm, func_t f);

/* execution; returns false if QC raised an error (already reported) */
bool QC_Execute(qcvm_t *vm, func_t fnum);
bool QC_ExecuteByName(qcvm_t *vm, const char *name);
/* copies changed cvars into autocvar_ globals; QC_Execute does this on entry */
void QC_SyncAutocvars(qcvm_t *vm);
BW_NORETURN void QC_RunError(qcvm_t *vm, const char *fmt, ...) BW_PRINTF(2, 3);
void QC_StackTrace(qcvm_t *vm);
extern char qc_tracefilter[64]; /* non-empty: print entries to matching functions */

/* strings */
const char *QC_GetString(qcvm_t *vm, string_t s);
string_t QC_NewString(qcvm_t *vm, const char *s);
string_t QC_NewStringLen(qcvm_t *vm, const char *s, size_t len);
/* writable view into an engine string or the progs table, or NULL */
char *QC_StringBuffer(qcvm_t *vm, string_t s, size_t *len);
void QC_CollectGarbage(qcvm_t *vm, bool force);
int QC_LiveStrings(void);

/* entities */
qcval_t *QC_EdictFields(qcvm_t *vm, int e);
int QC_SpawnEdict(qcvm_t *vm);
void QC_FreeEdict(qcvm_t *vm, int e, float now);
void QC_ClearEdict(qcvm_t *vm, int e);
bool QC_ValidEdict(qcvm_t *vm, int e);
/* parse "key" "value" pairs from a { } block into the entity; returns pointer after the block */
const char *QC_ParseEdict(qcvm_t *vm, int e, const char *data, bool *ok);
bool QC_SetFieldFromString(qcvm_t *vm, int e, qcdef_t *field, const char *value);
void QC_FieldToString(qcvm_t *vm, int e, qcdef_t *field, char *out, size_t outsize);

/* memory */
void *QC_Pointer(qcvm_t *vm, uint32_t addr, size_t bytes); /* errors if out of range */
uint32_t QC_HeapAlloc(qcvm_t *vm, size_t bytes);
void QC_HeapFree(qcvm_t *vm, uint32_t addr);
uint32_t QC_HeapRealloc(qcvm_t *vm, uint32_t addr, size_t bytes);

/* random numbers in [0,1) */
float QC_Random(qcvm_t *vm);

/* parameter and return helpers for builtins */
qcval_t *QC_Parm(qcvm_t *vm, int n);
#define QC_PFLOAT(vm, n) (QC_Parm(vm, n)->f)
#define QC_PINT(vm, n) (QC_Parm(vm, n)->i)
#define QC_PVEC(vm, n) (&QC_Parm(vm, n)->f)
#define QC_PSTR(vm, n) QC_GetString(vm, QC_Parm(vm, n)->i)
#define QC_PENT(vm, n) (QC_Parm(vm, n)->i)
#define QC_RETURN(vm) ((vm)->globals + OFS_RETURN)
static inline void QC_ReturnFloat(qcvm_t *vm, float f) { vm->globals[OFS_RETURN].f = f; }
static inline void QC_ReturnInt(qcvm_t *vm, int32_t i) { vm->globals[OFS_RETURN].i = i; }
static inline void QC_ReturnVector(qcvm_t *vm, const float *v)
{
	vm->globals[OFS_RETURN].f = v[0];
	vm->globals[OFS_RETURN + 1].f = v[1];
	vm->globals[OFS_RETURN + 2].f = v[2];
}
static inline void QC_ReturnString(qcvm_t *vm, const char *s) { vm->globals[OFS_RETURN].i = QC_NewString(vm, s); }

/* global/field accessors by offset (offset < 0 means absent) */
static inline float QC_GFloat(qcvm_t *vm, int ofs) { return ofs < 0 ? 0 : vm->globals[ofs].f; }
static inline void QC_SetGFloat(qcvm_t *vm, int ofs, float v) { if (ofs >= 0) vm->globals[ofs].f = v; }
static inline int32_t QC_GInt(qcvm_t *vm, int ofs) { return ofs < 0 ? 0 : vm->globals[ofs].i; }
static inline void QC_SetGInt(qcvm_t *vm, int ofs, int32_t v) { if (ofs >= 0) vm->globals[ofs].i = v; }
static inline void QC_SetGVec(qcvm_t *vm, int ofs, const float *v)
{
	if (ofs >= 0) {
		vm->globals[ofs].f = v[0];
		vm->globals[ofs + 1].f = v[1];
		vm->globals[ofs + 2].f = v[2];
	}
}
static inline qcval_t *QC_EField(qcvm_t *vm, int e, int ofs)
{
	return ofs < 0 ? NULL : QC_EdictFields(vm, e) + ofs;
}
static inline float QC_EFloat(qcvm_t *vm, int e, int ofs)
{
	return ofs < 0 ? 0 : QC_EdictFields(vm, e)[ofs].f;
}
static inline void QC_SetEFloat(qcvm_t *vm, int e, int ofs, float v)
{
	if (ofs >= 0)
		QC_EdictFields(vm, e)[ofs].f = v;
}
static inline int32_t QC_EInt(qcvm_t *vm, int e, int ofs)
{
	return ofs < 0 ? 0 : QC_EdictFields(vm, e)[ofs].i;
}
static inline void QC_SetEInt(qcvm_t *vm, int e, int ofs, int32_t v)
{
	if (ofs >= 0)
		QC_EdictFields(vm, e)[ofs].i = v;
}

/* builtin tables shared by every module */
extern const qc_builtin_t qc_builtins_core[];
extern const qc_builtin_t qc_builtins_extra[];
/* skeleton objects and tags; every VM falls back to these */
extern const qc_builtin_t qc_builtins_skel[];
/* model events and frameforaction on studio models (qc_modelevents.c) */
extern const qc_builtin_t qc_builtins_modelevents[];
struct studio_model_s;
/* the GoldSrc studio model behind a model index, or NULL */
const struct studio_model_s *QC_StudioModel(qcvm_t *vm, int modelindex);

struct skelmodel_s;
/* the bones of skeleton object id, for drawing */
bool QC_SkelBones(qcvm_t *vm, int id, struct skelmodel_s **model, const float (**bones)[12], bool *absolute);
void QC_SkelFreeVM(qcvm_t *vm);

#endif
