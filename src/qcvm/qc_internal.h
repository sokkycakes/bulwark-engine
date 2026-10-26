/* Bulwark engine - QuakeC VM internals shared between its source files.
 * ISC licence, see LICENSE. */
#ifndef BW_QC_INTERNAL_H
#define BW_QC_INTERNAL_H

#include "qcvm.h"
#include "opcodes.h"
#include <stdio.h>

#define QC_MAX_TOKENS 256
typedef struct qc_tokens_s {
	int count;
	string_t argv[QC_MAX_TOKENS];
	int start[QC_MAX_TOKENS];
	int end[QC_MAX_TOKENS];
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
	qc_tokens_t tokens;
};

void QC_ExtraInit(qcvm_t *vm);
void QC_ExtraFree(qcvm_t *vm);
void QC_HeapInit(qcvm_t *vm);
bool QC_HeapBlockUsed(qcvm_t *vm, size_t ofs, size_t *size);
int QC_VMCount(void);
qcvm_t *QC_VMAt(int i);
const char *QC_ProgsString(qcvm_t *vm, int32_t ofs);
void QC_MarkHashTables(qcvm_t *vm, void (*mark)(uint32_t value));
bool QC_AnyExecuting(void);
BW_NORETURN void QC_AbortUnwind(qcvm_t *vm);
bool QC_BuiltinImplemented(qcvm_t *vm, int fnum);

#endif
