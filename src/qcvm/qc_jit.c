/* Bulwark engine - QuakeC to x86 machine code (see qc_jit.h).
 * ISC licence, see LICENSE.
 *
 * One template per statement, working on memory: no register allocation.
 * The same encodings serve 32-bit code (the Xbox) and 64-bit code (Linux
 * desktops, where the QC core dumps can be compared with the
 * interpreter's): only the prologue, the epilogue and calls differ.
 * Registers that live through a function:
 *   ebx  the running progs' globals
 *   ebp  entity 0's fields
 *   esi  the VM (r13 in 64-bit code, as esi is not kept across calls there)
 * Floats are SSE scalars, as the interpreter's are on both targets (the
 * Xbox build uses -mfpmath=sse), so every result is rounded as it is in C,
 * one operation at a time and in the same order. */
#include "qc_internal.h"
#include "qc_jit.h"
#include "../common/cmd.h"

#ifdef QC_JIT

#include <stddef.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

cvar_t *qc_jit_cvar;
static cvar_t *jit_map;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
#define RG RBX
#define RE RBP
#ifdef __x86_64__
#define RV 13
#else
#define RV RSI
#endif

/* condition codes */
enum { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_S = 8, CC_NS = 9, CC_P = 10, CC_NP = 11,
       CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };

/* code memory: chunks, each filled from the start, kept until the VM goes */
#define CHUNK_BYTES (256 * 1024)
typedef struct qc_jitchunk_s {
	struct qc_jitchunk_s *next;
	uint8_t *base;
	void *mem; /* as allocated */
	size_t size, used;
} qc_jitchunk_t;

typedef struct qc_jitmem_s {
	qc_jitchunk_t *chunks;
	size_t total; /* bytes of code memory taken */
	int functions; /* compiled */
	size_t code; /* bytes of code in them */
} qc_jitmem_t;

/* size is whole pages; the code has its pages to itself */
static uint8_t *code_alloc(size_t size, void **mem)
{
#ifdef __linux__
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	*mem = p;
	return p == MAP_FAILED ? NULL : p;
#else
	/* the Pentium III has no no-execute bit: any memory runs. Data written
	 * on a page of code makes an emulator throw that page's translations
	 * away, so the code starts on a page of its own */
	uint8_t *p = malloc(size + 4095);
	*mem = p;
	return p ? (uint8_t *)(((uintptr_t)p + 4095) & ~(uintptr_t)4095) : NULL;
#endif
}

static void code_free(void *p, size_t size)
{
#ifdef __linux__
	munmap(p, size);
#else
	(void)size;
	free(p);
#endif
}

void QC_JitFree(qcvm_t *vm)
{
	qc_jitmem_t *m = vm->jitmem;
	if (!m)
		return;
	while (m->chunks) {
		qc_jitchunk_t *c = m->chunks;
		m->chunks = c->next;
		code_free(c->mem, c->size);
		Mem_Free(c);
	}
	Mem_Free(m);
	vm->jitmem = NULL;
}

/* --- emitter ----------------------------------------------------------- */

typedef struct {
	uint32_t at; /* where the rel32 is */
	int to; /* statement index, or a stub */
} patch_t;

typedef struct {
	int s; /* statement the stub is for */
	int kind; /* STUB_STEP: run s in the interpreter and go on there; STUB_BAIL: go on at s */
} stub_t;
enum { STUB_STEP, STUB_BAIL };

typedef struct {
	uint32_t at; /* where a call's rel32 is */
	const void *fn;
} extcall_t;

typedef struct {
	uint8_t *base, *p, *limit; /* a scratch buffer: code is moved to its place when done */
	size_t cap;
	bool bad;
	extcall_t *ext;
	int next, extcap;
	qcvm_t *vm;
	const qcstatement_t *stmts;
	int first, end; /* the statements being compiled */
	uint32_t *label; /* offset of each statement's code */
	patch_t *patches;
	int npatches, patchcap;
	stub_t *stubs;
	int nstubs, stubcap;
} emit_t;

static void b1(emit_t *e, int x) { *e->p++ = (uint8_t)x; }
static void u32(emit_t *e, uint32_t v)
{
	memcpy(e->p, &v, 4);
	e->p += 4;
}

static void rex(emit_t *e, int w, int reg, int idx, int base)
{
#ifdef __x86_64__
	int r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((idx & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
	if (r != 0x40)
		b1(e, r);
#else
	(void)e, (void)w, (void)reg, (void)idx, (void)base;
#endif
}

/* modrm, sib and displacement for [base + idx * scale + disp]; idx < 0 for none */
static void modrm_mem(emit_t *e, int reg, int base, int idx, int scale, int32_t disp)
{
	int mod = (disp == 0 && (base & 7) != RBP) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
	if (idx < 0 && (base & 7) != RSP) {
		b1(e, (mod << 6) | ((reg & 7) << 3) | (base & 7));
	} else {
		int ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
		b1(e, (mod << 6) | ((reg & 7) << 3) | 4);
		b1(e, (ss << 6) | (((idx < 0 ? RSP : idx) & 7) << 3) | (base & 7));
	}
	if (mod == 1)
		b1(e, (uint8_t)(int8_t)disp);
	else if (mod == 2)
		u32(e, (uint32_t)disp);
}

static void opcode(emit_t *e, int op)
{
	if (op > 0xffff)
		b1(e, op >> 16);
	if (op > 0xff)
		b1(e, (op >> 8) & 0xff);
	b1(e, op & 0xff);
}

/* an instruction with a memory operand; pfx is a mandatory prefix (F3, 66) or 0 */
static void opm(emit_t *e, int pfx, int w, int op, int reg, int base, int idx, int scale, int32_t disp)
{
	if (pfx)
		b1(e, pfx);
	rex(e, w, reg, idx < 0 ? 0 : idx, base);
	opcode(e, op);
	modrm_mem(e, reg, base, idx, scale, disp);
}

/* an instruction on two registers (reg, rm) */
static void opr(emit_t *e, int pfx, int w, int op, int reg, int rm)
{
	if (pfx)
		b1(e, pfx);
	rex(e, w, reg, 0, rm);
	opcode(e, op);
	b1(e, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

#define GD(o) ((int32_t)(o) * 4) /* displacement of global o */

/* 32-bit moves and arithmetic on globals */
static void ldg(emit_t *e, int r, int o) { opm(e, 0, 0, 0x8b, r, RG, -1, 0, GD(o)); }
static void stg(emit_t *e, int o, int r) { opm(e, 0, 0, 0x89, r, RG, -1, 0, GD(o)); }
/* add 03, or 0b, and 23, sub 2b, xor 33, cmp 3b, imul 0faf: r op= global */
static void alug(emit_t *e, int op, int r, int o) { opm(e, 0, 0, op, r, RG, -1, 0, GD(o)); }
static void alurr(emit_t *e, int op, int dst, int src) { opr(e, 0, 0, op, dst, src); }
/* r op= imm: add /0, or /1, and /4, sub /5, xor /6, cmp /7 */
static void alui(emit_t *e, int ext, int r, int32_t imm)
{
	if (imm >= -128 && imm <= 127) {
		opr(e, 0, 0, 0x83, ext, r);
		b1(e, (uint8_t)(int8_t)imm);
	} else {
		opr(e, 0, 0, 0x81, ext, r);
		u32(e, (uint32_t)imm);
	}
}
static void movri(emit_t *e, int r, uint32_t imm)
{
	rex(e, 0, 0, 0, r);
	b1(e, 0xb8 + (r & 7));
	u32(e, imm);
}
/* test dword [global], imm */
static void testgi(emit_t *e, int o, uint32_t imm)
{
	opm(e, 0, 0, 0xf7, 0, RG, -1, 0, GD(o));
	u32(e, imm);
}
static void setcc(emit_t *e, int cc, int r8) { opr(e, 0, 0, 0x0f90 | cc, 0, r8); }
static void movzx8(emit_t *e, int r, int r8) { opr(e, 0, 0, 0x0fb6, r, r8); }

/* SSE scalars */
static void xld(emit_t *e, int x, int o) { opm(e, 0xf3, 0, 0x0f10, x, RG, -1, 0, GD(o)); }
static void xst(emit_t *e, int o, int x) { opm(e, 0xf3, 0, 0x0f11, x, RG, -1, 0, GD(o)); }
/* add 58, mul 59, sub 5c, div 5e: x op= global */
static void xopg(emit_t *e, int op, int x, int o) { opm(e, 0xf3, 0, 0x0f00 | op, x, RG, -1, 0, GD(o)); }
static void xoprr(emit_t *e, int op, int x, int y) { opr(e, 0xf3, 0, 0x0f00 | op, x, y); }
static void xmov(emit_t *e, int x, int y) { opr(e, 0xf3, 0, 0x0f10, x, y); }
static void xcvtig(emit_t *e, int x, int o) { opm(e, 0xf3, 0, 0x0f2a, x, RG, -1, 0, GD(o)); }
static void xcvtir(emit_t *e, int x, int r) { opr(e, 0xf3, 0, 0x0f2a, x, r); }
static void xucomrr(emit_t *e, int x, int y) { opr(e, 0, 0, 0x0f2e, x, y); }
static void xucomg(emit_t *e, int x, int o) { opm(e, 0, 0, 0x0f2e, x, RG, -1, 0, GD(o)); }

/* the VM's own fields */
#define VMD(field) ((int32_t)offsetof(qcvm_t, field))
static void ldvm(emit_t *e, int r, int32_t d) { opm(e, 0, 0, 0x8b, r, RV, -1, 0, d); }
static void stvm(emit_t *e, int32_t d, int r) { opm(e, 0, 0, 0x89, r, RV, -1, 0, d); }

/* --- jumps, stubs and calls --- */

static uint32_t here(emit_t *e) { return (uint32_t)(e->p - e->base); }

static void add_patch(emit_t *e, int to)
{
	if (e->npatches == e->patchcap) {
		e->patchcap = e->patchcap ? e->patchcap * 2 : 256;
		e->patches = Mem_Realloc(e->patches, sizeof(patch_t) * (size_t)e->patchcap);
	}
	e->patches[e->npatches].at = here(e);
	e->patches[e->npatches++].to = to;
	u32(e, 0);
}

/* patch targets past the statements are stubs and the epilogue */
#define TO_EPILOGUE (-1)
#define TO_STEPCOMMON (-2)
#define TO_STUB(k) (-3 - (k))

static int new_stub(emit_t *e, int s, int kind)
{
	if (e->nstubs == e->stubcap) {
		e->stubcap = e->stubcap ? e->stubcap * 2 : 64;
		e->stubs = Mem_Realloc(e->stubs, sizeof(stub_t) * (size_t)e->stubcap);
	}
	/* the checks of one statement share its stub */
	for (int i = e->nstubs - 1; i >= 0 && e->stubs[i].s == s; i--)
		if (e->stubs[i].kind == kind)
			return TO_STUB(i);
	e->stubs[e->nstubs].s = s;
	e->stubs[e->nstubs].kind = kind;
	return TO_STUB(e->nstubs++);
}

static void jmp_to(emit_t *e, int to)
{
	b1(e, 0xe9);
	add_patch(e, to);
}
static void jcc_to(emit_t *e, int cc, int to)
{
	b1(e, 0x0f);
	b1(e, 0x80 | cc);
	add_patch(e, to);
}
/* a short forward jump within one template; returns where to fix it */
static uint8_t *jcc8(emit_t *e, int cc)
{
	b1(e, 0x70 | cc);
	b1(e, 0);
	return e->p - 1;
}
static uint8_t *jmp8(emit_t *e)
{
	b1(e, 0xeb);
	b1(e, 0);
	return e->p - 1;
}
#ifndef __x86_64__
/* call rel32 to fn; fixed once the code is where it runs */
static void call_rel(emit_t *e, const void *fn)
{
	if (e->next == e->extcap) {
		e->extcap = e->extcap ? e->extcap * 2 : 64;
		e->ext = Mem_Realloc(e->ext, sizeof(extcall_t) * (size_t)e->extcap);
	}
	b1(e, 0xe8);
	e->ext[e->next].at = (uint32_t)(e->p - e->base);
	e->ext[e->next++].fn = fn;
	u32(e, 0);
}
#endif

static void fix8(emit_t *e, uint8_t *at)
{
	ptrdiff_t d = e->p - (at + 1);
	if (d > 127)
		e->bad = true; /* a template outgrew its short jumps: not compiled */
	*at = (uint8_t)d;
}

/* fail: rerun statement s in the interpreter, which raises its error */
static void jcc_fail(emit_t *e, int cc, int s) { jcc_to(e, cc, new_stub(e, s, STUB_STEP)); }

/* 32-bit Linux keeps the stack 16-byte aligned at calls (its SSE code may
 * rely on it); the Xbox's 4-byte alignment needs no padding */
#if defined(__i386__) && defined(__linux__)
#define STACK_PAD_BYTES 8
#define STACK_PAD(e) alui(e, 5, RSP, 8)
#else
#define STACK_PAD_BYTES 0
#define STACK_PAD(e) ((void)0)
#endif

/* calls helper(vm, s); the result is in eax */
static void call_helper(emit_t *e, void *fn, int s)
{
#ifdef __x86_64__
	opr(e, 0, 1, 0x89, RV, RDI); /* mov rdi, r13 */
	movri(e, RSI, (uint32_t)s);
	b1(e, 0x48);
	b1(e, 0xb8); /* mov rax, imm64 */
	{
		uint64_t a = (uint64_t)(uintptr_t)fn;
		memcpy(e->p, &a, 8);
		e->p += 8;
	}
	b1(e, 0xff);
	b1(e, 0xd0); /* call rax */
#else
	STACK_PAD(e);
	b1(e, 0x68);
	u32(e, (uint32_t)s); /* push s */
	b1(e, 0x50 + RV); /* push esi */
	call_rel(e, fn);
	alui(e, 0, RSP, 8 + STACK_PAD_BYTES);
#endif
}

/* calls helper(vm, eax) */
static void call_helper_eax(emit_t *e, void *fn)
{
#ifdef __x86_64__
	alurr(e, 0x8b, RSI, RAX); /* mov esi, eax */
	opr(e, 0, 1, 0x89, RV, RDI);
	b1(e, 0x48);
	b1(e, 0xb8);
	{
		uint64_t a = (uint64_t)(uintptr_t)fn;
		memcpy(e->p, &a, 8);
		e->p += 8;
	}
	b1(e, 0xff);
	b1(e, 0xd0);
#else
	STACK_PAD(e);
	b1(e, 0x50); /* push eax */
	b1(e, 0x50 + RV);
	call_rel(e, fn);
	alui(e, 0, RSP, 8 + STACK_PAD_BYTES);
#endif
}

/* --- the parts of statements --- */

static bool in_body(emit_t *e, int t) { return t >= e->first && t < e->end; }

/* a jump from s to t, taken; backward ones count against the budget */
static void jump(emit_t *e, int s, int t)
{
	if (!in_body(e, t)) {
		/* somewhere else: the interpreter checks and takes it */
		jmp_to(e, new_stub(e, s, STUB_STEP));
		return;
	}
	if (t <= s) {
		opm(e, 0, 0, 0x81, 5, RV, -1, 0, (int32_t)offsetof(qcvm_t, jit_budget));
		u32(e, (uint32_t)(1 + s - t));
		jcc_to(e, CC_S, new_stub(e, s, STUB_BAIL));
	}
	jmp_to(e, t);
}

/* jumps from s to t when cc holds */
static void branch(emit_t *e, int cc, int s, int t)
{
	if (in_body(e, t) && t > s) {
		jcc_to(e, cc, t);
		return;
	}
	{
		uint8_t *skip = jcc8(e, cc ^ 1);
		jump(e, s, t);
		fix8(e, skip);
	}
}

/* f2i(xmm0) into eax: NaN is 0, out of range clamps */
static void f2i_xmm0(emit_t *e)
{
	uint8_t *ok, *nan, *neg, *done1, *done2;
	opr(e, 0xf3, 0, 0x0f2c, RAX, 0); /* cvttss2si eax, xmm0 */
	alui(e, 7, RAX, (int32_t)0x80000000u);
	ok = jcc8(e, CC_NE);
	xucomrr(e, 0, 0);
	nan = jcc8(e, CC_P);
	opr(e, 0, 0, 0x0f57, 1, 1); /* xorps xmm1, xmm1 */
	xucomrr(e, 0, 1);
	neg = jcc8(e, CC_BE);
	movri(e, RAX, 0x7fffffff);
	done1 = jmp8(e);
	fix8(e, nan);
	alurr(e, 0x33, RAX, RAX);
	done2 = jmp8(e);
	fix8(e, neg);
	movri(e, RAX, 0x80000000u);
	fix8(e, ok);
	fix8(e, done1);
	fix8(e, done2);
}

static void f2i_g(emit_t *e, int r, int o)
{
	xld(e, 0, o);
	f2i_xmm0(e);
	if (r != RAX)
		alurr(e, 0x8b, r, RAX);
}

/* eax (0 or 1) as a float into global o */
static void st_fbool(emit_t *e, int o)
{
	opr(e, 0, 0, 0xf7, 3, RAX); /* neg eax */
	alui(e, 4, RAX, 0x3f800000);
	stg(e, o, RAX);
}

/* cc as 0 or 1 in eax */
static void cc_bool(emit_t *e, int cc)
{
	setcc(e, cc, RAX);
	movzx8(e, RAX, RAX);
}

/* feq of globals a and b, as 0 or 1 in eax */
static void feq(emit_t *e, int a, int b)
{
	uint8_t *ne, *done;
	ldg(e, RAX, a);
	ldg(e, RCX, b);
	alurr(e, 0x3b, RAX, RCX);
	ne = jcc8(e, CC_NE);
	alui(e, 4, RAX, 0x7fffffff);
	alui(e, 7, RAX, 0x7f800000);
	cc_bool(e, CC_BE);
	done = jmp8(e);
	fix8(e, ne);
	alurr(e, 0x0b, RAX, RCX);
	opr(e, 0, 0, 0xf7, 0, RAX); /* test eax, imm */
	u32(e, 0x7fffffff);
	cc_bool(e, CC_E);
	fix8(e, done);
}

/* ftrue of global o into eax as 0 or 1 */
static void ftrue(emit_t *e, int o)
{
	testgi(e, o, 0x7fffffff);
	cc_bool(e, CC_NE);
}

/* eax = e * words + f for entity global a and field global b, checked for
 * an access of n words */
static void field_index(emit_t *e, int s, int a, int b, int n)
{
	qcvm_t *vm = e->vm;
	ldg(e, RAX, a);
	ldg(e, RCX, b);
	alui(e, 7, RAX, vm->max_edicts);
	jcc_fail(e, CC_AE, s);
	alui(e, 7, RCX, vm->edict_words - n);
	jcc_fail(e, CC_A, s);
	opr(e, 0, 0, 0x69, RAX, RAX); /* imul eax, eax, words */
	u32(e, (uint32_t)vm->edict_words);
	alurr(e, 0x03, RAX, RCX);
}

/* eax = arena address in global o plus global k words (k < 0: none),
 * checked as mem_ptr checks it for n words; rdx = vm->mem */
static void pointer(emit_t *e, int s, int o, int k, int n)
{
	ldg(e, RAX, o);
	if (k >= 0) {
		ldg(e, RCX, k);
		opr(e, 0, 0, 0xc1, 4, RCX); /* shl ecx, 2 */
		b1(e, 2);
		alurr(e, 0x03, RAX, RCX);
	}
	b1(e, 0xa8); /* test al, 3 */
	b1(e, 3);
	jcc_fail(e, CC_NE, s);
	opr(e, 0, 0, 0x85, RAX, RAX); /* test eax, eax */
	jcc_fail(e, CC_E, s);
	alui(e, 7, RAX, (int32_t)(e->vm->memsize - (size_t)n * 4));
	jcc_fail(e, CC_A, s);
	opm(e, 0, 1, 0x8b, RDX, RV, -1, 0, (int32_t)offsetof(qcvm_t, mem)); /* mov rdx, vm->mem */
}

/* word i at [rdx + rax] */
static void ldp(emit_t *e, int r, int i) { opm(e, 0, 0, 0x8b, r, RDX, RAX, 1, i * 4); }
static void stp(emit_t *e, int i, int r) { opm(e, 0, 0, 0x89, r, RDX, RAX, 1, i * 4); }
/* field word i at [ebp + eax * 4] */
static void ldf(emit_t *e, int r, int i) { opm(e, 0, 0, 0x8b, r, RE, RAX, 4, i * 4); }
static void stf(emit_t *e, int i, int r) { opm(e, 0, 0, 0x89, r, RE, RAX, 4, i * 4); }

/* the generic case: the interpreter runs statement s, then native code
 * goes on wherever it went, if that is a statement compiled here */
static void step_with(emit_t *e, void *fn, int s, const int *targets, int ntargets)
{
	call_helper(e, fn, s);
	for (int i = 0; i < ntargets; i++) {
		int t = targets[i];
		bool dup = false;
		for (int j = 0; j < i; j++)
			dup |= targets[j] == t;
		if (dup || !in_body(e, t))
			continue;
		alui(e, 7, RAX, t);
		jcc_to(e, CC_E, t);
	}
	jmp_to(e, TO_EPILOGUE);
}

static void step(emit_t *e, int s, const int *targets, int ntargets)
{
	step_with(e, (void *)QC_JitStep, s, targets, ntargets);
}

#define JUMPOP(x) ((int)(int16_t)(x))


/* --- calls --- */

/* a rel32 jump within one template, fixed with fix32 */
static uint32_t jcc32(emit_t *e, int cc)
{
	b1(e, 0x0f);
	b1(e, 0x80 | cc);
	u32(e, 0);
	return here(e) - 4;
}
static uint32_t jmp32(emit_t *e)
{
	b1(e, 0xe9);
	u32(e, 0);
	return here(e) - 4;
}
static void fix32(emit_t *e, uint32_t at)
{
	uint32_t rel = here(e) - (at + 4);
	memcpy(e->base + at, &rel, 4);
}

/* r = a pointer the code is built with */
static void movptr(emit_t *e, int r, const void *p)
{
#ifdef __x86_64__
	rex(e, 1, 0, 0, r);
	b1(e, 0xb8 + (r & 7));
	{
		uint64_t v = (uint64_t)(uintptr_t)p;
		memcpy(e->p, &v, 8);
		e->p += 8;
	}
#else
	movri(e, r, (uint32_t)(uintptr_t)p);
#endif
}

/* cmp byte [p], 0 for a variable of the engine's; uses rcx */
static void cmp_byte0(emit_t *e, const void *p)
{
	movptr(e, RCX, p);
	opm(e, 0, 0, 0x80, 7, RCX, -1, 0, 0);
	b1(e, 0);
}

/* calls fn(vm) */
static void call_vm(emit_t *e, const void *fn)
{
#ifdef __x86_64__
	opr(e, 0, 1, 0x89, RV, RDI);
	movptr(e, RAX, fn);
	b1(e, 0xff);
	b1(e, 0xd0);
#else
#if defined(__linux__)
	alui(e, 5, RSP, 12);
#endif
	b1(e, 0x50 + RV);
	call_rel(e, fn);
#if defined(__linux__)
	alui(e, 0, RSP, 16);
#else
	alui(e, 0, RSP, 4);
#endif
#endif
}

/* what call_begin does before the call itself: argc, the arguments that
 * travel in b and c, and the statement for error messages */
static void call_args(emit_t *e, int s, int argc, bool h, int b, int c)
{
	opm(e, 0, 0, 0xc7, 0, RV, -1, 0, VMD(argc));
	u32(e, (uint32_t)argc);
	if (h) {
		/* copy3: all three read before any is written */
		for (int k = 0; k < (argc >= 2 ? 2 : 1); k++) {
			int from = k ? c : b, to = OFS_PARM(k);
			ldg(e, RAX, from);
			ldg(e, RCX, from + 1);
			ldg(e, RDX, from + 2);
			stg(e, to, RAX);
			stg(e, to + 1, RCX);
			stg(e, to + 2, RDX);
		}
	}
	opm(e, 0, 0, 0xc7, 0, RV, -1, 0, VMD(xstatement));
	u32(e, (uint32_t)s);
}

/* hooks that make the rest of the function the interpreter's: bail at t */
static void check_hooks(emit_t *e, int t)
{
	opm(e, 0, 0, 0x80, 7, RV, -1, 0, VMD(debug_hooks));
	b1(e, 0);
	jcc_to(e, CC_NE, new_stub(e, t, STUB_BAIL));
	cmp_byte0(e, &qc_profiling);
	jcc_to(e, CC_NE, new_stub(e, t, STUB_BAIL));
}

/* copies n words from [from + i] to [to + i], 1 word at a time */
static void copy_mem(emit_t *e, int tobase, int toidx, int32_t todisp, int frombase, int fromidx, int32_t fromdisp, int n)
{
	for (int i = 0; i < n; i++) {
		opm(e, 0, 0, 0x8b, RAX, frombase, fromidx, fromidx < 0 ? 0 : 4, fromdisp + i * 4);
		opm(e, 0, 0, 0x89, RAX, tobase, toidx, toidx < 0 ? 0 : 4, todisp + i * 4);
	}
}

/* the most locals a function entered from native code saves inline */
#define CALL_MAX_LOCALS 96

_Static_assert(sizeof(qcframe_t) == 16, "a frame is found by shifting depth left 4");

/* A call statement. When the function global still names the builtin or
 * field getter it named at compile time, the builtin is called directly or
 * the getter done in place. At other sites a compiled QC function of this
 * progs is entered by its call entry, whatever the global holds when the
 * call runs. Anything else goes through QC_JitCall. */
static void emit_call(emit_t *e, int s, const qcstatement_t *st, unsigned op)
{
	qcvm_t *vm = e->vm;
	const int a = st->a, b = st->b, c = st->c;
	const bool h = op >= OP_CALL1H;
	const int argc = h ? (int)(op - OP_CALL1H) + 1 : (int)(op - OP_CALL0);
	const int32_t fv = vm->globals[a].i;
	const int fnum = QC_FUNC_INDEX(fv);
	qcfunc_t *cf = NULL;
	uint32_t slow[16], done[4];
	int nslow = 0, ndone = 0;

	if (a >= RESERVED_OFS && QC_FUNC_PROGS(fv) == vm->curprogs && fnum > 0 && fnum < vm->numfunctions)
		cf = &vm->functions[fnum];

	if (cf && cf->builtin) {
		uint32_t post;
		opm(e, 0, 0, 0x81, 7, RG, -1, 0, GD(a));
		u32(e, (uint32_t)fv);
		slow[nslow++] = jcc32(e, CC_NE);
		call_args(e, s, argc, h, b, c);
		opm(e, 0, 1, 0x8b, RAX, RV, -1, 0, VMD(extra));
		opm(e, 0, 0, 0xc7, 0, RAX, -1, 0, (int32_t)offsetof(struct qc_extra_s, calling_builtin));
		u32(e, (uint32_t)fnum);
		call_vm(e, (const void *)cf->builtin);
		/* back in this progs, with the autocvars up to date */
		opm(e, 0, 0, 0x81, 7, RV, -1, 0, VMD(curprogs));
		u32(e, (uint32_t)vm->curprogs);
		{
			uint32_t p1 = jcc32(e, CC_NE);
			ldvm(e, RAX, VMD(autocvar_gen));
			movptr(e, RCX, &cvar_generation);
			opm(e, 0, 0, 0x3b, RAX, RCX, -1, 0, 0);
			post = jcc32(e, CC_E);
			fix32(e, p1);
			call_helper(e, (void *)QC_JitBuiltinDone, vm->curprogs);
			fix32(e, post);
		}
		check_hooks(e, s + 1);
		done[ndone++] = jmp32(e);
	} else if (cf && cf->getter && cf->d.first_statement > 0) {
		const qcstatement_t *gs = vm->statements + cf->d.first_statement;
		const int ge = gs->a, gf = gs->b, gt = gs->c, w = cf->getter;
		const int ps = cf->d.parm_start, locals = cf->d.locals;
		if (ge >= RESERVED_OFS && gf >= RESERVED_OFS) {
			opm(e, 0, 0, 0x81, 7, RG, -1, 0, GD(a));
			u32(e, (uint32_t)fv);
			slow[nslow++] = jcc32(e, CC_NE);
			/* a trace or a full stack takes the frame path */
			cmp_byte0(e, qc_tracefilter);
			slow[nslow++] = jcc32(e, CC_NE);
			opm(e, 0, 0, 0x81, 7, RV, -1, 0, VMD(depth));
			u32(e, QC_MAX_DEPTH);
			slow[nslow++] = jcc32(e, CC_GE);
			ldg(e, RAX, ge);
			ldg(e, RCX, gf);
			alui(e, 7, RAX, vm->max_edicts);
			slow[nslow++] = jcc32(e, CC_AE);
			alui(e, 7, RCX, vm->edict_words - w);
			slow[nslow++] = jcc32(e, CC_A);
			call_args(e, s, argc, h, b, c);
			ldg(e, RAX, ge);
			ldg(e, RCX, gf);
			opr(e, 0, 0, 0x69, RAX, RAX);
			u32(e, (uint32_t)vm->edict_words);
			alurr(e, 0x03, RAX, RCX);
			/* the getter's own locals are saved around it, so its
			 * temporary keeps its value if it is one of them */
			for (int i = 0; i < 3; i++) {
				bool mine = gt + i >= ps && gt + i < ps + locals;
				if (i < w) {
					ldf(e, RCX, i);
					if (!mine)
						stg(e, gt + i, RCX);
				} else {
					ldg(e, RCX, gt + i);
				}
				stg(e, OFS_RETURN + i, RCX);
			}
			done[ndone++] = jmp32(e);
		}
	} else if (!cf || !cf->getter) {
		/* the function the global holds when the call runs, so method
		 * and other indirect calls take this path too. Its entry makes
		 * the frame (see call_entry): 0 is done, 1 is hooks on (the rest
		 * of this function is the interpreter's), 2 is not entered */
		ldg(e, RAX, a);
		alurr(e, 0x8b, RDX, RAX);
		opr(e, 0, 0, 0xc1, 5, RDX); /* shr edx, progs shift */
		b1(e, QC_PROGS_SHIFT);
		alui(e, 4, RDX, 0x7f);
		alui(e, 7, RDX, vm->curprogs);
		slow[nslow++] = jcc32(e, CC_NE);
		alui(e, 4, RAX, QC_INDEX_MASK);
		slow[nslow++] = jcc32(e, CC_E);
		opm(e, 0, 0, 0x3b, RAX, RV, -1, 0, VMD(numfunctions));
		slow[nslow++] = jcc32(e, CC_AE);
		opr(e, 0, 0, 0x69, RAX, RAX); /* imul eax, eax, size */
		u32(e, (uint32_t)sizeof(qcfunc_t));
		opm(e, 0, 1, 0x8b, RCX, RV, -1, 0, VMD(functions));
		opm(e, 0, 1, 0x8b, RCX, RCX, RAX, 1, (int32_t)offsetof(qcfunc_t, jit_call));
		opr(e, 0, 1, 0x85, RCX, RCX);
		slow[nslow++] = jcc32(e, CC_E);
		/* the arguments only once nothing can take the slow path, which
		 * copies them itself: a copy run twice differs when b or c
		 * overlaps the parameters. call_args uses eax, ecx and edx. */
		b1(e, 0x51); /* push rcx */
		call_args(e, s, argc, h, b, c);
		b1(e, 0x59); /* pop rcx */
		movri(e, RAX, (uint32_t)(s + 1));
		b1(e, 0xff);
		b1(e, 0xd1); /* call rcx */
		opr(e, 0, 0, 0x85, RAX, RAX);
		done[ndone++] = jcc32(e, CC_E);
		alui(e, 7, RAX, 1);
		jcc_to(e, CC_E, new_stub(e, s + 1, STUB_BAIL));
	}

	for (int i = 0; i < nslow; i++)
		fix32(e, slow[i]);
	call_helper(e, (void *)QC_JitCall, s);
	opr(e, 0, 0, 0x85, RAX, RAX);
	jcc_to(e, CC_NE, new_stub(e, s + 1, STUB_BAIL));
	for (int i = 0; i < ndone; i++)
		fix32(e, done[i]);
}

/* one statement; false if it has no template here */
static bool statement(emit_t *e, int s)
{
	const qcstatement_t *st = e->stmts + s;
	const int a = st->a, b = st->b, c = st->c;
	qcvm_t *vm = e->vm;
	/* the loader's fused opcodes are compiled as their plain statements */
	const unsigned op = QC_PlainOp(st->op);

	switch (op) {
	/* --- float arithmetic --- */
	case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F: {
		int sop = op == OP_ADD_F ? 0x58 : op == OP_SUB_F ? 0x5c : op == OP_MUL_F ? 0x59 : 0x5e;
		xld(e, 0, a);
		xopg(e, sop, 0, b);
		xst(e, c, 0);
		return true;
	}
	case OP_ADD_V: case OP_SUB_V:
		for (int i = 0; i < 3; i++) {
			xld(e, 0, a + i);
			xopg(e, op == OP_ADD_V ? 0x58 : 0x5c, 0, b + i);
			xst(e, c + i, 0);
		}
		return true;
	case OP_MUL_V:
		xld(e, 0, a);
		xopg(e, 0x59, 0, b);
		xld(e, 1, a + 1);
		xopg(e, 0x59, 1, b + 1);
		xoprr(e, 0x58, 0, 1);
		xld(e, 1, a + 2);
		xopg(e, 0x59, 1, b + 2);
		xoprr(e, 0x58, 0, 1);
		xst(e, c, 0);
		return true;
	case OP_MUL_FV: case OP_MUL_IV:
		/* f * b[i], f read first */
		if (op == OP_MUL_FV)
			xld(e, 1, a);
		else
			xcvtig(e, 1, a);
		for (int i = 0; i < 3; i++) {
			xmov(e, 0, 1);
			xopg(e, 0x59, 0, b + i);
			xst(e, c + i, 0);
		}
		return true;
	case OP_MUL_VF: case OP_MUL_VI: case OP_DIV_VF:
		if (op == OP_MUL_VI)
			xcvtig(e, 1, b);
		else
			xld(e, 1, b);
		for (int i = 0; i < 3; i++) {
			xld(e, 0, a + i);
			xoprr(e, op == OP_DIV_VF ? 0x5e : 0x59, 0, 1);
			xst(e, c + i, 0);
		}
		return true;
	case OP_BITAND: case OP_BITOR:
		f2i_g(e, RDX, a);
		f2i_g(e, RAX, b);
		alurr(e, op == OP_BITAND ? 0x23 : 0x0b, RAX, RDX);
		xcvtir(e, 0, RAX);
		xst(e, c, 0);
		return true;
	case OP_AND_F: case OP_OR_F: {
		/* both sides are plain reads, so testing both is the same as && */
		ftrue(e, a);
		alurr(e, 0x8b, RDX, RAX);
		ftrue(e, b);
		alurr(e, op == OP_AND_F ? 0x23 : 0x0b, RAX, RDX);
		st_fbool(e, c);
		return true;
	}

	/* --- comparisons --- */
	case OP_EQ_F: case OP_NE_F:
		feq(e, a, b);
		if (op == OP_NE_F)
			alui(e, 6, RAX, 1);
		st_fbool(e, c);
		return true;
	case OP_LE_F: case OP_LT_F:
		/* a <= b as b >= a, so unordered is false */
		xld(e, 0, b);
		xucomg(e, 0, a);
		cc_bool(e, op == OP_LE_F ? CC_AE : CC_A);
		st_fbool(e, c);
		return true;
	case OP_GE_F: case OP_GT_F:
		xld(e, 0, a);
		xucomg(e, 0, b);
		cc_bool(e, op == OP_GE_F ? CC_AE : CC_A);
		st_fbool(e, c);
		return true;
	case OP_EQ_V: case OP_NE_V: {
		/* == on each component; unordered is not equal */
		uint8_t *out[6];
		int n = 0;
		for (int i = 0; i < 3; i++) {
			xld(e, 0, a + i);
			xucomg(e, 0, b + i);
			out[n++] = jcc8(e, CC_P);
			out[n++] = jcc8(e, CC_NE);
		}
		movri(e, RAX, op == OP_EQ_V ? 1 : 0);
		{
			uint8_t *done = jmp8(e);
			for (int i = 0; i < n; i++)
				fix8(e, out[i]);
			movri(e, RAX, op == OP_EQ_V ? 0 : 1);
			fix8(e, done);
		}
		st_fbool(e, c);
		return true;
	}
	case OP_EQ_E: case OP_EQ_FNC: case OP_NE_E: case OP_NE_FNC:
		ldg(e, RCX, a);
		alug(e, 0x3b, RCX, b);
		cc_bool(e, (op == OP_EQ_E || op == OP_EQ_FNC) ? CC_E : CC_NE);
		st_fbool(e, c);
		return true;
	case OP_NOT_F:
		testgi(e, a, 0x7fffffff);
		cc_bool(e, CC_E);
		st_fbool(e, c);
		return true;
	case OP_NOT_V: {
		uint8_t *out[3];
		for (int i = 0; i < 3; i++) {
			testgi(e, a + i, 0x7fffffff);
			out[i] = jcc8(e, CC_NE);
		}
		movri(e, RAX, 1);
		{
			uint8_t *done = jmp8(e);
			for (int i = 0; i < 3; i++)
				fix8(e, out[i]);
			alurr(e, 0x33, RAX, RAX);
			fix8(e, done);
		}
		st_fbool(e, c);
		return true;
	}
	case OP_NOT_ENT: case OP_NOT_FNC:
		opm(e, 0, 0, 0x83, 7, RG, -1, 0, GD(a)); /* cmp dword [a], 0 */
		b1(e, 0);
		cc_bool(e, CC_E);
		st_fbool(e, c);
		return true;
	case OP_EQ_S: case OP_NE_S: {
		/* the same handle is equal; the rest is the helper's */
		uint8_t *same, *done;
		ldg(e, RAX, a);
		alug(e, 0x3b, RAX, b);
		same = jcc8(e, CC_E);
		call_helper(e, (void *)QC_JitStrEq, s);
		done = jmp8(e);
		fix8(e, same);
		movri(e, RAX, 1);
		fix8(e, done);
		if (op == OP_NE_S)
			alui(e, 6, RAX, 1);
		st_fbool(e, c);
		return true;
	}
	case OP_NOT_S:
		call_helper(e, (void *)QC_JitStrEmpty, s);
		st_fbool(e, c);
		return true;

	/* --- entity fields --- */
	case OP_LOADF_F: case OP_LOADF_S: case OP_LOADF_ENT: case OP_LOADF_FLD: case OP_LOADF_FNC:
	case OP_LOADF_I: case OP_LOADF_P:
		field_index(e, s, a, b, 1);
		ldf(e, RAX, 0);
		stg(e, c, RAX);
		return true;
	case OP_LOADF_V: case OP_LOADF_I64: {
		int n = op == OP_LOADF_V ? 3 : 2;
		field_index(e, s, a, b, n);
		for (int i = 0; i < n; i++) {
			ldf(e, RCX, i);
			stg(e, c + i, RCX);
		}
		return true;
	}
	case OP_FLDADDRESS:
		field_index(e, s, a, b, 1);
		opr(e, 0, 0, 0xc1, 4, RAX); /* shl eax, 2 */
		b1(e, 2);
		alui(e, 0, RAX, (int32_t)vm->edicts_ofs);
		stg(e, c, RAX);
		return true;
	case OP_STOREF_F: case OP_STOREF_S: case OP_STOREF_I:
		field_index(e, s, a, b, 1);
		ldg(e, RCX, c);
		stf(e, 0, RCX);
		return true;
	case OP_STOREF_V: case OP_STOREF_I64: {
		int n = op == OP_STOREF_V ? 3 : 2;
		field_index(e, s, a, b, n);
		for (int i = 0; i < n; i++) {
			ldg(e, RCX, c + i);
			stf(e, i, RCX);
		}
		return true;
	}

	/* --- plain stores --- */
	case OP_STORE_F: case OP_STORE_S: case OP_STORE_ENT: case OP_STORE_FLD: case OP_STORE_FNC:
	case OP_STORE_I: case OP_STORE_P:
		ldg(e, RAX, a);
		stg(e, b, RAX);
		return true;
	case OP_STORE_V: case OP_STORE_I64:
		for (int i = 0; i < (op == OP_STORE_V ? 3 : 2); i++) {
			ldg(e, RAX, a + i);
			stg(e, b + i, RAX);
		}
		return true;
	case OP_STORE_IF:
		xcvtig(e, 0, a);
		xst(e, b, 0);
		return true;
	case OP_STORE_FI:
		f2i_g(e, RAX, a);
		stg(e, b, RAX);
		return true;

	/* --- pointers --- */
	case OP_STOREP_F: case OP_STOREP_S: case OP_STOREP_ENT: case OP_STOREP_FLD: case OP_STOREP_FNC:
	case OP_STOREP_I:
		pointer(e, s, b, c, 1);
		ldg(e, RCX, a);
		stp(e, 0, RCX);
		return true;
	case OP_STOREP_V: case OP_STOREP_I64: {
		int n = op == OP_STOREP_V ? 3 : 2;
		pointer(e, s, b, c, n);
		for (int i = 0; i < n; i++) {
			ldg(e, RCX, a + i);
			stp(e, i, RCX);
		}
		return true;
	}
	case OP_LOADP_F: case OP_LOADP_S: case OP_LOADP_ENT: case OP_LOADP_FLD: case OP_LOADP_FNC:
	case OP_LOADP_I:
		pointer(e, s, a, b, 1);
		ldp(e, RCX, 0);
		stg(e, c, RCX);
		return true;
	case OP_LOADP_V: case OP_LOADP_I64: {
		int n = op == OP_LOADP_V ? 3 : 2;
		pointer(e, s, a, b, n);
		for (int i = 0; i < n; i++) {
			ldp(e, RCX, i);
			stg(e, c + i, RCX);
		}
		return true;
	}

	/* --- global arrays and addresses --- */
	case OP_LOADA_F: case OP_LOADA_S: case OP_LOADA_ENT: case OP_LOADA_FLD: case OP_LOADA_FNC:
	case OP_LOADA_I: case OP_LOADA_V: case OP_LOADA_I64: {
		int n = op == OP_LOADA_V ? 3 : op == OP_LOADA_I64 ? 2 : 1;
		/* a + b must lie in [0, globalslots - n] */
		ldg(e, RCX, b);
		alui(e, 7, RCX, -a);
		jcc_fail(e, CC_L, s);
		alui(e, 7, RCX, vm->globalslots - n - a);
		jcc_fail(e, CC_G, s);
		alui(e, 0, RCX, a);
		for (int i = 0; i < n; i++) {
			opm(e, 0, 0, 0x8b, RAX, RG, RCX, 4, i * 4);
			stg(e, c + i, RAX);
		}
		return true;
	}
	case OP_GLOBALADDRESS:
		ldg(e, RAX, b);
		alui(e, 0, RAX, a);
		opr(e, 0, 0, 0xc1, 4, RAX);
		b1(e, 2);
		alui(e, 0, RAX, (int32_t)vm->globals_addr);
		stg(e, c, RAX);
		return true;
	case OP_ADD_PIW:
		ldg(e, RAX, b);
		opr(e, 0, 0, 0xc1, 4, RAX);
		b1(e, 2);
		alug(e, 0x03, RAX, a);
		stg(e, c, RAX);
		return true;
	case OP_BOUNDCHECK:
		ldg(e, RAX, a);
		alui(e, 7, RAX, (int32_t)c);
		jcc_fail(e, CC_L, s);
		alui(e, 7, RAX, (int32_t)b);
		jcc_fail(e, CC_GE, s);
		return true;

	/* --- b op= a, also into c --- */
	case OP_MULSTORE_F: case OP_DIVSTORE_F: case OP_ADDSTORE_F: case OP_SUBSTORE_F: {
		int sop = op == OP_MULSTORE_F ? 0x59 : op == OP_DIVSTORE_F ? 0x5e : op == OP_ADDSTORE_F ? 0x58 : 0x5c;
		xld(e, 0, b);
		xopg(e, sop, 0, a);
		xst(e, b, 0);
		if (c)
			xst(e, c, 0);
		return true;
	}
	case OP_BITSETSTORE_F: case OP_BITCLRSTORE_F:
		f2i_g(e, RDX, b);
		f2i_g(e, RAX, a);
		if (op == OP_BITCLRSTORE_F) {
			opr(e, 0, 0, 0xf7, 2, RAX); /* not eax */
			alurr(e, 0x23, RAX, RDX);
		} else {
			alurr(e, 0x0b, RAX, RDX);
		}
		xcvtir(e, 0, RAX);
		xst(e, b, 0);
		if (c)
			xst(e, c, 0);
		return true;

	/* --- integers --- */
	case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_BITAND_I: case OP_BITOR_I: case OP_BITXOR_I: {
		int sop = op == OP_ADD_I ? 0x03 : op == OP_SUB_I ? 0x2b : op == OP_MUL_I ? 0x0faf :
		         op == OP_BITAND_I ? 0x23 : op == OP_BITOR_I ? 0x0b : 0x33;
		ldg(e, RAX, a);
		alug(e, sop, RAX, b);
		stg(e, c, RAX);
		return true;
	}
	case OP_DIV_I: {
		uint8_t *zero, *norm, *done1, *done2;
		ldg(e, RCX, b);
		opr(e, 0, 0, 0x85, RCX, RCX);
		zero = jcc8(e, CC_E);
		ldg(e, RAX, a);
		alui(e, 7, RCX, -1);
		norm = jcc8(e, CC_NE);
		alui(e, 7, RAX, (int32_t)0x80000000u);
		done1 = jcc8(e, CC_E); /* INT_MIN / -1 stays INT_MIN */
		fix8(e, norm);
		b1(e, 0x99); /* cdq */
		opr(e, 0, 0, 0xf7, 7, RCX); /* idiv ecx */
		done2 = jmp8(e);
		fix8(e, zero);
		alurr(e, 0x33, RAX, RAX);
		fix8(e, done1);
		fix8(e, done2);
		stg(e, c, RAX);
		return true;
	}
	case OP_DIV_U: {
		uint8_t *zero, *done;
		ldg(e, RCX, b);
		opr(e, 0, 0, 0x85, RCX, RCX);
		zero = jcc8(e, CC_E);
		ldg(e, RAX, a);
		alurr(e, 0x33, RDX, RDX);
		opr(e, 0, 0, 0xf7, 6, RCX); /* div ecx */
		done = jmp8(e);
		fix8(e, zero);
		alurr(e, 0x33, RAX, RAX);
		fix8(e, done);
		stg(e, c, RAX);
		return true;
	}
	case OP_ADD_FI: case OP_SUB_FI: case OP_MUL_FI: case OP_DIV_FI: {
		/* a->f op (float)b->i */
		int sop = op == OP_ADD_FI ? 0x58 : op == OP_SUB_FI ? 0x5c : op == OP_MUL_FI ? 0x59 : 0x5e;
		xcvtig(e, 1, b);
		xld(e, 0, a);
		xoprr(e, sop, 0, 1);
		xst(e, c, 0);
		return true;
	}
	case OP_ADD_IF: case OP_SUB_IF: case OP_MUL_IF: case OP_DIV_IF: {
		int sop = op == OP_ADD_IF ? 0x58 : op == OP_SUB_IF ? 0x5c : op == OP_MUL_IF ? 0x59 : 0x5e;
		xcvtig(e, 0, a);
		xopg(e, sop, 0, b);
		xst(e, c, 0);
		return true;
	}
	case OP_CONV_IF:
		xcvtig(e, 0, a);
		xst(e, c, 0);
		return true;
	case OP_CONV_FI:
		f2i_g(e, RAX, a);
		stg(e, c, RAX);
		return true;
	case OP_LSHIFT_I: case OP_RSHIFT_I: case OP_RSHIFT_U:
		ldg(e, RCX, b);
		ldg(e, RAX, a);
		/* the count is masked to 5 bits, as & 31 does */
		opr(e, 0, 0, 0xd3, op == OP_LSHIFT_I ? 4 : op == OP_RSHIFT_I ? 7 : 5, RAX);
		stg(e, c, RAX);
		return true;
	case OP_BITAND_IF: case OP_BITOR_IF:
		f2i_g(e, RAX, b);
		alug(e, op == OP_BITAND_IF ? 0x23 : 0x0b, RAX, a);
		stg(e, c, RAX);
		return true;
	case OP_BITAND_FI: case OP_BITOR_FI:
		f2i_g(e, RAX, a);
		alug(e, op == OP_BITAND_FI ? 0x23 : 0x0b, RAX, b);
		stg(e, c, RAX);
		return true;
	case OP_EQ_I: case OP_NE_I: case OP_LE_I: case OP_GE_I: case OP_LT_I: case OP_GT_I: case OP_LE_U:
	case OP_LT_U: {
		int cc = op == OP_EQ_I ? CC_E : op == OP_NE_I ? CC_NE : op == OP_LE_I ? CC_LE :
		         op == OP_GE_I ? CC_GE : op == OP_LT_I ? CC_L : op == OP_GT_I ? CC_G :
		         op == OP_LE_U ? CC_BE : CC_B;
		ldg(e, RCX, a);
		alug(e, 0x3b, RCX, b);
		cc_bool(e, cc);
		stg(e, c, RAX);
		return true;
	}
	case OP_LE_IF: case OP_GE_IF: case OP_LT_IF: case OP_GT_IF: case OP_EQ_IF: case OP_NE_IF:
	case OP_LE_FI: case OP_GE_FI: case OP_LT_FI: case OP_GT_FI: case OP_EQ_FI: case OP_NE_FI: {
		/* xmm0 = left, xmm1 = right, either one converted from an int */
		bool left_int = op == OP_LE_IF || op == OP_GE_IF || op == OP_LT_IF || op == OP_GT_IF || op == OP_EQ_IF ||
		                op == OP_NE_IF;
		int rel = (op == OP_LE_IF || op == OP_LE_FI) ? 0 : (op == OP_GE_IF || op == OP_GE_FI) ? 1 :
		          (op == OP_LT_IF || op == OP_LT_FI) ? 2 : (op == OP_GT_IF || op == OP_GT_FI) ? 3 :
		          (op == OP_EQ_IF || op == OP_EQ_FI) ? 4 : 5;
		if (left_int) {
			xcvtig(e, 0, a);
			xld(e, 1, b);
		} else {
			xld(e, 0, a);
			xcvtig(e, 1, b);
		}
		switch (rel) {
		case 0: xucomrr(e, 1, 0); cc_bool(e, CC_AE); break;
		case 1: xucomrr(e, 0, 1); cc_bool(e, CC_AE); break;
		case 2: xucomrr(e, 1, 0); cc_bool(e, CC_A); break;
		case 3: xucomrr(e, 0, 1); cc_bool(e, CC_A); break;
		case 4:
			xucomrr(e, 0, 1);
			setcc(e, CC_E, RAX);
			setcc(e, CC_NP, RCX);
			alurr(e, 0x23, RAX, RCX);
			movzx8(e, RAX, RAX);
			break;
		default:
			xucomrr(e, 0, 1);
			setcc(e, CC_NE, RAX);
			setcc(e, CC_P, RCX);
			alurr(e, 0x0b, RAX, RCX);
			movzx8(e, RAX, RAX);
			break;
		}
		stg(e, c, RAX);
		return true;
	}
	case OP_AND_I: case OP_OR_I: case OP_AND_IF: case OP_OR_IF: case OP_AND_FI: case OP_OR_FI: {
		bool af = op == OP_AND_FI || op == OP_OR_FI, bf = op == OP_AND_IF || op == OP_OR_IF;
		bool and = op == OP_AND_I || op == OP_AND_IF || op == OP_AND_FI;
		if (af)
			ftrue(e, a);
		else {
			opm(e, 0, 0, 0x83, 7, RG, -1, 0, GD(a));
			b1(e, 0);
			cc_bool(e, CC_NE);
		}
		alurr(e, 0x8b, RDX, RAX);
		if (bf)
			ftrue(e, b);
		else {
			opm(e, 0, 0, 0x83, 7, RG, -1, 0, GD(b));
			b1(e, 0);
			cc_bool(e, CC_NE);
		}
		alurr(e, and ? 0x23 : 0x0b, RAX, RDX);
		stg(e, c, RAX);
		return true;
	}
	case OP_NOT_I:
		opm(e, 0, 0, 0x83, 7, RG, -1, 0, GD(a));
		b1(e, 0);
		cc_bool(e, CC_E);
		stg(e, c, RAX);
		return true;

	/* --- control flow --- */
	case OP_IF: case OP_IFNOT:
		opm(e, 0, 0, 0x83, 7, RG, -1, 0, GD(a));
		b1(e, 0);
		branch(e, op == OP_IF ? CC_NE : CC_E, s, s + JUMPOP(b));
		return true;
	case OP_IF_F: case OP_IFNOT_F:
		testgi(e, a, 0x7fffffff);
		branch(e, op == OP_IF_F ? CC_NE : CC_E, s, s + JUMPOP(b));
		return true;
	case OP_IFS: case OP_IFNOTS:
		call_helper(e, (void *)QC_JitStrEmpty, s);
		opr(e, 0, 0, 0x85, RAX, RAX);
		branch(e, op == OP_IFS ? CC_E : CC_NE, s, s + JUMPOP(b));
		return true;
	case OP_GOTO:
		jump(e, s, s + JUMPOP(a));
		return true;

	case OP_CALL0: case OP_CALL1: case OP_CALL2: case OP_CALL3: case OP_CALL4: case OP_CALL5:
	case OP_CALL6: case OP_CALL7: case OP_CALL8: case OP_CALL1H: case OP_CALL2H: case OP_CALL3H:
	case OP_CALL4H: case OP_CALL5H: case OP_CALL6H: case OP_CALL7H: case OP_CALL8H:
		emit_call(e, s, st, op);
		return true;

	case OP_SWITCH_F: case OP_SWITCH_V: case OP_SWITCH_S: case OP_SWITCH_E: case OP_SWITCH_FNC:
	case OP_SWITCH_I:
		/* the value the cases after it compare with */
		opm(e, 0, 0, 0xc7, 0, RV, -1, 0, VMD(switch_op));
		u32(e, op);
		for (int i = 0; i < (op == OP_SWITCH_V ? 3 : 1); i++) {
			ldg(e, RAX, a + i);
			stvm(e, VMD(switch_value) + i * 4, RAX);
		}
		jump(e, s, s + JUMPOP(b));
		return true;
	case OP_CASE: {
		/* floats and plain words here, vectors and strings in the interpreter */
		uint8_t *notf, *nomatch1, *nomatch2, *match, *slow1, *slow2, *nomatch3, *done;
		int t = s + JUMPOP(b), targets[2] = {s + 1, t};
		ldvm(e, RAX, VMD(switch_op));
		alui(e, 7, RAX, OP_SWITCH_F);
		notf = jcc8(e, CC_NE);
		opm(e, 0xf3, 0, 0x0f10, 0, RV, -1, 0, VMD(switch_value));
		xucomg(e, 0, a);
		nomatch1 = jcc8(e, CC_P);
		nomatch2 = jcc8(e, CC_NE);
		match = jmp8(e);
		fix8(e, notf);
		alui(e, 7, RAX, OP_SWITCH_V);
		slow1 = jcc8(e, CC_E);
		alui(e, 7, RAX, OP_SWITCH_S);
		slow2 = jcc8(e, CC_E);
		ldvm(e, RAX, VMD(switch_value));
		alug(e, 0x3b, RAX, a);
		nomatch3 = jcc8(e, CC_NE);
		fix8(e, match);
		jump(e, s, t);
		fix8(e, slow1);
		fix8(e, slow2);
		step(e, s, targets, 2);
		fix8(e, nomatch1);
		fix8(e, nomatch2);
		fix8(e, nomatch3);
		(void)done;
		return true;
	}

	case OP_MULSTOREP_F: case OP_DIVSTOREP_F: case OP_ADDSTOREP_F: case OP_SUBSTOREP_F: {
		int sop = op == OP_MULSTOREP_F ? 0x59 : op == OP_DIVSTOREP_F ? 0x5e : op == OP_ADDSTOREP_F ? 0x58 : 0x5c;
		pointer(e, s, b, -1, 1);
		opm(e, 0xf3, 0, 0x0f10, 0, RDX, RAX, 1, 0);
		xopg(e, sop, 0, a);
		opm(e, 0xf3, 0, 0x0f11, 0, RDX, RAX, 1, 0);
		if (c)
			xst(e, c, 0);
		return true;
	}
	case OP_BITSETSTOREP_F: case OP_BITCLRSTOREP_F:
		pointer(e, s, b, -1, 1);
		opr(e, 0, 1, 0x03, RDX, RAX); /* add rdx, rax: rdx = p */
		b1(e, 0x52); /* push rdx */
		opm(e, 0xf3, 0, 0x0f10, 0, RDX, -1, 0, 0);
		f2i_xmm0(e);
		alurr(e, 0x8b, RCX, RAX);
		f2i_g(e, RAX, a);
		if (op == OP_BITCLRSTOREP_F) {
			opr(e, 0, 0, 0xf7, 2, RAX); /* not eax */
			alurr(e, 0x23, RAX, RCX);
		} else {
			alurr(e, 0x0b, RAX, RCX);
		}
		b1(e, 0x5a); /* pop rdx */
		xcvtir(e, 0, RAX);
		opm(e, 0xf3, 0, 0x0f11, 0, RDX, -1, 0, 0);
		if (c)
			xst(e, c, 0);
		return true;
	case OP_ADDSTORE_V: case OP_SUBSTORE_V: case OP_ADDSTOREP_V: case OP_SUBSTOREP_V: {
		/* p[i] += sign * a[i], with the multiply kept as the interpreter has it */
		bool ptr = op == OP_ADDSTOREP_V || op == OP_SUBSTOREP_V;
		int base = RG, idx = -1, scale = 0;
		int32_t d0 = GD(b);
		if (ptr) {
			pointer(e, s, b, -1, 3);
			base = RDX;
			idx = RAX;
			scale = 1;
			d0 = 0;
		}
		b1(e, 0x68);
		u32(e, (op == OP_ADDSTORE_V || op == OP_ADDSTOREP_V) ? 0x3f800000u : 0xbf800000u); /* push sign */
		opm(e, 0xf3, 0, 0x0f10, 1, RSP, -1, 0, 0); /* movss xmm1, [esp] */
		b1(e, 0x59); /* pop ecx */
		for (int i = 0; i < 3; i++) {
			xmov(e, 0, 1);
			xopg(e, 0x59, 0, a + i);
			opm(e, 0xf3, 0, 0x0f10, 2, base, idx, scale, d0 + i * 4);
			xoprr(e, 0x58, 2, 0);
			opm(e, 0xf3, 0, 0x0f11, 2, base, idx, scale, d0 + i * 4);
		}
		if (c)
			for (int i = 0; i < 3; i++) {
				opm(e, 0, 0, 0x8b, RCX, base, idx, scale, d0 + i * 4);
				stg(e, c + i, RCX);
			}
		return true;
	}

	/* --- 64-bit integers, as two words --- */
	case OP_BITAND_I64: case OP_BITOR_I64: case OP_BITXOR_I64: case OP_ADD_I64: case OP_SUB_I64: {
		int lo = op == OP_BITAND_I64 ? 0x23 : op == OP_BITOR_I64 ? 0x0b : op == OP_BITXOR_I64 ? 0x33 :
		         op == OP_ADD_I64 ? 0x03 : 0x2b;
		int hi = op == OP_ADD_I64 ? 0x13 : op == OP_SUB_I64 ? 0x1b : lo; /* adc, sbb */
		ldg(e, RAX, a);
		ldg(e, RDX, a + 1);
		alug(e, lo, RAX, b);
		alug(e, hi, RDX, b + 1);
		stg(e, c, RAX);
		stg(e, c + 1, RDX);
		return true;
	}
	case OP_EQ_I64: case OP_NE_I64:
		ldg(e, RCX, a);
		ldg(e, RDX, a + 1);
		alug(e, 0x33, RCX, b);
		alug(e, 0x33, RDX, b + 1);
		alurr(e, 0x0b, RCX, RDX);
		cc_bool(e, op == OP_EQ_I64 ? CC_E : CC_NE);
		stg(e, c, RAX);
		return true;

	case OP_RETURN:
		for (int i = 0; i < 3; i++) {
			ldg(e, RAX, a + i);
			stg(e, OFS_RETURN + i, RAX);
		}
		/* fall through */
	case OP_DONE:
		movri(e, RAX, (uint32_t)-1);
		jmp_to(e, TO_EPILOGUE);
		return true;

	default:
		return false;
	}
}

/* --- functions --------------------------------------------------------- */

static void prologue(emit_t *e)
{
	b1(e, 0x55); /* push ebp */
	b1(e, 0x53); /* push ebx */
#ifdef __x86_64__
	b1(e, 0x41);
	b1(e, 0x55); /* push r13 */
	opr(e, 0, 1, 0x89, RDI, RV); /* mov r13, rdi */
#else
	b1(e, 0x56); /* push esi */
	opm(e, 0, 0, 0x8b, RV, RSP, -1, 0, 16); /* mov esi, [esp + 16] */
#endif
	opm(e, 0, 1, 0x8b, RG, RV, -1, 0, (int32_t)offsetof(qcvm_t, globals));
	opm(e, 0, 1, 0x8b, RE, RV, -1, 0, (int32_t)offsetof(qcvm_t, mem));
	/* add rbp, edicts_ofs */
	opr(e, 0, 1, 0x81, 0, RE);
	u32(e, (uint32_t)e->vm->edicts_ofs);
}

static void epilogue(emit_t *e)
{
#ifdef __x86_64__
	b1(e, 0x41);
	b1(e, 0x5d); /* pop r13 */
#else
	b1(e, 0x5e); /* pop esi */
#endif
	b1(e, 0x5b);
	b1(e, 0x5d);
	b1(e, 0xc3);
}

/* The entry other native code calls f by, with eax the statement to come
 * back to and the caller's registers: what enter_function does, the body,
 * then leave_function. 0 is done, 1 is hooks on (the caller hands the rest
 * of its function to the interpreter), 2 is not entered (the caller takes
 * the interpreter's call path). Doing this here rather than at every call
 * keeps the code small. */
static void call_entry(emit_t *e, qcfunc_t *f)
{
	qcvm_t *vm = e->vm;
	const int ps = f->d.parm_start, locals = f->d.locals;
	const int pad = 16 - (int)sizeof(void *); /* the stack as the body's own calls have it */
	uint32_t no[8], bailed;
	int nno = 0;

	opr(e, 0, 1, 0x83, 5, RSP); b1(e, (uint8_t)pad);
	cmp_byte0(e, qc_tracefilter);
	no[nno++] = jcc32(e, CC_NE);
	cmp_byte0(e, &qc_profiling);
	no[nno++] = jcc32(e, CC_NE);
	opm(e, 0, 0, 0x80, 7, RV, -1, 0, VMD(debug_hooks));
	b1(e, 0);
	no[nno++] = jcc32(e, CC_NE);
	opm(e, 0, 0, 0x81, 7, RV, -1, 0, VMD(jit_nest));
	u32(e, QC_JIT_NEST);
	no[nno++] = jcc32(e, CC_GE);
	ldvm(e, RCX, VMD(depth));
	alui(e, 7, RCX, QC_MAX_DEPTH);
	no[nno++] = jcc32(e, CC_GE);
	ldvm(e, RDX, VMD(localstack_used));
	alui(e, 0, RDX, locals);
	opm(e, 0, 0, 0x3b, RDX, RV, -1, 0, VMD(localstack_size));
	no[nno++] = jcc32(e, CC_G);
	/* the frame (rcx = depth) */
	opr(e, 0, 0, 0xc1, 4, RCX);
	b1(e, 4);
	opm(e, 0, 0, 0x89, RAX, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, s));
	ldvm(e, RAX, VMD(xfunction));
	opm(e, 0, 0, 0x89, RAX, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, f));
	opm(e, 0, 0, 0xc7, 0, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, progs));
	u32(e, (uint32_t)vm->curprogs);
	ldvm(e, RAX, VMD(localstack_used));
	opm(e, 0, 0, 0x89, RAX, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, localbase));
	opm(e, 0, 0, 0xff, 0, RV, -1, 0, VMD(depth)); /* inc */
	/* save the locals: rdx = localstack + used */
	opm(e, 0, 1, 0x8b, RDX, RV, -1, 0, VMD(localstack));
	opm(e, 0, 1, 0x8d, RDX, RDX, RAX, 4, 0); /* lea rdx, [rdx + rax * 4] */
	copy_mem(e, RDX, -1, 0, RG, -1, GD(ps), locals);
	opm(e, 0, 0, 0x81, 0, RV, -1, 0, VMD(localstack_used));
	u32(e, (uint32_t)locals);
	/* arguments into the parameter locals, last word first */
	{
		int o = ps;
		for (int i = 0; i < f->callparms; i++) {
			int sz = f->d.parm_size[i];
			for (int k = sz - 1; k >= 0; k--) {
				ldg(e, RAX, OFS_PARM(i) + k);
				stg(e, o + k, RAX);
			}
			o += sz;
		}
	}
	opm(e, 0, 0, 0xc7, 0, RV, -1, 0, VMD(xfunction));
	u32(e, (uint32_t)(f - vm->functions));
	opm(e, 0, 0, 0xff, 0, RV, -1, 0, VMD(jit_nest));
	/* the body, called as C calls it */
#ifdef __x86_64__
	opr(e, 0, 1, 0x89, RV, RDI);
#else
#if defined(__linux__)
	alui(e, 5, RSP, 12);
#endif
	b1(e, 0x50 + RV);
#endif
	b1(e, 0xe8);
	u32(e, 0 - (here(e) + 4));
#ifndef __x86_64__
#if defined(__linux__)
	alui(e, 0, RSP, 16);
#else
	alui(e, 0, RSP, 4);
#endif
#endif
	opm(e, 0, 0, 0xff, 1, RV, -1, 0, VMD(jit_nest));
	opr(e, 0, 0, 0x85, RAX, RAX);
	bailed = jcc32(e, CC_NS);
	/* leave_function: the caller's locals back, the frame off */
	opm(e, 0, 0, 0xff, 1, RV, -1, 0, VMD(depth));
	ldvm(e, RCX, VMD(depth));
	opr(e, 0, 0, 0xc1, 4, RCX);
	b1(e, 4);
	opm(e, 0, 0, 0x8b, RAX, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, f));
	stvm(e, VMD(xfunction), RAX);
	opm(e, 0, 0, 0x8b, RAX, RV, RCX, 1, VMD(stack) + (int32_t)offsetof(qcframe_t, localbase));
	stvm(e, VMD(localstack_used), RAX);
	opm(e, 0, 1, 0x8b, RDX, RV, -1, 0, VMD(localstack));
	opm(e, 0, 1, 0x8d, RDX, RDX, RAX, 4, 0);
	copy_mem(e, RG, -1, GD(ps), RDX, -1, 0, locals);
	alurr(e, 0x33, RAX, RAX);
	opr(e, 0, 1, 0x83, 0, RSP); b1(e, (uint8_t)pad);
	b1(e, 0xc3);
	/* the body handed back mid-way: the interpreter finishes it */
	fix32(e, bailed);
	call_helper_eax(e, (void *)QC_JitFinish);
	opr(e, 0, 0, 0x85, RAX, RAX);
	setcc(e, CC_NE, RAX);
	movzx8(e, RAX, RAX);
	opr(e, 0, 1, 0x83, 0, RSP); b1(e, (uint8_t)pad);
	b1(e, 0xc3);
	for (int i = 0; i < nno; i++)
		fix32(e, no[i]);
	movri(e, RAX, 2);
	opr(e, 0, 1, 0x83, 0, RSP); b1(e, (uint8_t)pad);
	b1(e, 0xc3);
}

/* the most code memory one VM may take */
#ifdef BW_LOWMEM
#define MAX_CODE_BYTES (2u << 20)
#else
#define MAX_CODE_BYTES (32u << 20)
#endif

static qc_jitchunk_t *chunk_with_room(qc_jitmem_t *m, size_t need)
{
	qc_jitchunk_t *c;
	size_t size;
	for (c = m->chunks; c; c = c->next)
		if (c->size - c->used >= need)
			return c;
	size = need > CHUNK_BYTES ? (need + 4095) & ~(size_t)4095 : CHUNK_BYTES;
	if (m->total + size > MAX_CODE_BYTES)
		return NULL;
	c = Mem_Alloc(sizeof(*c));
	c->base = code_alloc(size, &c->mem);
	if (!c->base) {
		Mem_Free(c);
		return NULL;
	}
	c->size = size;
	c->next = m->chunks;
	m->chunks = c;
	m->total += size;
	return c;
}

/* room the code of one statement can take, at most */
#define STATEMENT_ROOM 16384
#define MAX_FUNCTION_STATEMENTS 40000

qc_jitfn_t QC_JitCompile(qcvm_t *vm, qcfunc_t *f)
{
	emit_t e;
	qc_jitchunk_t *chunk;
	int n = f->jit_end - f->d.first_statement;
	size_t size;
	uint32_t epi, stepcommon, entry = 0;
	const bool callable = f->d.locals <= CALL_MAX_LOCALS;
	uint32_t *stubat = NULL;
	uint8_t *code = NULL;

	f->jit_failed = true;
	if (vm->stmt32 || f->builtin || f->d.first_statement <= 0 || n <= 0 || n > MAX_FUNCTION_STATEMENTS)
		return NULL;
	if (!vm->jitmem)
		vm->jitmem = Mem_Alloc(sizeof(qc_jitmem_t));

	memset(&e, 0, sizeof(e));
	e.vm = vm;
	e.stmts = vm->statements;
	e.first = f->d.first_statement;
	e.end = f->jit_end;
	e.cap = (size_t)n * 128 + STATEMENT_ROOM * 2;
	e.base = Mem_Alloc(e.cap);
	e.p = e.base;
	e.limit = e.base + e.cap;
	e.label = Mem_Alloc(sizeof(uint32_t) * (size_t)n);

	prologue(&e);
	for (int s = e.first; s < e.end; s++) {
		/* positions are offsets, so the buffer can grow between statements */
		if ((size_t)(e.limit - e.p) < STATEMENT_ROOM) {
			size_t at = (size_t)(e.p - e.base);
			e.cap *= 2;
			e.base = Mem_Realloc(e.base, e.cap);
			e.p = e.base + at;
			e.limit = e.base + e.cap;
		}
		e.label[s - e.first] = here(&e);
		if (e.stmts[s].op == QCOP_FIND_S) {
			/* a whole search loop in one go is the interpreter's; it
			 * comes back at the match or out of the loop */
			int t[2] = {s + 7, s + 1 + JUMPOP(e.stmts[s + 1].b)};
			step_with(&e, (void *)QC_JitStepFused, s, t, 2);
		} else if (e.stmts[s].op == QCOP_ARGV_FIND) {
			/* the same for a word search; it can also stop at its call */
			int t[3] = {s + 6, s + 1 + JUMPOP(e.stmts[s + 1].b), s + 3};
			step_with(&e, (void *)QC_JitStepFused, s, t, 3);
		} else if (!statement(&e, s)) {
			const qcstatement_t *st = e.stmts + s;
			int t[4] = {s + 1, s + JUMPOP(st->a), s + JUMPOP(st->b), s + JUMPOP(st->c)};
			unsigned op = QC_PlainOp(st->op);
			bool branchy = op == OP_SWITCH_F || op == OP_SWITCH_V || op == OP_SWITCH_S || op == OP_SWITCH_E ||
			               op == OP_SWITCH_FNC || op == OP_SWITCH_I || op == OP_CASE || op == OP_CASERANGE;
			step(&e, s, t, branchy ? 4 : 1);
		}
	}
	if ((size_t)(e.limit - e.p) < STATEMENT_ROOM * 2 + (size_t)e.nstubs * 16 + (size_t)f->d.locals * 32) {
		size_t at = (size_t)(e.p - e.base);
		e.cap = at + STATEMENT_ROOM * 2 + (size_t)e.nstubs * 16 + (size_t)f->d.locals * 32;
		e.base = Mem_Realloc(e.base, e.cap);
		e.p = e.base + at;
		e.limit = e.base + e.cap;
	}
	/* running off the end is the interpreter's error to raise */
	movri(&e, RAX, (uint32_t)e.end);
	jmp_to(&e, TO_EPILOGUE);

	/* a stub's statement, in eax, rerun by the interpreter, which goes on from there */
	stepcommon = here(&e);
	call_helper_eax(&e, (void *)QC_JitStep);
	epi = here(&e);
	epilogue(&e);

	stubat = Mem_Alloc(sizeof(uint32_t) * (size_t)BW_MAX(e.nstubs, 1));
	for (int i = 0; i < e.nstubs; i++) {
		stubat[i] = here(&e);
		movri(&e, RAX, (uint32_t)e.stubs[i].s);
		b1(&e, 0xe9);
		u32(&e, (e.stubs[i].kind == STUB_STEP ? stepcommon : epi) - (here(&e) + 4));
	}
	if (callable) {
		entry = here(&e);
		call_entry(&e, f);
	}
	if (e.bad) {
		Con_DPrintf("%s: %s is not compiled\n", vm->name, f->name);
		goto out;
	}

	for (int i = 0; i < e.npatches; i++) {
		patch_t *pt = &e.patches[i];
		uint32_t to;
		if (pt->to >= 0)
			to = e.label[pt->to - e.first];
		else if (pt->to == TO_EPILOGUE)
			to = epi;
		else if (pt->to == TO_STEPCOMMON)
			to = stepcommon;
		else
			to = stubat[-3 - pt->to];
		{
			uint32_t rel = to - (pt->at + 4);
			memcpy(e.base + pt->at, &rel, 4);
		}
	}

	/* to its place, where calls out of it are fixed */
	size = (size_t)(e.p - e.base);
	chunk = chunk_with_room(vm->jitmem, size);
	if (!chunk)
		goto out;
	code = chunk->base + chunk->used;
	memcpy(code, e.base, size);
	for (int i = 0; i < e.next; i++) {
		uint32_t rel = (uint32_t)((uintptr_t)e.ext[i].fn - (uintptr_t)(code + e.ext[i].at + 4));
		memcpy(code + e.ext[i].at, &rel, 4);
	}
	chunk->used += (size + 15) & ~(size_t)15;
	vm->jitmem->functions++;
	vm->jitmem->code += size;
	/* where each function went, for matching profiler samples to QC */
	if (jit_map && jit_map->ivalue) {
		Con_Printf("jitmap %s %p %u %s\n", vm->name, (void *)code, (unsigned)size, f->name);
		if (jit_map->ivalue > 1)
			for (int i = 0; i < n; i++)
				Con_Printf("jitst %p %d\n", (void *)(code + e.label[i]), e.first + i);
	}
	f->jit = code;
	f->jit_call = callable ? code + entry : NULL;
	f->jit_failed = false;
out:
	Mem_Free(stubat);
	Mem_Free(e.label);
	Mem_Free(e.patches);
	Mem_Free(e.stubs);
	Mem_Free(e.ext);
	Mem_Free(e.base);
	return (qc_jitfn_t)(void *)code;
}

static void JitStats_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	for (int i = 0; i < QC_VMCount(); i++) {
		qcvm_t *vm = QC_VMAt(i);
		qc_jitmem_t *m = vm->jitmem;
		Con_Printf("%s: %d functions in %u KB of native code, %u KB taken\n", vm->name, m ? m->functions : 0,
		           m ? (unsigned)(m->code / 1024) : 0, m ? (unsigned)(m->total / 1024) : 0);
	}
}

/* Off by default. On a real x86 the server game runs in about three
 * quarters of the interpreter's time; in xemu it is a little slower than
 * the interpreter, which only a console can settle (docs/platforms/xbox.md,
 * "In-match benchmark"). */
void QC_JitInit(void)
{
	qc_jit_cvar = Cvar_Get("pr_jit", "0", 0, "Run QC functions as native code once they have been called often");
	jit_map = Cvar_Get("pr_jit_map", "0", 0, "Print where each QC function's native code goes (2: each statement too), for profiling");
	Cmd_AddD("pr_jitstats", JitStats_f, "How many QC functions each VM runs as native code, and their size");
}

#endif
