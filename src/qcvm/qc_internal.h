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
#define QCOP_COUNT (OP_NUMOPS + 12)

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
 * it: they are kept alive by the collector while the split is cached, and
 * a string's text only changes through a character store, which empties
 * the whole cache. */
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
/* bumped by every character store into a string (str[i] = c in QC) */
extern uint32_t qc_string_writes;

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
};

void QC_ExtraInit(qcvm_t *vm);
/* qc_autocvar.c */
void QC_BindAutocvars(qcvm_t *vm, int progs);
void QC_SyncAutocvars(qcvm_t *vm);
void QC_ExtraFree(qcvm_t *vm);
void QC_HeapInit(qcvm_t *vm);
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

#endif
