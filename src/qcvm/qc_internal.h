/* Bulwark engine - QuakeC VM internals shared between its source files.
 * ISC licence, see LICENSE. */
#ifndef BW_QC_INTERNAL_H
#define BW_QC_INTERNAL_H

#include "qcvm.h"
#include "opcodes.h"
#include <stdio.h>

/* opcodes the loader writes in place of what must not run: one this VM does
 * not know, and the statement after the last, so the interpreter never has
 * to range-check the statement it is about to run */
#define QCOP_BAD OP_NUMOPS
#define QCOP_END (OP_NUMOPS + 1)
/* A compare whose result only feeds the IFNOT after it runs as one
 * statement that does both, saving a dispatch on a ninth of all statements.
 * The IFNOT is left in place, as it can be a jump target. */
#define QCOP_NEF_IFNOT (OP_NUMOPS + 2)
#define QCOP_EQF_IFNOT (OP_NUMOPS + 3)
#define QCOP_EQS_IFNOT (OP_NUMOPS + 4)
#define QCOP_LTI_IFNOT (OP_NUMOPS + 5)
#define QCOP_BITAND_IFNOTF (OP_NUMOPS + 6)
/* Longer sequences the loader marks the same way, by the opcode of their
 * first statement; each runs its statements one after another, exactly as
 * they would run alone, without a dispatch between them:
 * a field copy, a field load and store of one word (or a vector) */
#define QCOP_FIELD_COPY (OP_NUMOPS + 7)
#define QCOP_FIELD_COPYV (OP_NUMOPS + 8)
/* two field loads, their float != and the IFNOT on it */
#define QCOP_FIELDS_NEF_IFNOT (OP_NUMOPS + 9)
/* i += step and the backward GOTO at the end of a loop */
#define QCOP_ADDI_GOTO (OP_NUMOPS + 10)
/* a whole search loop, for (i = ..; i < n; i++) if (s == table[i].f) */
#define QCOP_FIND_S (OP_NUMOPS + 11)
/* more compares, and a one-word field load, whose result only feeds the
 * IFNOT after them (as QCOP_NEF_IFNOT) */
#define QCOP_LTIF_IFNOT (OP_NUMOPS + 12)
#define QCOP_GTF_IFNOT (OP_NUMOPS + 13)
#define QCOP_LTF_IFNOT (OP_NUMOPS + 14)
#define QCOP_GEF_IFNOT (OP_NUMOPS + 15)
#define QCOP_LEF_IFNOT (OP_NUMOPS + 16)
#define QCOP_ANDF_IFNOT (OP_NUMOPS + 17)
#define QCOP_ORF_IFNOT (OP_NUMOPS + 18)
#define QCOP_NEI_IFNOT (OP_NUMOPS + 19)
#define QCOP_EQI_IFNOT (OP_NUMOPS + 20)
#define QCOP_ANDI_IFNOT (OP_NUMOPS + 21)
#define QCOP_NEIF_IFNOT (OP_NUMOPS + 22)
#define QCOP_NEE_IFNOT (OP_NUMOPS + 23)
#define QCOP_LOADF_IFNOT (OP_NUMOPS + 24)
/* a loop over a builtin's results, as Nuclide reads a split string:
 * for (c = 0; c < n; c += k) if (argv(c) == key) ... */
#define QCOP_CALL_SCAN (OP_NUMOPS + 25)
/* QCOP_FIELDS_NEF_IFNOT whose IFNOT skips one statement, as EvaluateEntity
 * tests each field: if (x != x_net) SetSendFlags(..); x_net = x */
#define QCOP_FIELD_SYNC (OP_NUMOPS + 26)
/* a one-word store followed by another, or by a call */
#define QCOP_STORE_RUN (OP_NUMOPS + 27)
#define QCOP_STORE_CALL (OP_NUMOPS + 28)
#define QCOP_COUNT (OP_NUMOPS + 29)

/* the opcode a statement had before the loader fused it */
static inline unsigned QC_PlainOp(unsigned op)
{
	switch (op) {
	case QCOP_NEF_IFNOT: return OP_NE_F;
	case QCOP_EQF_IFNOT: return OP_EQ_F;
	case QCOP_EQS_IFNOT: return OP_EQ_S;
	case QCOP_LTI_IFNOT: return OP_LT_I;
	case QCOP_BITAND_IFNOTF: return OP_BITAND;
	case QCOP_FIELD_COPY: return OP_LOADF_F;
	case QCOP_FIELD_COPYV: return OP_LOADF_V;
	case QCOP_FIELDS_NEF_IFNOT: return OP_LOADF_F;
	case QCOP_ADDI_GOTO: return OP_ADD_I;
	case QCOP_FIND_S: return OP_LT_I;
	case QCOP_LTIF_IFNOT: return OP_LT_IF;
	case QCOP_GTF_IFNOT: return OP_GT_F;
	case QCOP_LTF_IFNOT: return OP_LT_F;
	case QCOP_GEF_IFNOT: return OP_GE_F;
	case QCOP_LEF_IFNOT: return OP_LE_F;
	case QCOP_ANDF_IFNOT: return OP_AND_F;
	case QCOP_ORF_IFNOT: return OP_OR_F;
	case QCOP_NEI_IFNOT: return OP_NE_I;
	case QCOP_EQI_IFNOT: return OP_EQ_I;
	case QCOP_ANDI_IFNOT: return OP_AND_I;
	case QCOP_NEIF_IFNOT: return OP_NE_IF;
	case QCOP_NEE_IFNOT: return OP_NE_E;
	case QCOP_LOADF_IFNOT: return OP_LOADF_F;
	case QCOP_CALL_SCAN: return OP_LT_IF;
	case QCOP_FIELD_SYNC: return OP_LOADF_F;
	case QCOP_STORE_RUN: return OP_STORE_F;
	case QCOP_STORE_CALL: return OP_STORE_F;
	default: return op;
	}
}

#define QC_MAX_TOKENS 1024
/* One split of a string. The text of each token is kept in buf, and argv()
 * makes it a string the first time it is asked for: Nuclide tokenizes whole
 * decl files while a map loads, when no collection can run, and most tokens
 * are never read. */
typedef struct qc_tokset_s {
	int count, cap;
	string_t *argv; /* 0 until argv() asks for the token */
	int *start, *end;
	int *text; /* offset of the token's text in buf */
	int *len;
	char *buf;
	size_t buflen, bufcap;
	/* what tokenize_console split, for finding it again: the string
	 * handle (0 for none) and a copy of the text */
	string_t key;
	char *src;
	size_t srclen, srccap;
	bool src_valid;
	uint32_t used; /* when it was last used, for replacing the oldest */
} qc_tokset_t;

/* Nuclide looks up entity keys by splitting the same few spawn data strings
 * again and again, walking each class's inheritance chain, so the last
 * splits are kept. A split found again keeps the strings argv() made for
 * it, kept alive by the collector while the split is cached, but only in a
 * VM none of whose progs has a character store: otherwise s[i] = c into
 * one argv() result would also change the copy QC kept from an earlier
 * split, so argv() makes new strings, as before. Any character store also
 * empties the whole cache, as the text it was found by may have changed.
 * A progs added later with character stores could still write into a
 * string handed out twice before it was added. */
#ifdef BW_LOWMEM
#define QC_TOKEN_SETS 16
#else
#define QC_TOKEN_SETS 32
#endif
typedef struct qc_tokens_s {
	qc_tokset_t sets[QC_TOKEN_SETS];
	qc_tokset_t *cur; /* what argv() reads; NULL before the first split */
	uint32_t clock;
	uint32_t writes; /* qc_string_writes when the cache was last checked */
} qc_tokens_t;
/* the engine string table (qc_strings.c): slot u of handle
 * QC_STRING_RUNTIME | u holds its text, NULL when free, with the length in
 * the word before the text */
extern char **qc_strs;
extern uint32_t qc_nstrs;
/* set in the length word once the text may end before the length (QC
 * stored a NUL into it, or it was made with one) */
#define LEN_SHORTENED 0x80000000u

/* string == without a call when both are engine strings whose lengths
 * settle it; 1 equal, 0 not, -1 for QC_StringsEqual to decide */
static inline int QC_StringsEqualQuick(string_t a, string_t b)
{
	uint32_t ua = (uint32_t)a & ~QC_STRING_RUNTIME, ub = (uint32_t)b & ~QC_STRING_RUNTIME;
	const char *x, *y;
	uint32_t lx, ly;
	if (a == b)
		return 1;
	if (!((uint32_t)a & (uint32_t)b & QC_STRING_RUNTIME) || !ua || !ub || ua >= qc_nstrs || ub >= qc_nstrs)
		return -1;
	x = qc_strs[ua];
	y = qc_strs[ub];
	if (!x || !y)
		return -1;
	lx = ((const uint32_t *)(const void *)x)[-1];
	ly = ((const uint32_t *)(const void *)y)[-1];
	if ((lx | ly) & LEN_SHORTENED)
		return -1;
	if (lx != ly || x[0] != y[0])
		return 0;
	return -1;
}

/* bumped by every character store into a string (str[i] = c in QC) */
extern uint32_t qc_string_writes;
/* bumped by every collection that frees a string, whose slot a new string
 * can then take */
extern uint32_t qc_string_frees;

#define QC_MAX_FILES 64
#define QC_MAX_SEARCHES 32

typedef struct {
	FILE *fp;      /* real file for writing/appending */
	char *data;    /* whole file for reading */
	size_t len, pos;
	bool inuse;
	bool write;
} qc_file_t;

typedef struct {
	char **names;
	int count;
	bool inuse;
} qc_search_t;

/* a string buffer (buf_create): a growable array of strings, NULL for unset */
typedef struct {
	char **strs;
	int count, cap;
	bool inuse;
} qc_strbuf_t;

struct qc_hashtab_s;
struct qc_json_s;
typedef struct qc_findcache_s qc_findcache_t;

struct qc_extra_s {
	int calling_builtin; /* function index of the builtin being run */
	qc_file_t files[QC_MAX_FILES];
	qc_search_t searches[QC_MAX_SEARCHES];
	struct qc_hashtab_s **hashtabs;
	int numhashtabs;
	struct qc_json_s **json;
	int numjson;
	qc_strbuf_t *strbufs;
	int numstrbufs;
	qc_tokens_t tokens;
	qc_findcache_t *findcaches; /* qc_scan.c */
	int numfindcaches;
};

void QC_ExtraInit(qcvm_t *vm);
/* qc_autocvar.c */
void QC_BindAutocvars(qcvm_t *vm, int progs);
void QC_SyncAutocvars(qcvm_t *vm);
void QC_ExtraFree(qcvm_t *vm);
void QC_HeapInit(qcvm_t *vm);
void QC_PrintStringStats(void);
bool QC_HeapBlockUsed(qcvm_t *vm, size_t ofs, size_t *size);
const char *QC_ProgsString(qcvm_t *vm, int32_t ofs);
void QC_MarkHashTables(qcvm_t *vm, void (*mark)(uint32_t value));
void QC_MarkTokens(qcvm_t *vm, void (*mark)(uint32_t value));
void QC_FreeTokens(qcvm_t *vm);
/* str[idx] = ch from QC: the only way QC changes a string's text */
void QC_StringSetChar(qcvm_t *vm, string_t s, int64_t idx, int ch);
bool QC_AnyExecuting(void);
BW_NORETURN void QC_AbortUnwind(qcvm_t *vm);
bool QC_BuiltinImplemented(qcvm_t *vm, int fnum);
/* qc_scan.c: how many iterations of the QCOP_FIND_S loop at statement
 * sindex (s0, with its step statement) can be skipped, as none of them
 * would find a match; the loop runs the next one itself */
int QC_FindSkip(qcvm_t *vm, const qcstatement_t *s0, const qcstatement_t *step, int sindex);
void QC_FreeFindCaches(qcvm_t *vm);
/* the same for a QCOP_CALL_SCAN loop whose call is to fn, when fn is
 * argv(); at most maxskip */
int QC_CallScanSkip(qcvm_t *vm, const qcstatement_t *s0, const qcstatement_t *step, int sindex, qc_builtin_fn fn,
                    int maxskip);
/* qc_builtins_core.c: of the iterations of "if (argv(c) == key)" from c,
 * stepping by step while (float)c < limit, how many in a row before the
 * one that matches or the last find no match (at most maxskip), making
 * the strings they would have made; 0 unless fn is argv() */
int QC_ArgvScanSkip(qcvm_t *vm, qc_builtin_fn fn, int32_t c, int32_t step, float limit, string_t key, int maxskip);

#endif
