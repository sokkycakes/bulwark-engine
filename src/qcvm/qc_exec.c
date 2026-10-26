/* Bulwark engine - QuakeC interpreter loop.
 * Operand roles for every opcode are documented in docs/spec/opcodes.md.
 * ISC licence, see LICENSE. */
#include "qc_internal.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

static int executing_count;
bool QC_AnyExecuting(void) { return executing_count > 0; }

#define QC_MAX_INSTRUCTIONS (1u << 28)

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
	if (!vm->abort_jmp)
		Sys_Error("%s: QC error outside of execution: %s", vm->name, buf);
	vm->aborting = false;
	longjmp(*(jmp_buf *)vm->abort_jmp, 1);
}

/* calls fnum of the running progs; caller is the progs to return to */
/* pr_trace: prints each QC function entered whose name contains this */
char qc_tracefilter[64];

static void enter_function(qcvm_t *vm, int fnum, int returnto, int caller)
{
	qcfunc_t *f = &vm->functions[fnum];
	int locals = f->d.locals;
	int numparms = f->d.numparms;
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
	memcpy(vm->localstack + vm->localstack_used, vm->globals + f->d.parm_start, sizeof(qcval_t) * locals);
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
			memcpy(vm->globals + o, vm->globals + OFS_PARM(i), sizeof(qcval_t) * sz);
			o += sz;
		}
	}
	vm->xfunction = fnum;
}

/* returns the statement to continue at */
static int leave_function(qcvm_t *vm)
{
	qcframe_t *fr = &vm->stack[--vm->depth];
	qcfunc_t *f = &vm->functions[vm->xfunction];
	memcpy(vm->globals + f->d.parm_start, vm->localstack + fr->localbase, sizeof(qcval_t) * f->d.locals);
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

static inline qcval_t *field_ptr(qcvm_t *vm, int32_t e, int32_t f, int words)
{
	if ((uint32_t)e >= (uint32_t)vm->max_edicts)
		QC_RunError(vm, "entity %d out of range", e);
	if (f < 0 || f + words > vm->edict_words)
		QC_RunError(vm, "field offset %d out of range", f);
	return (qcval_t *)(vm->mem + vm->edicts_ofs + ((size_t)e * vm->edict_words + f) * 4);
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

static bool str_eq(qcvm_t *vm, int32_t a, int32_t b)
{
	if (a == b)
		return true;
	return !strcmp(QC_GetString(vm, a), QC_GetString(vm, b));
}

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

#define JUMP(x) ((vm->stmt32) ? (int32_t)(x) : (int32_t)(int16_t)(x))

static void run(qcvm_t *vm, int s, int entrydepth)
{
	qcval_t *G = vm->globals;
	uint32_t count = 0;
	const dstatement32_t *st;
	qcval_t *a, *b, *c;

	for (;;) {
		if (++count >= QC_MAX_INSTRUCTIONS) {
			vm->xstatement = s;
			QC_RunError(vm, "runaway loop (%u instructions)", count);
		}
		if ((unsigned)s >= (unsigned)vm->numstatements) {
			vm->xstatement = s;
			QC_RunError(vm, "statement %d out of range", s);
		}
		st = &vm->statements[s];
		a = G + st->a;
		b = G + st->b;
		c = G + st->c;

		switch (st->op) {
		/* --- float/vector arithmetic --- */
		case OP_ADD_F: c->f = a->f + b->f; break;
		case OP_SUB_F: c->f = a->f - b->f; break;
		case OP_MUL_F: c->f = a->f * b->f; break;
		case OP_DIV_F: c->f = a->f / b->f; break;
		case OP_ADD_V:
			c[0].f = a[0].f + b[0].f; c[1].f = a[1].f + b[1].f; c[2].f = a[2].f + b[2].f;
			break;
		case OP_SUB_V:
			c[0].f = a[0].f - b[0].f; c[1].f = a[1].f - b[1].f; c[2].f = a[2].f - b[2].f;
			break;
		case OP_MUL_V: c->f = a[0].f * b[0].f + a[1].f * b[1].f + a[2].f * b[2].f; break;
		case OP_MUL_FV: {
			float f = a->f;
			c[0].f = f * b[0].f; c[1].f = f * b[1].f; c[2].f = f * b[2].f;
			break;
		}
		case OP_MUL_VF: {
			float f = b->f;
			c[0].f = a[0].f * f; c[1].f = a[1].f * f; c[2].f = a[2].f * f;
			break;
		}
		case OP_DIV_VF: {
			float f = b->f;
			c[0].f = a[0].f / f; c[1].f = a[1].f / f; c[2].f = a[2].f / f;
			break;
		}
		case OP_MUL_VI: {
			float f = (float)b->i;
			c[0].f = a[0].f * f; c[1].f = a[1].f * f; c[2].f = a[2].f * f;
			break;
		}
		case OP_MUL_IV: {
			float f = (float)a->i;
			c[0].f = f * b[0].f; c[1].f = f * b[1].f; c[2].f = f * b[2].f;
			break;
		}
		case OP_BITAND: c->f = (float)(f2i(a->f) & f2i(b->f)); break;
		case OP_BITOR: c->f = (float)(f2i(a->f) | f2i(b->f)); break;
		case OP_AND_F: c->f = (a->f && b->f); break;
		case OP_OR_F: c->f = (a->f || b->f); break;

		/* --- comparisons with float results --- */
		case OP_EQ_F: c->f = a->f == b->f; break;
		case OP_NE_F: c->f = a->f != b->f; break;
		case OP_LE_F: c->f = a->f <= b->f; break;
		case OP_GE_F: c->f = a->f >= b->f; break;
		case OP_LT_F: c->f = a->f < b->f; break;
		case OP_GT_F: c->f = a->f > b->f; break;
		case OP_EQ_V: c->f = a[0].f == b[0].f && a[1].f == b[1].f && a[2].f == b[2].f; break;
		case OP_NE_V: c->f = a[0].f != b[0].f || a[1].f != b[1].f || a[2].f != b[2].f; break;
		case OP_EQ_S: c->f = str_eq(vm, a->i, b->i); break;
		case OP_NE_S: c->f = !str_eq(vm, a->i, b->i); break;
		case OP_EQ_E: case OP_EQ_FNC: c->f = a->i == b->i; break;
		case OP_NE_E: case OP_NE_FNC: c->f = a->i != b->i; break;
		case OP_NOT_F: c->f = !a->f; break;
		case OP_NOT_V: c->f = !a[0].f && !a[1].f && !a[2].f; break;
		case OP_NOT_S: c->f = str_empty(vm, a->i); break;
		case OP_NOT_ENT: case OP_NOT_FNC: c->f = !a->i; break;

		/* --- entity fields --- */
		case OP_LOADF_F: case OP_LOADF_S: case OP_LOADF_ENT: case OP_LOADF_FLD:
		case OP_LOADF_FNC: case OP_LOADF_I: case OP_LOADF_P:
			c->i = field_ptr(vm, a->i, b->i, 1)->i;
			break;
		case OP_LOADF_V: {
			qcval_t *p = field_ptr(vm, a->i, b->i, 3);
			c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
			break;
		}
		case OP_LOADF_I64: {
			qcval_t *p = field_ptr(vm, a->i, b->i, 2);
			c[0] = p[0]; c[1] = p[1];
			break;
		}
		case OP_FLDADDRESS:
			field_ptr(vm, a->i, b->i, 1);
			c->u = (uint32_t)(vm->edicts_ofs + ((size_t)a->i * vm->edict_words + b->i) * 4);
			break;
		case OP_STOREF_F: case OP_STOREF_S: case OP_STOREF_I:
			field_ptr(vm, a->i, b->i, 1)->i = c->i;
			break;
		case OP_STOREF_V: {
			qcval_t *p = field_ptr(vm, a->i, b->i, 3);
			p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
			break;
		}
		case OP_STOREF_I64: {
			qcval_t *p = field_ptr(vm, a->i, b->i, 2);
			p[0] = c[0]; p[1] = c[1];
			break;
		}

		/* --- plain stores (b = a) --- */
		case OP_STORE_F: case OP_STORE_S: case OP_STORE_ENT: case OP_STORE_FLD:
		case OP_STORE_FNC: case OP_STORE_I: case OP_STORE_P:
			b->i = a->i;
			break;
		case OP_STORE_V: b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; break;
		case OP_STORE_I64: b[0] = a[0]; b[1] = a[1]; break;
		case OP_STORE_IF: b->f = (float)a->i; break;
		case OP_STORE_FI: b->i = f2i(a->f); break;

		/* --- pointer stores: *(b + c words) = a --- */
		case OP_STOREP_F: case OP_STOREP_S: case OP_STOREP_ENT: case OP_STOREP_FLD:
		case OP_STOREP_FNC: case OP_STOREP_I:
			mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->i = a->i;
			break;
		case OP_STOREP_V: {
			qcval_t *p = mem_ptr(vm, b->u + (uint32_t)c->i * 4, 3);
			p[0] = a[0]; p[1] = a[1]; p[2] = a[2];
			break;
		}
		case OP_STOREP_I64: {
			qcval_t *p = mem_ptr(vm, b->u + (uint32_t)c->i * 4, 2);
			p[0] = a[0]; p[1] = a[1];
			break;
		}
		case OP_STOREP_IF: mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->f = (float)a->i; break;
		case OP_STOREP_FI: mem_ptr(vm, b->u + (uint32_t)c->i * 4, 1)->i = f2i(a->f); break;
		case OP_STOREP_C: string_setchar(vm, b->i, c->i, f2i(a->f)); break;
		case OP_STOREP_I8: string_setchar(vm, b->i, c->i, a->i); break;

		/* --- pointer loads: c = *(a + b words) --- */
		case OP_LOADP_F: case OP_LOADP_S: case OP_LOADP_ENT: case OP_LOADP_FLD:
		case OP_LOADP_FNC: case OP_LOADP_I:
			c->i = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 1)->i;
			break;
		case OP_LOADP_V: {
			qcval_t *p = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 3);
			c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
			break;
		}
		case OP_LOADP_I64: {
			qcval_t *p = mem_ptr(vm, a->u + (uint32_t)b->i * 4, 2);
			c[0] = p[0]; c[1] = p[1];
			break;
		}
		case OP_LOADP_C: c->f = (float)string_char(vm, a->i, f2i(b->f)); break;
		case OP_LOADP_U8: c->i = string_char(vm, a->i, b->i); break;

		/* --- global arrays and addresses --- */
		case OP_LOADA_F: case OP_LOADA_S: case OP_LOADA_ENT: case OP_LOADA_FLD:
		case OP_LOADA_FNC: case OP_LOADA_I:
			c->i = global_index(vm, (int64_t)st->a + b->i, 1)->i;
			break;
		case OP_LOADA_V: {
			qcval_t *p = global_index(vm, (int64_t)st->a + b->i, 3);
			c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
			break;
		}
		case OP_LOADA_I64: {
			qcval_t *p = global_index(vm, (int64_t)st->a + b->i, 2);
			c[0] = p[0]; c[1] = p[1];
			break;
		}
		case OP_GLOBALADDRESS: c->u = vm->globals_addr + (uint32_t)(st->a + b->i) * 4; break;
		case OP_ADD_PIW: c->u = a->u + (uint32_t)b->i * 4; break;
		case OP_BOUNDCHECK: {
			int32_t lo = (int32_t)st->c, hi = (int32_t)st->b;
			if (a->i < lo || a->i >= hi) {
				vm->xstatement = s;
				QC_RunError(vm, "array index %d out of bounds %d..%d", a->i, lo, hi - 1);
			}
			break;
		}

		/* --- compound assignment, b op= a, result also written to c --- */
#define CSTORE_F(expr)                                                                             \
	{                                                                                              \
		b->f = (expr);                                                                             \
		if (st->c) c->f = b->f;                                                                    \
		break;                                                                                     \
	}
#define CSTOREP_F(expr)                                                                            \
	{                                                                                              \
		qcval_t *p = mem_ptr(vm, b->u, 1);                                                         \
		p->f = (expr);                                                                             \
		if (st->c) c->f = p->f;                                                                    \
		break;                                                                                     \
	}
		case OP_MULSTORE_F: CSTORE_F(b->f * a->f)
		case OP_DIVSTORE_F: CSTORE_F(b->f / a->f)
		case OP_ADDSTORE_F: CSTORE_F(b->f + a->f)
		case OP_SUBSTORE_F: CSTORE_F(b->f - a->f)
		case OP_BITSETSTORE_F: CSTORE_F((float)(f2i(b->f) | f2i(a->f)))
		case OP_BITCLRSTORE_F: CSTORE_F((float)(f2i(b->f) & ~f2i(a->f)))
		case OP_MULSTOREP_F: CSTOREP_F(p->f * a->f)
		case OP_DIVSTOREP_F: CSTOREP_F(p->f / a->f)
		case OP_ADDSTOREP_F: CSTOREP_F(p->f + a->f)
		case OP_SUBSTOREP_F: CSTOREP_F(p->f - a->f)
		case OP_BITSETSTOREP_F: CSTOREP_F((float)(f2i(p->f) | f2i(a->f)))
		case OP_BITCLRSTOREP_F: CSTOREP_F((float)(f2i(p->f) & ~f2i(a->f)))
#undef CSTORE_F
#undef CSTOREP_F
		case OP_MULSTORE_VF: case OP_MULSTOREP_VF: {
			qcval_t *p = st->op == OP_MULSTORE_VF ? b : mem_ptr(vm, b->u, 3);
			float f = a->f;
			p[0].f *= f; p[1].f *= f; p[2].f *= f;
			if (st->c) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; }
			break;
		}
		case OP_ADDSTORE_V: case OP_ADDSTOREP_V: case OP_SUBSTORE_V: case OP_SUBSTOREP_V: {
			bool ptr = st->op == OP_ADDSTOREP_V || st->op == OP_SUBSTOREP_V;
			float sign = (st->op == OP_SUBSTORE_V || st->op == OP_SUBSTOREP_V) ? -1.0f : 1.0f;
			qcval_t *p = ptr ? mem_ptr(vm, b->u, 3) : b;
			p[0].f += sign * a[0].f; p[1].f += sign * a[1].f; p[2].f += sign * a[2].f;
			if (st->c) { c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; }
			break;
		}

		/* --- integers --- */
		case OP_ADD_I: c->i = (int32_t)((uint32_t)a->i + (uint32_t)b->i); break;
		case OP_SUB_I: c->i = (int32_t)((uint32_t)a->i - (uint32_t)b->i); break;
		case OP_MUL_I: c->i = (int32_t)((uint32_t)a->i * (uint32_t)b->i); break;
		case OP_DIV_I:
			if (!b->i) c->i = 0;
			else if (a->i == INT_MIN && b->i == -1) c->i = INT_MIN;
			else c->i = a->i / b->i;
			break;
		case OP_DIV_U: c->u = b->u ? a->u / b->u : 0; break;
		case OP_ADD_FI: c->f = a->f + (float)b->i; break;
		case OP_ADD_IF: c->f = (float)a->i + b->f; break;
		case OP_SUB_FI: c->f = a->f - (float)b->i; break;
		case OP_SUB_IF: c->f = (float)a->i - b->f; break;
		case OP_MUL_FI: c->f = a->f * (float)b->i; break;
		case OP_MUL_IF: c->f = (float)a->i * b->f; break;
		case OP_DIV_FI: c->f = a->f / (float)b->i; break;
		case OP_DIV_IF: c->f = (float)a->i / b->f; break;
		case OP_CONV_IF: c->f = (float)a->i; break;
		case OP_CONV_FI: c->i = f2i(a->f); break;
		case OP_BITAND_I: c->i = a->i & b->i; break;
		case OP_BITOR_I: c->i = a->i | b->i; break;
		case OP_BITXOR_I: c->i = a->i ^ b->i; break;
		case OP_LSHIFT_I: c->u = a->u << (b->i & 31); break;
		case OP_RSHIFT_I: c->i = a->i >> (b->i & 31); break;
		case OP_RSHIFT_U: c->u = a->u >> (b->i & 31); break;
		case OP_BITAND_IF: c->i = a->i & f2i(b->f); break;
		case OP_BITOR_IF: c->i = a->i | f2i(b->f); break;
		case OP_BITAND_FI: c->i = f2i(a->f) & b->i; break;
		case OP_BITOR_FI: c->i = f2i(a->f) | b->i; break;
		case OP_EQ_I: c->i = a->i == b->i; break;
		case OP_NE_I: c->i = a->i != b->i; break;
		case OP_LE_I: c->i = a->i <= b->i; break;
		case OP_GE_I: c->i = a->i >= b->i; break;
		case OP_LT_I: c->i = a->i < b->i; break;
		case OP_GT_I: c->i = a->i > b->i; break;
		case OP_LE_U: c->i = a->u <= b->u; break;
		case OP_LT_U: c->i = a->u < b->u; break;
		case OP_LE_IF: c->i = (float)a->i <= b->f; break;
		case OP_GE_IF: c->i = (float)a->i >= b->f; break;
		case OP_LT_IF: c->i = (float)a->i < b->f; break;
		case OP_GT_IF: c->i = (float)a->i > b->f; break;
		case OP_LE_FI: c->i = a->f <= (float)b->i; break;
		case OP_GE_FI: c->i = a->f >= (float)b->i; break;
		case OP_LT_FI: c->i = a->f < (float)b->i; break;
		case OP_GT_FI: c->i = a->f > (float)b->i; break;
		case OP_EQ_IF: c->i = (float)a->i == b->f; break;
		case OP_EQ_FI: c->i = a->f == (float)b->i; break;
		case OP_NE_IF: c->i = (float)a->i != b->f; break;
		case OP_NE_FI: c->i = a->f != (float)b->i; break;
		case OP_AND_I: c->i = a->i && b->i; break;
		case OP_OR_I: c->i = a->i || b->i; break;
		case OP_AND_IF: c->i = a->i && b->f; break;
		case OP_OR_IF: c->i = a->i || b->f; break;
		case OP_AND_FI: c->i = a->f && b->i; break;
		case OP_OR_FI: c->i = a->f || b->i; break;
		case OP_NOT_I: c->i = !a->i; break;

		/* --- 64-bit integers and doubles --- */
		case OP_ADD_I64: st64(c, (int64_t)((uint64_t)ld64(a) + (uint64_t)ld64(b))); break;
		case OP_SUB_I64: st64(c, (int64_t)((uint64_t)ld64(a) - (uint64_t)ld64(b))); break;
		case OP_MUL_I64: st64(c, (int64_t)((uint64_t)ld64(a) * (uint64_t)ld64(b))); break;
		case OP_DIV_I64: {
			int64_t x = ld64(a), y = ld64(b);
			st64(c, (!y || (x == INT64_MIN && y == -1)) ? 0 : x / y);
			break;
		}
		case OP_DIV_U64: {
			uint64_t x = (uint64_t)ld64(a), y = (uint64_t)ld64(b);
			st64(c, (int64_t)(y ? x / y : 0));
			break;
		}
		case OP_BITAND_I64: st64(c, ld64(a) & ld64(b)); break;
		case OP_BITOR_I64: st64(c, ld64(a) | ld64(b)); break;
		case OP_BITXOR_I64: st64(c, ld64(a) ^ ld64(b)); break;
		case OP_LSHIFT_I64I: st64(c, (int64_t)((uint64_t)ld64(a) << (b->i & 63))); break;
		case OP_RSHIFT_I64I: st64(c, ld64(a) >> (b->i & 63)); break;
		case OP_RSHIFT_U64I: st64(c, (int64_t)((uint64_t)ld64(a) >> (b->i & 63))); break;
		case OP_LE_I64: c->i = ld64(a) <= ld64(b); break;
		case OP_LT_I64: c->i = ld64(a) < ld64(b); break;
		case OP_EQ_I64: c->i = ld64(a) == ld64(b); break;
		case OP_NE_I64: c->i = ld64(a) != ld64(b); break;
		case OP_LE_U64: c->i = (uint64_t)ld64(a) <= (uint64_t)ld64(b); break;
		case OP_LT_U64: c->i = (uint64_t)ld64(a) < (uint64_t)ld64(b); break;
		case OP_CONV_UI64: st64(c, (int64_t)a->u); break;
		case OP_CONV_II64: st64(c, (int64_t)a->i); break;
		case OP_CONV_I64I: c->i = (int32_t)ld64(a); break;
		case OP_CONV_FD: std_(c, (double)a->f); break;
		case OP_CONV_DF: c->f = (float)ldd(a); break;
		case OP_CONV_I64F: c->f = (float)ld64(a); break;
		case OP_CONV_FI64: st64(c, (int64_t)a->f); break;
		case OP_CONV_I64D: std_(c, (double)ld64(a)); break;
		case OP_CONV_DI64: st64(c, (int64_t)ldd(a)); break;
		case OP_ADD_D: std_(c, ldd(a) + ldd(b)); break;
		case OP_SUB_D: std_(c, ldd(a) - ldd(b)); break;
		case OP_MUL_D: std_(c, ldd(a) * ldd(b)); break;
		case OP_DIV_D: std_(c, ldd(a) / ldd(b)); break;
		case OP_LE_D: c->i = ldd(a) <= ldd(b); break;
		case OP_LT_D: c->i = ldd(a) < ldd(b); break;
		case OP_EQ_D: c->i = ldd(a) == ldd(b); break;
		case OP_NE_D: c->i = ldd(a) != ldd(b); break;

		/* --- strings --- */
		case OP_ADD_SF: {
			/* pointer arithmetic on a progs string; engine strings cannot be offset */
			if ((uint32_t)a->i & QC_STRING_RUNTIME) {
				size_t len;
				char *p = QC_StringBuffer(vm, a->i, &len);
				int32_t ofs = f2i(b->f);
				c->i = (p && ofs >= 0 && (size_t)ofs <= len) ? QC_NewString(vm, p + ofs) : 0;
			} else {
				c->i = a->i + f2i(b->f);
			}
			break;
		}
		case OP_SUB_S: c->i = a->i - b->i; break;

		/* --- random --- */
		case OP_RAND0: c->f = QC_Random(vm); break;
		case OP_RAND1: c->f = QC_Random(vm) * a->f; break;
		case OP_RAND2: {
			float lo = a->f, hi = b->f;
			c->f = lo + QC_Random(vm) * (hi - lo);
			break;
		}
		case OP_RANDV0:
			c[0].f = QC_Random(vm); c[1].f = QC_Random(vm); c[2].f = QC_Random(vm);
			break;
		case OP_RANDV1:
			for (int i = 0; i < 3; i++) c[i].f = QC_Random(vm) * a[i].f;
			break;
		case OP_RANDV2:
			for (int i = 0; i < 3; i++) c[i].f = a[i].f + QC_Random(vm) * (b[i].f - a[i].f);
			break;

		/* --- control flow --- */
		case OP_IF: if (a->i) { s += JUMP(st->b); continue; } break;
		case OP_IFNOT: if (!a->i) { s += JUMP(st->b); continue; } break;
		case OP_IF_F: if (a->f) { s += JUMP(st->b); continue; } break;
		case OP_IFNOT_F: if (!a->f) { s += JUMP(st->b); continue; } break;
		case OP_IFS: if (!str_empty(vm, a->i)) { s += JUMP(st->b); continue; } break;
		case OP_IFNOTS: if (str_empty(vm, a->i)) { s += JUMP(st->b); continue; } break;
		case OP_GOTO: s += JUMP(st->a); continue;

		case OP_SWITCH_F: case OP_SWITCH_V: case OP_SWITCH_S: case OP_SWITCH_E:
		case OP_SWITCH_FNC: case OP_SWITCH_I:
			vm->switch_op = (int)st->op;
			vm->switch_value[0] = a[0].u;
			if (st->op == OP_SWITCH_V) {
				vm->switch_value[1] = a[1].u;
				vm->switch_value[2] = a[2].u;
			}
			s += JUMP(st->b);
			continue;
		case OP_CASE: {
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
			if (match) { s += JUMP(st->b); continue; }
			break;
		}
		case OP_CASERANGE: {
			qcval_t sv;
			bool match;
			sv.u = vm->switch_value[0];
			if (vm->switch_op == OP_SWITCH_F)
				match = sv.f >= a->f && sv.f <= b->f;
			else if (vm->switch_op == OP_SWITCH_I || vm->switch_op == OP_SWITCH_E)
				match = sv.i >= a->i && sv.i <= b->i;
			else {
				vm->xstatement = s;
				QC_RunError(vm, "case range used in a switch that cannot order its values");
			}
			if (match) { s += JUMP(st->c); continue; }
			break;
		}

		case OP_STATE: {
			int self = QC_GInt(vm, vm->gofs.self);
			QC_SetEFloat(vm, self, vm->fofs.frame, a->f);
			QC_SetEInt(vm, self, vm->fofs.think, b->i);
			QC_SetEFloat(vm, self, vm->fofs.nextthink, QC_GFloat(vm, vm->gofs.time) + 0.1f);
			break;
		}
		case OP_THINKTIME:
			QC_SetEFloat(vm, a->i, vm->fofs.nextthink, QC_GFloat(vm, vm->gofs.time) + b->f);
			break;

		case OP_CALL0: case OP_CALL1: case OP_CALL2: case OP_CALL3: case OP_CALL4:
		case OP_CALL5: case OP_CALL6: case OP_CALL7: case OP_CALL8:
		case OP_CALL1H: case OP_CALL2H: case OP_CALL3H: case OP_CALL4H:
		case OP_CALL5H: case OP_CALL6H: case OP_CALL7H: case OP_CALL8H: {
			int target = a->i, caller = vm->curprogs;
			int fnum = QC_FUNC_INDEX(target), pn = QC_FUNC_PROGS(target);
			qcfunc_t *f;
			if (st->op >= OP_CALL1H) {
				vm->argc = (int)(st->op - OP_CALL1H) + 1;
				/* the first two arguments travel in the b and c operands */
				memcpy(G + OFS_PARM(0), b, sizeof(qcval_t) * 3);
				if (vm->argc >= 2)
					memcpy(G + OFS_PARM(1), c, sizeof(qcval_t) * 3);
			} else {
				vm->argc = (int)(st->op - OP_CALL0);
			}
			if (pn >= vm->numprogs || fnum <= 0 || fnum >= vm->progs[pn].numfunctions) {
				vm->xstatement = s;
				QC_RunError(vm, "call to %s function (%d)", target ? "invalid" : "NULL", target);
			}
			vm->xstatement = s;
			if (pn != caller)
				QC_SwitchProgs(vm, pn);
			f = &vm->functions[fnum];
			if (f->builtin) {
				vm->extra->calling_builtin = fnum;
				f->builtin(vm);
				if (vm->curprogs != caller)
					QC_SwitchProgs(vm, caller);
				G = vm->globals;
				break;
			}
			enter_function(vm, fnum, s + 1, caller);
			G = vm->globals;
			s = f->d.first_statement;
			continue;
		}

		case OP_RETURN:
			G[OFS_RETURN] = a[0];
			G[OFS_RETURN + 1] = a[1];
			G[OFS_RETURN + 2] = a[2];
			/* fall through */
		case OP_DONE:
			s = leave_function(vm);
			G = vm->globals;
			if (vm->depth <= entrydepth)
				return;
			continue;

		default:
			vm->xstatement = s;
			QC_RunError(vm, "unsupported opcode %u", st->op);
		}
		s++;
	}
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
