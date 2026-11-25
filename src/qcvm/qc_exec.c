/* Bulwark engine - QuakeC interpreter loop.
 * Operand roles for every opcode are documented in docs/spec/opcodes.md.
 * ISC licence, see LICENSE. */
#include "qc_internal.h"
#include "../common/cvar.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

static int executing_count;
bool QC_AnyExecuting(void) { return executing_count > 0; }

static void trace_line(qcvm_t *vm, int progs, int f, int s)
{
	qcprogs_t *p = &vm->progs[progs];
	if (f > 0 && f < p->numfunctions)
		Con_Printf("  %s (%s) statement %d%s%s\n", p->functions[f].name, p->functions[f].file, s,
		           progs ? " in " : "", progs ? p->name : "");
}

void QC_StackTrace(qcvm_t *vm)
{
	int progs = vm->curprogs;
	trace_line(vm, progs, vm->xfunction, vm->xstatement);
	for (int i = vm->depth - 1; i >= 0; i--)
		trace_line(vm, vm->stack[i].progs, vm->stack[i].f, vm->stack[i].s);
}

void QC_RunError(qcvm_t *vm, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Printf("^1%s: QC error: %s\n", vm->name, buf);
	QC_StackTrace(vm);
	QC_CoreDump(vm, buf);
	if (!vm->abort_jmp)
		Sys_Error("%s: QC error outside of execution: %s", vm->name, buf);
	vm->aborting = false;
	longjmp(*(jmp_buf *)vm->abort_jmp, 1);
}

/* calls fnum of the running progs; caller is the progs to return to */
/* pr_trace: prints each QC function entered whose name contains this */
char qc_tracefilter[64];
bool qc_profiling;

/* word copies for the calling convention: most are one or three words,
 * which a library call would cost more than */
static inline void copy_words(qcval_t *d, const qcval_t *s, int n)
{
	if (n > 16) {
		memcpy(d, s, sizeof(qcval_t) * (size_t)n);
		return;
	}
	switch (n) {
	case 16: d[15] = s[15]; /* fall through */
	case 15: d[14] = s[14]; /* fall through */
	case 14: d[13] = s[13]; /* fall through */
	case 13: d[12] = s[12]; /* fall through */
	case 12: d[11] = s[11]; /* fall through */
	case 11: d[10] = s[10]; /* fall through */
	case 10: d[9] = s[9]; /* fall through */
	case 9: d[8] = s[8]; /* fall through */
	case 8: d[7] = s[7]; /* fall through */
	case 7: d[6] = s[6]; /* fall through */
	case 6: d[5] = s[5]; /* fall through */
	case 5: d[4] = s[4]; /* fall through */
	case 4: d[3] = s[3]; /* fall through */
	case 3: d[2] = s[2]; /* fall through */
	case 2: d[1] = s[1]; /* fall through */
	case 1: d[0] = s[0]; /* fall through */
	default: break;
	}
}

#if defined(__GNUC__) || defined(__clang__)
#define QC_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define QC_ALWAYS_INLINE inline
#endif

static QC_ALWAYS_INLINE void enter_function(qcvm_t *vm, int fnum, int returnto, int caller)
{
	qcfunc_t *f = &vm->functions[fnum];
	int locals = f->d.locals;
	int numparms = f->d.numparms;
	qcval_t *g = vm->globals;
	qcframe_t *fr;

	if (vm->depth >= QC_MAX_DEPTH)
		QC_RunError(vm, "stack overflow calling %s", f->name);
	if (qc_tracefilter[0] && strstr(f->name, qc_tracefilter))
		Con_Printf("%s: %*s%s (self %d)\n", vm->name, vm->depth, "", f->name, QC_GInt(vm, vm->gofs.self));
	fr = &vm->stack[vm->depth++];
	fr->s = returnto;
	fr->f = vm->xfunction;
	fr->progs = caller;
	fr->localbase = vm->localstack_used;

	/* save the callee's locals so recursion does not clobber them */
	if (vm->localstack_used + locals > vm->localstack_size) {
		while (vm->localstack_used + locals > vm->localstack_size)
			vm->localstack_size *= 2;
		if (vm->localstack_size > (1 << 24))
			QC_RunError(vm, "local stack overflow calling %s", f->name);
		vm->localstack = Mem_Realloc(vm->localstack, sizeof(qcval_t) * vm->localstack_size);
	}
	copy_words(vm->localstack + vm->localstack_used, g + f->d.parm_start, locals);
	vm->localstack_used += locals;

	/* copy arguments into the parameter locals */
	if (numparms < 0)
		numparms = -numparms - 1;
	if (numparms > MAX_PARMS)
		numparms = MAX_PARMS;
	{
		int o = f->d.parm_start;
		for (int i = 0; i < numparms; i++) {
			int sz = f->d.parm_size[i];
			if (o + sz > vm->globalslots)
				break;
			copy_words(g + o, g + OFS_PARM(i), sz);
			o += sz;
		}
	}
	vm->xfunction = fnum;
	if (vm->debug_hooks)
		QC_DebugFunctionEntered(vm, f);
}

/* returns the statement to continue at */
static QC_ALWAYS_INLINE int leave_function(qcvm_t *vm)
{
	qcframe_t *fr = &vm->stack[--vm->depth];
	qcfunc_t *f = &vm->functions[vm->xfunction];
	copy_words(vm->globals + f->d.parm_start, vm->localstack + fr->localbase, f->d.locals);
	vm->localstack_used = fr->localbase;
	if (fr->progs != vm->curprogs)
		QC_SwitchProgs(vm, fr->progs);
	vm->xfunction = fr->f;
	return fr->s;
}

static inline int32_t f2i(float f)
{
	if (f != f)
		return 0;
	if (f >= 2147483647.0f)
		return INT_MAX;
	if (f <= -2147483648.0f)
		return INT_MIN;
	return (int32_t)f;
}

static inline int64_t ld64(const qcval_t *p)
{
	int64_t v;
	memcpy(&v, p, 8);
	return v;
}
static inline void st64(qcval_t *p, int64_t v) { memcpy(p, &v, 8); }
static inline double ldd(const qcval_t *p)
{
	double v;
	memcpy(&v, p, 8);
	return v;
}
static inline void std_(qcval_t *p, double v) { memcpy(p, &v, 8); }

static BW_NORETURN void field_error(qcvm_t *vm, int32_t e, int32_t f)
{
	if ((uint32_t)e >= (uint32_t)vm->max_edicts)
		QC_RunError(vm, "entity %d out of range", e);
	QC_RunError(vm, "field offset %d out of range", f);
}

/* an entity field: both checks are single unsigned compares, as a negative
 * field offset wraps to a huge one (edict_words is always above 3) */
typedef struct {
	uint8_t *base; /* entity 0's fields */
	uint32_t count; /* max_edicts */
	uint32_t words; /* edict_words */
} edictspace_t;

static inline qcval_t *field_ptr(qcvm_t *vm, const edictspace_t *es, int32_t e, int32_t f, uint32_t words)
{
	if ((uint32_t)e >= es->count || (uint32_t)f > es->words - words)
		field_error(vm, e, f);
	return (qcval_t *)(es->base + ((size_t)(uint32_t)e * es->words + (uint32_t)f) * 4);
}

static inline qcval_t *mem_ptr(qcvm_t *vm, uint32_t addr, int words)
{
	if (addr & 3)
		QC_RunError(vm, "misaligned pointer 0x%x", addr);
	return (qcval_t *)QC_Pointer(vm, addr, (size_t)words * 4);
}

static inline qcval_t *global_index(qcvm_t *vm, int64_t idx, int words)
{
	if (idx < 0 || idx + words > vm->globalslots)
		QC_RunError(vm, "array index %lld out of range", (long long)idx);
	return vm->globals + idx;
}

static inline bool str_eq(qcvm_t *vm, int32_t a, int32_t b) { return a == b || QC_StringsEqual(vm, a, b); }

static inline bool str_empty(qcvm_t *vm, int32_t s) { return !s || !*QC_GetString(vm, s); }

static int string_char(qcvm_t *vm, int32_t s, int64_t idx)
{
	size_t len;
	char *p = QC_StringBuffer(vm, s, &len);
	if (!p || idx < 0 || (size_t)idx >= len)
		return 0;
	return (unsigned char)p[idx];
}

static void string_setchar(qcvm_t *vm, int32_t s, int64_t idx, int ch)
{
	size_t len;
	char *p = QC_StringBuffer(vm, s, &len);
	if (!p || idx < 0 || (size_t)idx >= len)
		QC_RunError(vm, "string index %lld out of range", (long long)idx);
	p[idx] = (char)ch;
}

/* Float truth and equality by IEEE rules, whatever the FPU does with
 * denormals. QuakeC keeps integers (function and entity numbers) in float
 * variables, where they read as denormals, and tests them with the float
 * opcodes; the 3DS runs its FPU flushing denormals to zero, which would
 * make every such value false and equal to 0. */
static inline bool ftrue(const qcval_t *v) { return (v->i & 0x7fffffff) != 0; }
/* in integer operations alone: equal bits are equal floats unless NaN, and
 * different bits are equal floats only as +0 and -0 (denormals included) */
static inline bool feq(const qcval_t *a, const qcval_t *b)
{
	uint32_t x = a->u, y = b->u;
	return x == y ? (x & 0x7fffffff) <= 0x7f800000 : ((x | y) & 0x7fffffff) == 0;
}

#define JUMP(x) ((vm->stmt32) ? (int32_t)(x) : (int32_t)(int16_t)(x))

/* A loop that never ends must jump backwards, so the runaway limit is only
 * checked on backward jumps, each adding the statements it jumps over: about
 * the same statement budget as counting every one. */
#define QC_MAX_INSTRUCTIONS (1u << 28)

static BW_NORETURN void bad_statement(qcvm_t *vm, int s, unsigned op)
{
	vm->xstatement = s;
	if (op == QCOP_END || (unsigned)s >= (unsigned)vm->numstatements)
		QC_RunError(vm, "statement %d out of range", s);
	if (op == QCOP_BAD)
		QC_RunError(vm, "unknown opcode (logged when the progs loaded)");
	QC_RunError(vm, "unsupported opcode %u", op);
}

static BW_NORETURN void runaway(qcvm_t *vm, int s)
{
	vm->xstatement = s;
	QC_RunError(vm, "runaway loop (%u statements)", QC_MAX_INSTRUCTIONS);
}

static void debug_statement(qcvm_t *vm, int s)
{
	if (vm->debug_hooks)
		QC_DebugStatement(vm, s);
	if (qc_profiling)
		vm->functions[vm->xfunction].profile++;
}

/* Labels as values let every handler end in its own indirect jump, which
 * predicts far better than one shared switch; other compilers get the
 * switch. Both run the same handler bodies, written once below. */
#if defined(__GNUC__) || defined(__clang__)
#define QC_THREADED 1
#endif

static void run(qcvm_t *vm, int s0, int entrydepth)
{
	const qcstatement_t *stmts, *st;
	qcval_t *G;
	/* fixed for the life of the VM, so kept out of the loop's loads */
	const edictspace_t es = {vm->mem + vm->edicts_ofs, (uint32_t)vm->max_edicts, (uint32_t)vm->edict_words};
	unsigned nst;
	uint32_t looped = 0;
#ifdef QC_THREADED
	/* plaintable runs fused statements one at a time, for the debugger */
	static const void *optable[QCOP_COUNT], *debugtable[QCOP_COUNT], *plaintable[QCOP_COUNT];
	const void *const *disp;
#else
	bool hooks;
#endif

#define RELOAD() (G = vm->globals, stmts = vm->statements, nst = (unsigned)vm->numstatements)
#define SINDEX() ((int)(st - stmts))
#ifdef QC_THREADED
#define CHECK_HOOKS() (disp = vm->debug_hooks || qc_profiling ? debugtable : optable)
#define OPCODE(x) L_##x:
#define DISPATCH() goto *disp[st->op]
#else
#define CHECK_HOOKS() (hooks = vm->debug_hooks || qc_profiling)
#define OPCODE(x) case x:
#define DISPATCH() goto dispatch
#endif
#define NEXT() do { st++; DISPATCH(); } while (0)
/* a relative jump; targets are checked here rather than on every statement */
#define JUMPBY(off)                                                                                \
	do {                                                                                           \
		int32_t o_ = (off);                                                                        \
		uintptr_t np_ = (uintptr_t)st + (uintptr_t)(intptr_t)o_ * sizeof(*st);                     \
		if (o_ <= 0 && (looped += 1u - (uint32_t)o_) >= QC_MAX_INSTRUCTIONS)                       \
			runaway(vm, SINDEX());                                                                 \
		if (np_ - (uintptr_t)stmts >= (uintptr_t)nst * sizeof(*st))                                \
			bad_statement(vm, SINDEX() + o_, 0);                                                   \
		st = (const qcstatement_t *)np_;                                                           \
		DISPATCH();                                                                                \
	} while (0)
#define OPS                                                                                        \
	qcval_t *a = G + st->a, *b = G + st->b, *c = G + st->c;                                        \
	(void)a, (void)b, (void)c

#ifdef QC_THREADED
	if (!optable[0]) {
		for (int i = 0; i < QCOP_COUNT; i++) {
			optable[i] = &&L_unsupported;
			debugtable[i] = &&L_debug;
		}
#define T(x) optable[x] = &&L_##x;
		T(OP_ADD_F) T(OP_SUB_F) T(OP_MUL_F) T(OP_DIV_F) T(OP_ADD_V) T(OP_SUB_V) T(OP_MUL_V)
		T(OP_MUL_FV) T(OP_MUL_VF) T(OP_DIV_VF) T(OP_MUL_VI) T(OP_MUL_IV) T(OP_BITAND) T(OP_BITOR)
		T(OP_AND_F) T(OP_OR_F)
		T(OP_EQ_F) T(OP_NE_F) T(OP_LE_F) T(OP_GE_F) T(OP_LT_F) T(OP_GT_F) T(OP_EQ_V) T(OP_NE_V)
		T(OP_EQ_S) T(OP_NE_S) T(OP_EQ_E) T(OP_EQ_FNC) T(OP_NE_E) T(OP_NE_FNC) T(OP_NOT_F)
		T(OP_NOT_V) T(OP_NOT_S) T(OP_NOT_ENT) T(OP_NOT_FNC)
		T(OP_LOADF_F) T(OP_LOADF_S) T(OP_LOADF_ENT) T(OP_LOADF_FLD) T(OP_LOADF_FNC) T(OP_LOADF_I)
		T(OP_LOADF_P) T(OP_LOADF_V) T(OP_LOADF_I64) T(OP_FLDADDRESS) T(OP_STOREF_F) T(OP_STOREF_S)
		T(OP_STOREF_I) T(OP_STOREF_V) T(OP_STOREF_I64)
		T(OP_STORE_F) T(OP_STORE_S) T(OP_STORE_ENT) T(OP_STORE_FLD) T(OP_STORE_FNC) T(OP_STORE_I)
		T(OP_STORE_P) T(OP_STORE_V) T(OP_STORE_I64) T(OP_STORE_IF) T(OP_STORE_FI)
		T(OP_STOREP_F) T(OP_STOREP_S) T(OP_STOREP_ENT) T(OP_STOREP_FLD) T(OP_STOREP_FNC)
		T(OP_STOREP_I) T(OP_STOREP_V) T(OP_STOREP_I64) T(OP_STOREP_IF) T(OP_STOREP_FI)
		T(OP_STOREP_C) T(OP_STOREP_I8)
		T(OP_LOADP_F) T(OP_LOADP_S) T(OP_LOADP_ENT) T(OP_LOADP_FLD) T(OP_LOADP_FNC) T(OP_LOADP_I)
		T(OP_LOADP_V) T(OP_LOADP_I64) T(OP_LOADP_C) T(OP_LOADP_U8)
		T(OP_LOADA_F) T(OP_LOADA_S) T(OP_LOADA_ENT) T(OP_LOADA_FLD) T(OP_LOADA_FNC) T(OP_LOADA_I)
		T(OP_LOADA_V) T(OP_LOADA_I64) T(OP_GLOBALADDRESS) T(OP_ADD_PIW) T(OP_BOUNDCHECK)
		T(OP_MULSTORE_F) T(OP_DIVSTORE_F) T(OP_ADDSTORE_F) T(OP_SUBSTORE_F) T(OP_BITSETSTORE_F)
		T(OP_BITCLRSTORE_F) T(OP_MULSTOREP_F) T(OP_DIVSTOREP_F) T(OP_ADDSTOREP_F) T(OP_SUBSTOREP_F)
		T(OP_BITSETSTOREP_F) T(OP_BITCLRSTOREP_F) T(OP_MULSTORE_VF) T(OP_MULSTOREP_VF)
		T(OP_ADDSTORE_V) T(OP_ADDSTOREP_V) T(OP_SUBSTORE_V) T(OP_SUBSTOREP_V)
		T(OP_ADD_I) T(OP_SUB_I) T(OP_MUL_I) T(OP_DIV_I) T(OP_DIV_U) T(OP_ADD_FI) T(OP_ADD_IF)
		T(OP_SUB_FI) T(OP_SUB_IF) T(OP_MUL_FI) T(OP_MUL_IF) T(OP_DIV_FI) T(OP_DIV_IF) T(OP_CONV_IF)
		T(OP_CONV_FI) T(OP_BITAND_I) T(OP_BITOR_I) T(OP_BITXOR_I) T(OP_LSHIFT_I) T(OP_RSHIFT_I)
		T(OP_RSHIFT_U) T(OP_BITAND_IF) T(OP_BITOR_IF) T(OP_BITAND_FI) T(OP_BITOR_FI) T(OP_EQ_I)
		T(OP_NE_I) T(OP_LE_I) T(OP_GE_I) T(OP_LT_I) T(OP_GT_I) T(OP_LE_U) T(OP_LT_U) T(OP_LE_IF)
		T(OP_GE_IF) T(OP_LT_IF) T(OP_GT_IF) T(OP_LE_FI) T(OP_GE_FI) T(OP_LT_FI) T(OP_GT_FI)
		T(OP_EQ_IF) T(OP_EQ_FI) T(OP_NE_IF) T(OP_NE_FI) T(OP_AND_I) T(OP_OR_I) T(OP_AND_IF)
		T(OP_OR_IF) T(OP_AND_FI) T(OP_OR_FI) T(OP_NOT_I)
		T(OP_ADD_I64) T(OP_SUB_I64) T(OP_MUL_I64) T(OP_DIV_I64) T(OP_DIV_U64) T(OP_BITAND_I64)
		T(OP_BITOR_I64) T(OP_BITXOR_I64) T(OP_LSHIFT_I64I) T(OP_RSHIFT_I64I) T(OP_RSHIFT_U64I)
		T(OP_LE_I64) T(OP_LT_I64) T(OP_EQ_I64) T(OP_NE_I64) T(OP_LE_U64) T(OP_LT_U64)
		T(OP_CONV_UI64) T(OP_CONV_II64) T(OP_CONV_I64I) T(OP_CONV_FD) T(OP_CONV_DF) T(OP_CONV_I64F)
		T(OP_CONV_FI64) T(OP_CONV_I64D) T(OP_CONV_DI64) T(OP_ADD_D) T(OP_SUB_D) T(OP_MUL_D)
		T(OP_DIV_D) T(OP_LE_D) T(OP_LT_D) T(OP_EQ_D) T(OP_NE_D)
		T(OP_ADD_SF) T(OP_SUB_S)
		T(OP_RAND0) T(OP_RAND1) T(OP_RAND2) T(OP_RANDV0) T(OP_RANDV1) T(OP_RANDV2)
		T(OP_IF) T(OP_IFNOT) T(OP_IF_F) T(OP_IFNOT_F) T(OP_IFS) T(OP_IFNOTS) T(OP_GOTO)
		T(OP_SWITCH_F) T(OP_SWITCH_V) T(OP_SWITCH_S) T(OP_SWITCH_E) T(OP_SWITCH_FNC) T(OP_SWITCH_I)
		T(OP_CASE) T(OP_CASERANGE) T(OP_STATE) T(OP_THINKTIME)
		T(OP_CALL0) T(OP_CALL1) T(OP_CALL2) T(OP_CALL3) T(OP_CALL4) T(OP_CALL5) T(OP_CALL6)
		T(OP_CALL7) T(OP_CALL8) T(OP_CALL1H) T(OP_CALL2H) T(OP_CALL3H) T(OP_CALL4H) T(OP_CALL5H)
		T(OP_CALL6H) T(OP_CALL7H) T(OP_CALL8H)
		T(OP_RETURN) T(OP_DONE)
		optable[QCOP_END] = &&L_unsupported;
		for (int i = 0; i < QCOP_COUNT; i++)
			plaintable[i] = optable[QC_PlainOp((unsigned)i)];
		T(QCOP_NEF_IFNOT) T(QCOP_EQF_IFNOT) T(QCOP_EQS_IFNOT) T(QCOP_LTI_IFNOT)
#undef T
	}
#endif

	RELOAD();
	CHECK_HOOKS();
	st = stmts + s0;
	DISPATCH();

#ifdef QC_THREADED
L_debug:
	debug_statement(vm, SINDEX());
	goto *plaintable[st->op];
#else
dispatch:
	if (hooks)
		debug_statement(vm, SINDEX());
	switch (hooks ? QC_PlainOp(st->op) : st->op) {
#endif

	/* --- float/vector arithmetic --- */
	OPCODE(OP_ADD_F) { OPS; c->f = a->f + b->f; NEXT(); }
	OPCODE(OP_SUB_F) { OPS; c->f = a->f - b->f; NEXT(); }
	OPCODE(OP_MUL_F) { OPS; c->f = a->f * b->f; NEXT(); }
	OPCODE(OP_DIV_F) { OPS; c->f = a->f / b->f; NEXT(); }
	OPCODE(OP_ADD_V) {
		OPS;
		c[0].f = a[0].f + b[0].f; c[1].f = a[1].f + b[1].f; c[2].f = a[2].f + b[2].f;
		NEXT();
	}
	OPCODE(OP_SUB_V) {
		OPS;
		c[0].f = a[0].f - b[0].f; c[1].f = a[1].f - b[1].f; c[2].f = a[2].f - b[2].f;
		NEXT();
	}
	OPCODE(OP_MUL_V) { OPS; c->f = a[0].f * b[0].f + a[1].f * b[1].f + a[2].f * b[2].f; NEXT(); }
	OPCODE(OP_MUL_FV) {
		OPS;
		float f = a->f;
		c[0].f = f * b[0].f; c[1].f = f * b[1].f; c[2].f = f * b[2].f;
		NEXT();
	}
	OPCODE(OP_MUL_VF) {
		OPS;
		float f = b->f;
		c[0].f = a[0].f * f; c[1].f = a[1].f * f; c[2].f = a[2].f * f;
		NEXT();
	}
	OPCODE(OP_DIV_VF) {
		OPS;
		float f = b->f;
		c[0].f = a[0].f / f; c[1].f = a[1].f / f; c[2].f = a[2].f / f;
		NEXT();
	}
	OPCODE(OP_MUL_VI) {
		OPS;
		float f = (float)b->i;
		c[0].f = a[0].f * f; c[1].f = a[1].f * f; c[2].f = a[2].f * f;
		NEXT();
	}
	OPCODE(OP_MUL_IV) {
		OPS;
		float f = (float)a->i;
		c[0].f = f * b[0].f; c[1].f = f * b[1].f; c[2].f = f * b[2].f;
		NEXT();
	}
	OPCODE(OP_BITAND) { OPS; c->f = (float)(f2i(a->f) & f2i(b->f)); NEXT(); }
	OPCODE(OP_BITOR) { OPS; c->f = (float)(f2i(a->f) | f2i(b->f)); NEXT(); }
	OPCODE(OP_AND_F) { OPS; c->f = (ftrue(a) && ftrue(b)); NEXT(); }
	OPCODE(OP_OR_F) { OPS; c->f = (ftrue(a) || ftrue(b)); NEXT(); }

	/* --- comparisons with float results --- */
	OPCODE(OP_EQ_F) { OPS; c->f = feq(a, b); NEXT(); }
	OPCODE(OP_NE_F) { OPS; c->f = !feq(a, b); NEXT(); }
	OPCODE(OP_LE_F) { OPS; c->f = a->f <= b->f; NEXT(); }
	OPCODE(OP_GE_F) { OPS; c->f = a->f >= b->f; NEXT(); }
	OPCODE(OP_LT_F) { OPS; c->f = a->f < b->f; NEXT(); }
	OPCODE(OP_GT_F) { OPS; c->f = a->f > b->f; NEXT(); }
	OPCODE(OP_EQ_V) { OPS; c->f = a[0].f == b[0].f && a[1].f == b[1].f && a[2].f == b[2].f; NEXT(); }
	OPCODE(OP_NE_V) { OPS; c->f = a[0].f != b[0].f || a[1].f != b[1].f || a[2].f != b[2].f; NEXT(); }
	OPCODE(OP_EQ_S) { OPS; c->f = str_eq(vm, a->i, b->i); NEXT(); }
	OPCODE(OP_NE_S) { OPS; c->f = !str_eq(vm, a->i, b->i); NEXT(); }
	OPCODE(OP_EQ_E) OPCODE(OP_EQ_FNC) { OPS; c->f = a->i == b->i; NEXT(); }
	OPCODE(OP_NE_E) OPCODE(OP_NE_FNC) { OPS; c->f = a->i != b->i; NEXT(); }
	OPCODE(OP_NOT_F) { OPS; c->f = !ftrue(a); NEXT(); }
	OPCODE(OP_NOT_V) { OPS; c->f = !ftrue(&a[0]) && !ftrue(&a[1]) && !ftrue(&a[2]); NEXT(); }
	OPCODE(OP_NOT_S) { OPS; c->f = str_empty(vm, a->i); NEXT(); }
	OPCODE(OP_NOT_ENT) OPCODE(OP_NOT_FNC) { OPS; c->f = !a->i; NEXT(); }

	/* --- entity fields --- */
	OPCODE(OP_LOADF_F) OPCODE(OP_LOADF_S) OPCODE(OP_LOADF_ENT) OPCODE(OP_LOADF_FLD)
	OPCODE(OP_LOADF_FNC) OPCODE(OP_LOADF_I) OPCODE(OP_LOADF_P) {
		OPS;
		c->i = field_ptr(vm, &es, a->i, b->i, 1)->i;
		NEXT();
	}
	OPCODE(OP_LOADF_V) {
		OPS;
		qcval_t *p = field_ptr(vm, &es, a->i, b->i, 3);
		c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
		NEXT();
	}
	OPCODE(OP_LOADF_I64) {
		OPS;
		qcval_t *p = field_ptr(vm, &es, a->i, b->i, 2);
		c[0] = p[0]; c[1] = p[1];
		NEXT();
	}
	OPCODE(OP_FLDADDRESS) {
		OPS;
		field_ptr(vm, &es, a->i, b->i, 1);
		c->u = (uint32_t)(vm->edicts_ofs + ((size_t)a->i * vm->edict_words + b->i) * 4);
		NEXT();
	}
	OPCODE(OP_STOREF_F) OPCODE(OP_STOREF_S) OPCODE(OP_STOREF_I) {
		OPS;
		field_ptr(vm, &es, a->i, b->i, 1)->i = c->i;
		NEXT();
	}
	OPCODE(OP_STOREF_V) {
		OPS;
		qcval_t *p = field_ptr(vm, &es, a->i, b->i, 3);
		p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
		NEXT();
	}
	OPCODE(OP_STOREF_I64) {
		OPS;
		qcval_t *p = field_ptr(vm, &es, a->i, b->i, 2);
		p[0] = c[0]; p[1] = c[1];
		NEXT();
	}

	/* --- plain stores (b = a) --- */
	OPCODE(OP_STORE_F) OPCODE(OP_STORE_S) OPCODE(OP_STORE_ENT) OPCODE(OP_STORE_FLD)
	OPCODE(OP_STORE_FNC) OPCODE(OP_STORE_I) OPCODE(OP_STORE_P) { OPS; b->i = a->i; NEXT(); }
	OPCODE(OP_STORE_V) { OPS; b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; NEXT(); }
	OPCODE(OP_STORE_I64) { OPS; b[0] = a[0]; b[1] = a[1]; NEXT(); }
	OPCODE(OP_STORE_IF) { OPS; b->f = (float)a->i; NEXT(); }
	OPCODE(OP_STORE_FI) { OPS; b->i = f2i(a->f); NEXT(); }

	/* --- pointer stores: *(b + c words) = a --- */
	OPCODE(OP_STOREP_F) OPCODE(OP_STOREP_S) OPCODE(OP_STOREP_ENT) OPCODE(OP_STOREP_FLD)
	OPCODE(OP_STOREP_FNC) OPCODE(OP_STOREP_I) {
		OPS;
		mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->i = a->i;
		NEXT();
	}
	OPCODE(OP_STOREP_V) {
		OPS;
		qcval_t *p = mem_ptr(vm, b->u + (uint32_t)c->i * 4, 3);
		p[0] = a[0]; p[1] = a[1]; p[2] = a[2];
		NEXT();
	}
	OPCODE(OP_STOREP_I64) {
		OPS;
		qcval_t *p = mem_ptr(vm, b->u + (uint32_t)c->i * 4, 2);
		p[0] = a[0]; p[1] = a[1];
		NEXT();
	}
	OPCODE(OP_STOREP_IF) { OPS; mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->f = (float)a->i; NEXT(); }
	OPCODE(OP_STOREP_FI) { OPS; mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->i = f2i(a->f); NEXT(); }
	OPCODE(OP_STOREP_C) { OPS; string_setchar(vm, b->i, c->i, f2i(a->f)); NEXT(); }
	OPCODE(OP_STOREP_I8) { OPS; string_setchar(vm, b->i, c->i, a->i); NEXT(); }

	/* --- pointer loads: c = *(a + b words) --- */
	OPCODE(OP_LOADP_F) OPCODE(OP_LOADP_S) OPCODE(OP_LOADP_ENT) OPCODE(OP_LOADP_FLD)
	OPCODE(OP_LOADP_FNC) OPCODE(OP_LOADP_I) {
		OPS;
		c->i = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 1)->i;
		NEXT();
	}
	OPCODE(OP_LOADP_V) {
		OPS;
		qcval_t *p = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 3);
		c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
		NEXT();
	}
	OPCODE(OP_LOADP_I64) {
		OPS;
		qcval_t *p = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 2);
		c[0] = p[0]; c[1] = p[1];
		NEXT();
	}
	OPCODE(OP_LOADP_C) { OPS; c->f = (float)string_char(vm, a->i, f2i(b->f)); NEXT(); }
	OPCODE(OP_LOADP_U8) { OPS; c->i = string_char(vm, a->i, b->i); NEXT(); }

	/* --- global arrays and addresses --- */
	OPCODE(OP_LOADA_F) OPCODE(OP_LOADA_S) OPCODE(OP_LOADA_ENT) OPCODE(OP_LOADA_FLD)
	OPCODE(OP_LOADA_FNC) OPCODE(OP_LOADA_I) {
		OPS;
		c->i = global_index(vm, (int64_t)st->a + b->i, 1)->i;
		NEXT();
	}
	OPCODE(OP_LOADA_V) {
		OPS;
		qcval_t *p = global_index(vm, (int64_t)st->a + b->i, 3);
		c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
		NEXT();
	}
	OPCODE(OP_LOADA_I64) {
		OPS;
		qcval_t *p = global_index(vm, (int64_t)st->a + b->i, 2);
		c[0] = p[0]; c[1] = p[1];
		NEXT();
	}
	OPCODE(OP_GLOBALADDRESS) { OPS; c->u = vm->globals_addr + (uint32_t)(st->a + b->i) * 4; NEXT(); }
	OPCODE(OP_ADD_PIW) { OPS; c->u = a->u + (uint32_t)b->i * 4; NEXT(); }
	OPCODE(OP_BOUNDCHECK) {
		OPS;
		int32_t lo = (int32_t)st->c, hi = (int32_t)st->b;
		if (a->i < lo || a->i >= hi) {
			vm->xstatement = SINDEX();
			QC_RunError(vm, "array index %d out of bounds %d..%d", a->i, lo, hi - 1);
		}
		NEXT();
	}

	/* --- compound assignment, b op= a, result also written to c --- */
#define CSTORE_F(expr)                                                                             \
	{                                                                                              \
		OPS;                                                                                       \
		b->f = (expr);                                                                             \
		if (st->c) c->f = b->f;                                                                    \
		NEXT();                                                                                    \
	}
#define CSTOREP_F(expr)                                                                            \
	{                                                                                              \
		OPS;                                                                                       \
		qcval_t *p = mem_ptr(vm, b->u, 1);                                                         \
		p->f = (expr);                                                                             \
		if (st->c) c->f = p->f;                                                                    \
		NEXT();                                                                                    \
	}
	OPCODE(OP_MULSTORE_F) CSTORE_F(b->f * a->f)
	OPCODE(OP_DIVSTORE_F) CSTORE_F(b->f / a->f)
	OPCODE(OP_ADDSTORE_F) CSTORE_F(b->f + a->f)
	OPCODE(OP_SUBSTORE_F) CSTORE_F(b->f - a->f)
	OPCODE(OP_BITSETSTORE_F) CSTORE_F((float)(f2i(b->f) | f2i(a->f)))
	OPCODE(OP_BITCLRSTORE_F) CSTORE_F((float)(f2i(b->f) & ~f2i(a->f)))
	OPCODE(OP_MULSTOREP_F) CSTOREP_F(p->f * a->f)
	OPCODE(OP_DIVSTOREP_F) CSTOREP_F(p->f / a->f)
	OPCODE(OP_ADDSTOREP_F) CSTOREP_F(p->f + a->f)
	OPCODE(OP_SUBSTOREP_F) CSTOREP_F(p->f - a->f)
	OPCODE(OP_BITSETSTOREP_F) CSTOREP_F((float)(f2i(p->f) | f2i(a->f)))
	OPCODE(OP_BITCLRSTOREP_F) CSTOREP_F((float)(f2i(p->f) & ~f2i(a->f)))
#undef CSTORE_F
#undef CSTOREP_F
	OPCODE(OP_MULSTORE_VF) OPCODE(OP_MULSTOREP_VF) {
		OPS;
		qcval_t *p = st->op == OP_MULSTORE_VF ? b : mem_ptr(vm, b->u, 3);
		float f = a->f;
		p[0].f *= f; p[1].f *= f; p[2].f *= f;
		if (st->c) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; }
		NEXT();
	}
	OPCODE(OP_ADDSTORE_V) OPCODE(OP_ADDSTOREP_V) OPCODE(OP_SUBSTORE_V) OPCODE(OP_SUBSTOREP_V) {
		OPS;
		bool ptr = st->op == OP_ADDSTOREP_V || st->op == OP_SUBSTOREP_V;
		float sign = (st->op == OP_SUBSTORE_V || st->op == OP_SUBSTOREP_V) ? -1.0f : 1.0f;
		qcval_t *p = ptr ? mem_ptr(vm, b->u, 3) : b;
		p[0].f += sign * a[0].f; p[1].f += sign * a[1].f; p[2].f += sign * a[2].f;
		if (st->c) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; }
		NEXT();
	}

	/* --- integers --- */
	OPCODE(OP_ADD_I) { OPS; c->i = (int32_t)((uint32_t)a->i + (uint32_t)b->i); NEXT(); }
	OPCODE(OP_SUB_I) { OPS; c->i = (int32_t)((uint32_t)a->i - (uint32_t)b->i); NEXT(); }
	OPCODE(OP_MUL_I) { OPS; c->i = (int32_t)((uint32_t)a->i * (uint32_t)b->i); NEXT(); }
	OPCODE(OP_DIV_I) {
		OPS;
		if (!b->i) c->i = 0;
		else if (a->i == INT_MIN && b->i == -1) c->i = INT_MIN;
		else c->i = a->i / b->i;
		NEXT();
	}
	OPCODE(OP_DIV_U) { OPS; c->u = b->u ? a->u / b->u : 0; NEXT(); }
	OPCODE(OP_ADD_FI) { OPS; c->f = a->f + (float)b->i; NEXT(); }
	OPCODE(OP_ADD_IF) { OPS; c->f = (float)a->i + b->f; NEXT(); }
	OPCODE(OP_SUB_FI) { OPS; c->f = a->f - (float)b->i; NEXT(); }
	OPCODE(OP_SUB_IF) { OPS; c->f = (float)a->i - b->f; NEXT(); }
	OPCODE(OP_MUL_FI) { OPS; c->f = a->f * (float)b->i; NEXT(); }
	OPCODE(OP_MUL_IF) { OPS; c->f = (float)a->i * b->f; NEXT(); }
	OPCODE(OP_DIV_FI) { OPS; c->f = a->f / (float)b->i; NEXT(); }
	OPCODE(OP_DIV_IF) { OPS; c->f = (float)a->i / b->f; NEXT(); }
	OPCODE(OP_CONV_IF) { OPS; c->f = (float)a->i; NEXT(); }
	OPCODE(OP_CONV_FI) { OPS; c->i = f2i(a->f); NEXT(); }
	OPCODE(OP_BITAND_I) { OPS; c->i = a->i & b->i; NEXT(); }
	OPCODE(OP_BITOR_I) { OPS; c->i = a->i | b->i; NEXT(); }
	OPCODE(OP_BITXOR_I) { OPS; c->i = a->i ^ b->i; NEXT(); }
	OPCODE(OP_LSHIFT_I) { OPS; c->u = a->u << (b->i & 31); NEXT(); }
	OPCODE(OP_RSHIFT_I) { OPS; c->i = a->i >> (b->i & 31); NEXT(); }
	OPCODE(OP_RSHIFT_U) { OPS; c->u = a->u >> (b->i & 31); NEXT(); }
	OPCODE(OP_BITAND_IF) { OPS; c->i = a->i & f2i(b->f); NEXT(); }
	OPCODE(OP_BITOR_IF) { OPS; c->i = a->i | f2i(b->f); NEXT(); }
	OPCODE(OP_BITAND_FI) { OPS; c->i = f2i(a->f) & b->i; NEXT(); }
	OPCODE(OP_BITOR_FI) { OPS; c->i = f2i(a->f) | b->i; NEXT(); }
	OPCODE(OP_EQ_I) { OPS; c->i = a->i == b->i; NEXT(); }
	OPCODE(OP_NE_I) { OPS; c->i = a->i != b->i; NEXT(); }
	OPCODE(OP_LE_I) { OPS; c->i = a->i <= b->i; NEXT(); }
	OPCODE(OP_GE_I) { OPS; c->i = a->i >= b->i; NEXT(); }
	OPCODE(OP_LT_I) { OPS; c->i = a->i < b->i; NEXT(); }
	OPCODE(OP_GT_I) { OPS; c->i = a->i > b->i; NEXT(); }
	OPCODE(OP_LE_U) { OPS; c->i = a->u <= b->u; NEXT(); }
	OPCODE(OP_LT_U) { OPS; c->i = a->u < b->u; NEXT(); }
	OPCODE(OP_LE_IF) { OPS; c->i = (float)a->i <= b->f; NEXT(); }
	OPCODE(OP_GE_IF) { OPS; c->i = (float)a->i >= b->f; NEXT(); }
	OPCODE(OP_LT_IF) { OPS; c->i = (float)a->i < b->f; NEXT(); }
	OPCODE(OP_GT_IF) { OPS; c->i = (float)a->i > b->f; NEXT(); }
	OPCODE(OP_LE_FI) { OPS; c->i = a->f <= (float)b->i; NEXT(); }
	OPCODE(OP_GE_FI) { OPS; c->i = a->f >= (float)b->i; NEXT(); }
	OPCODE(OP_LT_FI) { OPS; c->i = a->f < (float)b->i; NEXT(); }
	OPCODE(OP_GT_FI) { OPS; c->i = a->f > (float)b->i; NEXT(); }
	OPCODE(OP_EQ_IF) { OPS; c->i = (float)a->i == b->f; NEXT(); }
	OPCODE(OP_EQ_FI) { OPS; c->i = a->f == (float)b->i; NEXT(); }
	OPCODE(OP_NE_IF) { OPS; c->i = (float)a->i != b->f; NEXT(); }
	OPCODE(OP_NE_FI) { OPS; c->i = a->f != (float)b->i; NEXT(); }
	OPCODE(OP_AND_I) { OPS; c->i = a->i && b->i; NEXT(); }
	OPCODE(OP_OR_I) { OPS; c->i = a->i || b->i; NEXT(); }
	OPCODE(OP_AND_IF) { OPS; c->i = a->i && ftrue(b); NEXT(); }
	OPCODE(OP_OR_IF) { OPS; c->i = a->i || ftrue(b); NEXT(); }
	OPCODE(OP_AND_FI) { OPS; c->i = ftrue(a) && b->i; NEXT(); }
	OPCODE(OP_OR_FI) { OPS; c->i = ftrue(a) || b->i; NEXT(); }
	OPCODE(OP_NOT_I) { OPS; c->i = !a->i; NEXT(); }

	/* --- 64-bit integers and doubles --- */
	OPCODE(OP_ADD_I64) { OPS; st64(c, (int64_t)((uint64_t)ld64(a) + (uint64_t)ld64(b))); NEXT(); }
	OPCODE(OP_SUB_I64) { OPS; st64(c, (int64_t)((uint64_t)ld64(a) - (uint64_t)ld64(b))); NEXT(); }
	OPCODE(OP_MUL_I64) { OPS; st64(c, (int64_t)((uint64_t)ld64(a) * (uint64_t)ld64(b))); NEXT(); }
	OPCODE(OP_DIV_I64) {
		OPS;
		int64_t x = ld64(a), y = ld64(b);
		st64(c, (!y || (x == INT64_MIN && y == -1)) ? 0 : x / y);
		NEXT();
	}
	OPCODE(OP_DIV_U64) {
		OPS;
		uint64_t x = (uint64_t)ld64(a), y = (uint64_t)ld64(b);
		st64(c, (int64_t)(y ? x / y : 0));
		NEXT();
	}
	OPCODE(OP_BITAND_I64) { OPS; st64(c, ld64(a) & ld64(b)); NEXT(); }
	OPCODE(OP_BITOR_I64) { OPS; st64(c, ld64(a) | ld64(b)); NEXT(); }
	OPCODE(OP_BITXOR_I64) { OPS; st64(c, ld64(a) ^ ld64(b)); NEXT(); }
	OPCODE(OP_LSHIFT_I64I) { OPS; st64(c, (int64_t)((uint64_t)ld64(a) << (b->i & 63))); NEXT(); }
	OPCODE(OP_RSHIFT_I64I) { OPS; st64(c, ld64(a) >> (b->i & 63)); NEXT(); }
	OPCODE(OP_RSHIFT_U64I) { OPS; st64(c, (int64_t)((uint64_t)ld64(a) >> (b->i & 63))); NEXT(); }
	OPCODE(OP_LE_I64) { OPS; c->i = ld64(a) <= ld64(b); NEXT(); }
	OPCODE(OP_LT_I64) { OPS; c->i = ld64(a) < ld64(b); NEXT(); }
	/* FTEQCC types these results as 64-bit: "if (x != y)" on two int64s
	 * tests the result with a second NE_I64 against a 64-bit zero, which
	 * reads both words. Writing only the low one left the high word as
	 * whatever the temp held before (Nuclide's FindXRInput then never
	 * matched and spawned three new NSXRInput entities each respawn). */
	OPCODE(OP_EQ_I64) { OPS; st64(c, ld64(a) == ld64(b)); NEXT(); }
	OPCODE(OP_NE_I64) { OPS; st64(c, ld64(a) != ld64(b)); NEXT(); }
	OPCODE(OP_LE_U64) { OPS; c->i = (uint64_t)ld64(a) <= (uint64_t)ld64(b); NEXT(); }
	OPCODE(OP_LT_U64) { OPS; c->i = (uint64_t)ld64(a) < (uint64_t)ld64(b); NEXT(); }
	OPCODE(OP_CONV_UI64) { OPS; st64(c, (int64_t)a->u); NEXT(); }
	OPCODE(OP_CONV_II64) { OPS; st64(c, (int64_t)a->i); NEXT(); }
	OPCODE(OP_CONV_I64I) { OPS; c->i = (int32_t)ld64(a); NEXT(); }
	OPCODE(OP_CONV_FD) { OPS; std_(c, (double)a->f); NEXT(); }
	OPCODE(OP_CONV_DF) { OPS; c->f = (float)ldd(a); NEXT(); }
	OPCODE(OP_CONV_I64F) { OPS; c->f = (float)ld64(a); NEXT(); }
	OPCODE(OP_CONV_FI64) { OPS; st64(c, (int64_t)a->f); NEXT(); }
	OPCODE(OP_CONV_I64D) { OPS; std_(c, (double)ld64(a)); NEXT(); }
	OPCODE(OP_CONV_DI64) { OPS; st64(c, (int64_t)ldd(a)); NEXT(); }
	OPCODE(OP_ADD_D) { OPS; std_(c, ldd(a) + ldd(b)); NEXT(); }
	OPCODE(OP_SUB_D) { OPS; std_(c, ldd(a) - ldd(b)); NEXT(); }
	OPCODE(OP_MUL_D) { OPS; std_(c, ldd(a) * ldd(b)); NEXT(); }
	OPCODE(OP_DIV_D) { OPS; std_(c, ldd(a) / ldd(b)); NEXT(); }
	OPCODE(OP_LE_D) { OPS; c->i = ldd(a) <= ldd(b); NEXT(); }
	OPCODE(OP_LT_D) { OPS; c->i = ldd(a) < ldd(b); NEXT(); }
	OPCODE(OP_EQ_D) { OPS; c->i = ldd(a) == ldd(b); NEXT(); }
	OPCODE(OP_NE_D) { OPS; c->i = ldd(a) != ldd(b); NEXT(); }

	/* --- strings --- */
	OPCODE(OP_ADD_SF) {
		OPS;
		/* pointer arithmetic on a progs string; engine strings cannot be offset */
		if ((uint32_t)a->i & QC_STRING_RUNTIME) {
			size_t len;
			char *p = QC_StringBuffer(vm, a->i, &len);
			int32_t ofs = f2i(b->f);
			c->i = (p && ofs >= 0 && (size_t)ofs <= len) ? QC_NewString(vm, p + ofs) : 0;
		} else {
			c->i = a->i + f2i(b->f);
		}
		NEXT();
	}
	OPCODE(OP_SUB_S) { OPS; c->i = a->i - b->i; NEXT(); }

	/* --- random --- */
	OPCODE(OP_RAND0) { OPS; c->f = QC_Random(vm); NEXT(); }
	OPCODE(OP_RAND1) { OPS; c->f = QC_Random(vm) * a->f; NEXT(); }
	OPCODE(OP_RAND2) {
		OPS;
		float lo = a->f, hi = b->f;
		c->f = lo + QC_Random(vm) * (hi - lo);
		NEXT();
	}
	OPCODE(OP_RANDV0) {
		OPS;
		c[0].f = QC_Random(vm); c[1].f = QC_Random(vm); c[2].f = QC_Random(vm);
		NEXT();
	}
	OPCODE(OP_RANDV1) {
		OPS;
		for (int i = 0; i < 3; i++) c[i].f = QC_Random(vm) * a[i].f;
		NEXT();
	}
	OPCODE(OP_RANDV2) {
		OPS;
		for (int i = 0; i < 3; i++) c[i].f = a[i].f + QC_Random(vm) * (b[i].f - a[i].f);
		NEXT();
	}

	/* --- control flow --- */
	OPCODE(OP_IF) { if (G[st->a].i) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_IFNOT) { if (!G[st->a].i) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_IF_F) { if (ftrue(&G[st->a])) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_IFNOT_F) { if (!ftrue(&G[st->a])) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_IFS) { if (!str_empty(vm, G[st->a].i)) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_IFNOTS) { if (str_empty(vm, G[st->a].i)) JUMPBY(JUMP(st->b)); NEXT(); }
	OPCODE(OP_GOTO) { JUMPBY(JUMP(st->a)); }

	/* a compare and the IFNOT after it; the result is still stored, as
	 * later code may read it */
#define FUSED_IFNOT(store)                                                                         \
	{                                                                                              \
		OPS;                                                                                       \
		store;                                                                                     \
		st++;                                                                                      \
		if (!G[st->a].i)                                                                           \
			JUMPBY(JUMP(st->b));                                                                   \
		NEXT();                                                                                    \
	}
	OPCODE(QCOP_NEF_IFNOT) FUSED_IFNOT(c->f = !feq(a, b))
	OPCODE(QCOP_EQF_IFNOT) FUSED_IFNOT(c->f = feq(a, b))
	OPCODE(QCOP_EQS_IFNOT) FUSED_IFNOT(c->f = str_eq(vm, a->i, b->i))
	OPCODE(QCOP_LTI_IFNOT) FUSED_IFNOT(c->i = a->i < b->i)
#undef FUSED_IFNOT

	OPCODE(OP_SWITCH_F) OPCODE(OP_SWITCH_V) OPCODE(OP_SWITCH_S) OPCODE(OP_SWITCH_E)
	OPCODE(OP_SWITCH_FNC) OPCODE(OP_SWITCH_I) {
		OPS;
		vm->switch_op = (int)st->op;
		vm->switch_value[0] = a[0].u;
		if (st->op == OP_SWITCH_V) {
			vm->switch_value[1] = a[1].u;
			vm->switch_value[2] = a[2].u;
		}
		JUMPBY(JUMP(st->b));
	}
	OPCODE(OP_CASE) {
		OPS;
		bool match;
		qcval_t sv[3];
		sv[0].u = vm->switch_value[0];
		sv[1].u = vm->switch_value[1];
		sv[2].u = vm->switch_value[2];
		switch (vm->switch_op) {
		case OP_SWITCH_F: match = sv[0].f == a->f; break;
		case OP_SWITCH_V: match = sv[0].f == a[0].f && sv[1].f == a[1].f && sv[2].f == a[2].f; break;
		case OP_SWITCH_S: match = str_eq(vm, sv[0].i, a->i); break;
		default: match = sv[0].i == a->i; break;
		}
		if (match)
			JUMPBY(JUMP(st->b));
		NEXT();
	}
	OPCODE(OP_CASERANGE) {
		OPS;
		qcval_t sv;
		bool match;
		sv.u = vm->switch_value[0];
		if (vm->switch_op == OP_SWITCH_F)
			match = sv.f >= a->f && sv.f <= b->f;
		else if (vm->switch_op == OP_SWITCH_I || vm->switch_op == OP_SWITCH_E)
			match = sv.i >= a->i && sv.i <= b->i;
		else {
			vm->xstatement = SINDEX();
			QC_RunError(vm, "case range used in a switch that cannot order its values");
		}
		if (match)
			JUMPBY(JUMP(st->c));
		NEXT();
	}

	OPCODE(OP_STATE) {
		OPS;
		int self = QC_GInt(vm, vm->gofs.self);
		QC_SetEFloat(vm, self, vm->fofs.frame, a->f);
		QC_SetEInt(vm, self, vm->fofs.think, b->i);
		QC_SetEFloat(vm, self, vm->fofs.nextthink, QC_GFloat(vm, vm->gofs.time) + 0.1f);
		NEXT();
	}
	OPCODE(OP_THINKTIME) {
		OPS;
		QC_SetEFloat(vm, a->i, vm->fofs.nextthink, QC_GFloat(vm, vm->gofs.time) + b->f);
		NEXT();
	}

	OPCODE(OP_CALL0) OPCODE(OP_CALL1) OPCODE(OP_CALL2) OPCODE(OP_CALL3) OPCODE(OP_CALL4)
	OPCODE(OP_CALL5) OPCODE(OP_CALL6) OPCODE(OP_CALL7) OPCODE(OP_CALL8)
	OPCODE(OP_CALL1H) OPCODE(OP_CALL2H) OPCODE(OP_CALL3H) OPCODE(OP_CALL4H)
	OPCODE(OP_CALL5H) OPCODE(OP_CALL6H) OPCODE(OP_CALL7H) OPCODE(OP_CALL8H) {
		OPS;
		int target = a->i, caller = vm->curprogs, s = SINDEX();
		int fnum = QC_FUNC_INDEX(target), pn = QC_FUNC_PROGS(target);
		unsigned op = st->op;
		qcfunc_t *f;
		if (op >= OP_CALL1H) {
			vm->argc = (int)(op - OP_CALL1H) + 1;
			/* the first two arguments travel in the b and c operands */
			memmove(G + OFS_PARM(0), b, sizeof(qcval_t) * 3);
			if (vm->argc >= 2)
				memmove(G + OFS_PARM(1), c, sizeof(qcval_t) * 3);
		} else {
			vm->argc = (int)(op - OP_CALL0);
		}
		vm->xstatement = s;
		if (pn >= vm->numprogs || fnum <= 0 || fnum >= vm->progs[pn].numfunctions)
			QC_RunError(vm, "call to %s function (%d)", target ? "invalid" : "NULL", target);
		if (pn != caller)
			QC_SwitchProgs(vm, pn);
		f = &vm->functions[fnum];
		if (f->builtin) {
			vm->extra->calling_builtin = fnum;
			if (vm->profiling) {
				double t0 = Sys_Seconds();
				f->builtin(vm);
				f->prof_time += Sys_Seconds() - t0;
				f->prof_calls++;
			} else {
				f->builtin(vm);
			}
			if (vm->curprogs != caller)
				QC_SwitchProgs(vm, caller);
			/* a builtin may have changed a cvar the progs reads as an autocvar */
			if (vm->autocvar_gen != cvar_generation)
				QC_SyncAutocvars(vm);
			RELOAD();
			CHECK_HOOKS();
			st = stmts + s;
			NEXT();
		}
		enter_function(vm, fnum, s + 1, caller);
		RELOAD();
		CHECK_HOOKS();
		st = stmts + f->d.first_statement;
		DISPATCH();
	}

	OPCODE(OP_RETURN) {
		OPS;
		G[OFS_RETURN] = a[0];
		G[OFS_RETURN + 1] = a[1];
		G[OFS_RETURN + 2] = a[2];
		goto done;
	}
	OPCODE(OP_DONE) {
	done:;
		int s = leave_function(vm);
		if (vm->depth <= entrydepth)
			return;
		RELOAD();
		CHECK_HOOKS();
		st = stmts + s;
		DISPATCH();
	}

#ifndef QC_THREADED
	default:
		goto L_unsupported;
	}
#endif
L_unsupported:
	bad_statement(vm, SINDEX(), st->op);

#undef RELOAD
#undef SINDEX
#undef CHECK_HOOKS
#undef OPCODE
#undef DISPATCH
#undef NEXT
#undef JUMPBY
#undef OPS
}


bool QC_Execute(qcvm_t *vm, func_t fnum)
{
	jmp_buf jb;
	void *oldjmp = vm->abort_jmp;
	int entrydepth = vm->depth;
	int oldfunc = vm->xfunction, oldstmt = vm->xstatement;
	int oldlocals = vm->localstack_used;
	int oldprogs = vm->curprogs, pn = QC_FUNC_PROGS(fnum);
	qcfunc_t *f = QC_Function(vm, fnum);


	if (!f) {
		Con_Printf("%s: tried to execute a NULL function\n", vm->name);
		return false;
	}
	const int idx = QC_FUNC_INDEX(fnum);
	if (!vm->exec_depth)
		QC_SyncAutocvars(vm);
	executing_count++;
	vm->exec_depth++;
	if (setjmp(jb)) {
		/* error or abort: unwind every frame this call created */
		while (vm->depth > entrydepth) {
			qcframe_t *fr = &vm->stack[--vm->depth];
			qcfunc_t *cf = &vm->functions[vm->xfunction];
			memcpy(vm->globals + cf->d.parm_start, vm->localstack + fr->localbase, sizeof(qcval_t) * cf->d.locals);
			if (fr->progs != vm->curprogs)
				QC_SwitchProgs(vm, fr->progs);
			vm->xfunction = fr->f;
		}
		if (vm->curprogs != oldprogs)
			QC_SwitchProgs(vm, oldprogs);
		vm->localstack_used = oldlocals;
		vm->xfunction = oldfunc;
		vm->xstatement = oldstmt;
		vm->abort_jmp = oldjmp;
		executing_count--;
		vm->exec_depth--;
		/* errors and aborts stop the whole QC call chain, not just this level */
		if (oldjmp)
			longjmp(*(jmp_buf *)oldjmp, 1);
		if (vm->aborting) {
			vm->aborting = false;
			vm->globals[OFS_RETURN] = vm->abort_value;
			return true;
		}
		return false;
	}
	vm->abort_jmp = &jb;

	if (pn != oldprogs)
		QC_SwitchProgs(vm, pn);
	if (f->builtin) {
		vm->extra->calling_builtin = idx;
		f->builtin(vm);
		if (vm->curprogs != oldprogs)
			QC_SwitchProgs(vm, oldprogs);
	} else {
		/* the frame returns to oldprogs */
		enter_function(vm, idx, -1, oldprogs);
		run(vm, f->d.first_statement, entrydepth);
	}

	vm->abort_jmp = oldjmp;
	vm->xfunction = oldfunc;
	vm->xstatement = oldstmt;
	executing_count--;
	vm->exec_depth--;
	return true;
}

bool QC_ExecuteByName(qcvm_t *vm, const char *name)
{
	int f = QC_FindFunction(vm, name);
	if (!f)
		return false;
	return QC_Execute(vm, f);
}

void QC_AbortUnwind(qcvm_t *vm)
{
	if (!vm->abort_jmp)
		Sys_Error("%s: abort() outside of execution", vm->name);
	vm->aborting = true;
	longjmp(*(jmp_buf *)vm->abort_jmp, 1);
}
