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
#define QCOP_COUNT (OP_NUMOPS + 2)

#define QC_MAX_TOKENS 1024
/* The text of each token is kept in buf, and argv() makes it a string the
 * first time it is asked for: Nuclide tokenizes whole decl files while a
 * map loads, when no collection can run, and most tokens are never read. */
typedef struct qc_tokens_s {
	int count;
	string_t argv[QC_MAX_TOKENS]; /* 0 until argv() asks for the token */
	int start[QC_MAX_TOKENS];
	int end[QC_MAX_TOKENS];
	int text[QC_MAX_TOKENS]; /* offset of the token's text in buf */
	int len[QC_MAX_TOKENS];
	char *buf;
	size_t buflen, bufcap;
	/* the text tokenize_console split last: Nuclide splits the same
	 * spawn data again for every key it looks up */
	char *src;
	size_t srclen, srccap;
	bool src_valid;
} qc_tokens_t;

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
bool QC_AnyExecuting(void);
BW_NORETURN void QC_AbortUnwind(qcvm_t *vm);
bool QC_BuiltinImplemented(qcvm_t *vm, int fnum);

#endif
