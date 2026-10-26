/* Bulwark engine - KeyValues text, the format of VGUI layout (.res) and
 * scheme files. The syntax and the recovery rules for malformed input are
 * written up in docs/spec/vgui-res.md, where each one says how it was
 * established. ISC licence, see LICENSE.
 *
 * The parser works on a byte buffer with a length, never reads past it, and
 * keeps bytes as they are (no character set conversion: CSO files mix ASCII
 * with CP949 Korean). Malformed input is recovered where the spec says how,
 * and every recovery is counted in the report so a caller can tell a clean
 * file from a repaired one. */
#ifndef BW_KEYVALUES_H
#define BW_KEYVALUES_H

#include "common.h"

#define KV_MAX_DEPTH 64
#define KV_MAX_TOKEN 8192
#define KV_MAX_NODES (1 << 20)

typedef struct kv_s {
	const char *key;
	const char *value; /* NULL for a block */
	struct kv_s *child; /* first entry of a block */
	struct kv_s *next;  /* next entry in the same block, in file order */
	int line;           /* 1-based line of the key */
} kv_t;

enum {
	KV_STRICT = 1,       /* fail on anything that needs a recovery */
	KV_SPAN_LINES = 2,   /* a quoted string may continue past a newline */
};

typedef struct {
	int nodes;
	int maxdepth;
	/* recoveries, each counted once per occurrence */
	int string_at_newline;  /* quoted string closed at end of line */
	int string_at_eof;      /* quoted string closed at end of input */
	int key_without_value;  /* key followed by '}' or end of input: dropped */
	int block_without_key;  /* '{' where a key was expected: kept, key "" */
	int stray_close;        /* '}' at the top level: ignored */
	int unclosed_blocks;    /* blocks still open at end of input: closed */
	int nul_end;            /* a NUL byte ended the text before the buffer did */
	char err[160];          /* reason when parsing failed, or the first recovery */
	int errline;
} kv_report_t;

/* parses a buffer into a list of top-level entries (usually one root block);
 * returns NULL on failure, or for input with no entries, with the reason in
 * rep->err. rep may be NULL. Free the result with KV_Free. */
kv_t *KV_Parse(const uint8_t *buf, size_t len, unsigned flags, kv_report_t *rep);
void KV_Free(kv_t *kv);

/* first entry of a block whose key matches, ignoring ASCII case; NULL if none */
kv_t *KV_Find(const kv_t *block, const char *key);
/* follows a '/'-separated path of keys from a block */
kv_t *KV_FindPath(const kv_t *block, const char *path);
const char *KV_GetString(const kv_t *block, const char *key, const char *def);
/* leading integer of a value, as strtol would read it; def if absent or not a number */
int KV_GetInt(const kv_t *block, const char *key, int def);
float KV_GetFloat(const kv_t *block, const char *key, float def);
bool KV_IsBlock(const kv_t *kv);

#endif
