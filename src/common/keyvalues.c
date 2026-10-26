/* Bulwark engine - KeyValues text parser. Syntax and recovery rules:
 * docs/spec/vgui-res.md. ISC licence, see LICENSE. */
#include "keyvalues.h"

#include <stdio.h>
#include <stdlib.h>

enum { T_EOF, T_STR, T_OPEN, T_CLOSE, T_ERR };

typedef struct {
	const uint8_t *p, *end;
	unsigned flags;
	int line;
	char tok[KV_MAX_TOKEN + 1];
	size_t toklen;
	int tokline;
	kv_report_t *rep;
	bool failed;
} lex_t;

static bool lfail(lex_t *l, int line, const char *msg)
{
	snprintf(l->rep->err, sizeof(l->rep->err), "line %d: %s", line, msg);
	l->rep->errline = line;
	l->failed = true;
	return false;
}

/* counts a recovery; under KV_STRICT it fails instead */
static bool recover(lex_t *l, int *counter, int line, const char *msg)
{
	(*counter)++;
	if (l->flags & KV_STRICT)
		return lfail(l, line, msg);
	if (!l->rep->err[0]) {
		snprintf(l->rep->err, sizeof(l->rep->err), "line %d: %s", line, msg);
		l->rep->errline = line;
	}
	return true;
}

static bool put(lex_t *l, uint8_t c)
{
	if (l->toklen >= KV_MAX_TOKEN)
		return lfail(l, l->tokline, "token too long");
	l->tok[l->toklen++] = (char)c;
	return true;
}

static int lex(lex_t *l)
{
	l->toklen = 0;
	for (;;) {
		if (l->p >= l->end)
			return T_EOF;
		uint8_t c = *l->p;
		if (c == 0) {
			/* text ends at a NUL, as a C string would */
			l->rep->nul_end++;
			l->p = l->end;
			return T_EOF;
		}
		if (c == '\n') {
			l->line++;
			l->p++;
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
			l->p++;
		} else if (c == '/' && l->p + 1 < l->end && l->p[1] == '/') {
			while (l->p < l->end && *l->p != '\n' && *l->p != 0)
				l->p++;
		} else {
			break;
		}
	}
	l->tokline = l->line;
	uint8_t c = *l->p;
	if (c == '{' || c == '}') {
		l->p++;
		return c == '{' ? T_OPEN : T_CLOSE;
	}
	if (c == '"') {
		/* no escapes: a backslash is an ordinary character */
		l->p++;
		for (;;) {
			if (l->p >= l->end || *l->p == 0) {
				if (!recover(l, &l->rep->string_at_eof, l->tokline, "quoted string not closed before end of input"))
					return T_ERR;
				break;
			}
			c = *l->p;
			if (c == '"') {
				l->p++;
				break;
			}
			if (c == '\n' && !(l->flags & KV_SPAN_LINES)) {
				/* the newline is left for the whitespace skip to count */
				if (!recover(l, &l->rep->string_at_newline, l->tokline, "quoted string not closed before end of line"))
					return T_ERR;
				break;
			}
			if (c == '\n')
				l->line++;
			if (!put(l, c))
				return T_ERR;
			l->p++;
		}
		/* a CR before the end-of-line closure is line ending, not text */
		if (l->toklen && l->tok[l->toklen - 1] == '\r' && l->p < l->end && *l->p == '\n')
			l->toklen--;
		l->tok[l->toklen] = 0;
		return T_STR;
	}
	while (l->p < l->end) {
		c = *l->p;
		if (c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' ||
			c == '{' || c == '}' || c == '"')
			break;
		if (c == '/' && l->p + 1 < l->end && l->p[1] == '/')
			break;
		if (!put(l, c))
			return T_ERR;
		l->p++;
	}
	l->tok[l->toklen] = 0;
	return T_STR;
}

static kv_t *newnode(lex_t *l, const char *key, size_t klen, const char *value, size_t vlen, int line)
{
	if (l->rep->nodes >= KV_MAX_NODES) {
		lfail(l, line, "too many entries");
		return NULL;
	}
	size_t size = sizeof(kv_t) + klen + 1 + (value ? vlen + 1 : 0);
	kv_t *kv = Mem_Alloc(size);
	char *s = (char *)(kv + 1);
	memcpy(s, key, klen);
	s[klen] = 0;
	kv->key = s;
	if (value) {
		s += klen + 1;
		memcpy(s, value, vlen);
		s[vlen] = 0;
		kv->value = s;
	}
	kv->line = line;
	l->rep->nodes++;
	return kv;
}

void KV_Free(kv_t *kv)
{
	while (kv) {
		kv_t *next = kv->next;
		KV_Free(kv->child); /* depth is bounded by KV_MAX_DEPTH */
		Mem_Free(kv);
		kv = next;
	}
}

kv_t *KV_Parse(const uint8_t *buf, size_t len, unsigned flags, kv_report_t *rep)
{
	kv_report_t dummy;
	if (!rep)
		rep = &dummy;
	memset(rep, 0, sizeof(*rep));
	lex_t *l = Mem_Alloc(sizeof(*l));
	l->p = buf;
	l->end = buf + len;
	l->flags = flags;
	l->line = 1;
	l->rep = rep;

	kv_t *top = NULL;
	/* tail[d] is where the next entry at depth d is linked */
	kv_t **tail[KV_MAX_DEPTH + 1];
	int depth = 0;
	tail[0] = &top;
	char key[KV_MAX_TOKEN + 1];

	for (;;) {
		int t = lex(l);
		if (t == T_ERR)
			break;
		if (t == T_EOF)
			break;
		if (t == T_CLOSE) {
			if (depth == 0) {
				if (!recover(l, &rep->stray_close, l->tokline, "'}' with no open block"))
					break;
			} else {
				depth--;
			}
			continue;
		}
		int keyline = l->tokline;
		size_t klen = 0;
		if (t == T_OPEN) {
			if (!recover(l, &rep->block_without_key, keyline, "'{' where a key was expected"))
				break;
		} else {
			klen = l->toklen;
			memcpy(key, l->tok, klen + 1);
			t = lex(l);
			if (t == T_ERR)
				break;
			if (t == T_CLOSE || t == T_EOF) {
				if (!recover(l, &rep->key_without_value, keyline, "key without a value"))
					break;
				if (t == T_EOF)
					break;
				if (depth == 0) {
					if (!recover(l, &rep->stray_close, l->tokline, "'}' with no open block"))
						break;
				} else {
					depth--;
				}
				continue;
			}
		}
		kv_t *kv = newnode(l, key, klen, t == T_STR ? l->tok : NULL, l->toklen, keyline);
		if (!kv)
			break;
		*tail[depth] = kv;
		tail[depth] = &kv->next;
		if (t == T_OPEN) {
			if (depth == KV_MAX_DEPTH) {
				lfail(l, keyline, "blocks nested too deeply");
				break;
			}
			depth++;
			tail[depth] = &kv->child;
			if (depth > rep->maxdepth)
				rep->maxdepth = depth;
		}
	}
	if (!l->failed && depth > 0) {
		rep->unclosed_blocks += depth;
		if (flags & KV_STRICT)
			lfail(l, l->line, "block not closed before end of input");
		else if (!rep->err[0])
			snprintf(rep->err, sizeof(rep->err), "line %d: %d block(s) not closed before end of input", l->line, depth);
	}
	bool failed = l->failed;
	Mem_Free(l);
	if (failed) {
		KV_Free(top);
		return NULL;
	}
	if (!top && !rep->err[0])
		snprintf(rep->err, sizeof(rep->err), "no entries");
	return top;
}

static bool keyeq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return false;
	}
	return a[n] == 0;
}

static kv_t *findn(const kv_t *block, const char *key, size_t n)
{
	if (!block)
		return NULL;
	for (kv_t *kv = block->child; kv; kv = kv->next)
		if (keyeq(kv->key, key, n))
			return kv;
	return NULL;
}

kv_t *KV_Find(const kv_t *block, const char *key)
{
	return findn(block, key, strlen(key));
}

kv_t *KV_FindPath(const kv_t *block, const char *path)
{
	const kv_t *kv = block;
	while (kv && *path) {
		const char *slash = strchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : strlen(path);
		kv = findn(kv, path, n);
		path += n + (slash ? 1 : 0);
	}
	return (kv_t *)kv;
}

const char *KV_GetString(const kv_t *block, const char *key, const char *def)
{
	const kv_t *kv = KV_Find(block, key);
	return kv && kv->value ? kv->value : def;
}

int KV_GetInt(const kv_t *block, const char *key, int def)
{
	const char *s = KV_GetString(block, key, NULL);
	if (!s)
		return def;
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s)
		return def;
	if (v > INT32_MAX)
		v = INT32_MAX;
	if (v < INT32_MIN)
		v = INT32_MIN;
	return (int)v;
}

float KV_GetFloat(const kv_t *block, const char *key, float def)
{
	const char *s = KV_GetString(block, key, NULL);
	if (!s)
		return def;
	char *end;
	float v = strtof(s, &end);
	return end == s ? def : v;
}

bool KV_IsBlock(const kv_t *kv)
{
	return kv && !kv->value;
}
