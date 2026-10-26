/* Bulwark engine - JSON document tree. ISC licence, see LICENSE.
 * Written from RFC 8259. */
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define JSON_MAX_DEPTH 256

/* --- construction ---------------------------------------------------- */

json_t *JSON_New(json_type_t type)
{
	json_t *j = Mem_Alloc(sizeof(*j));
	j->type = type;
	return j;
}

json_t *JSON_NewString(const char *s)
{
	json_t *j = JSON_New(JSON_STRING);
	j->str = Mem_Strdup(s ? s : "");
	return j;
}

/* shortest text that reads back as the same double */
static void format_number(char *out, size_t size, double n)
{
	if (!isfinite(n)) {
		Str_Copy(out, "0", size);
		return;
	}
	if (n == floor(n) && fabs(n) < 1e15) {
		snprintf(out, size, "%.0f", n);
		return;
	}
	for (int prec = 6; prec <= 17; prec++) {
		snprintf(out, size, "%.*g", prec, n);
		if (strtod(out, NULL) == n)
			break;
	}
}

json_t *JSON_NewNumber(double n)
{
	char buf[64];
	json_t *j = JSON_New(JSON_NUMBER);
	format_number(buf, sizeof(buf), n);
	j->num = n;
	j->str = Mem_Strdup(buf);
	return j;
}

json_t *JSON_NewNumberText(const char *text)
{
	json_t *j = JSON_New(JSON_NUMBER);
	j->num = strtod(text, NULL);
	j->str = Mem_Strdup(text);
	return j;
}

json_t *JSON_NewBool(bool b) { return JSON_New(b ? JSON_TRUE : JSON_FALSE); }

void JSON_Free(json_t *j)
{
	json_t *c;
	if (!j)
		return;
	c = j->child;
	while (c) {
		json_t *cn = c->next;
		JSON_Free(c);
		c = cn;
	}
	Mem_Free(j->key);
	Mem_Free(j->str);
	Mem_Free(j);
}

json_t *JSON_Clone(const json_t *j)
{
	json_t *c;
	if (!j)
		return NULL;
	c = JSON_New(j->type);
	c->num = j->num;
	if (j->str)
		c->str = Mem_Strdup(j->str);
	if (j->key)
		c->key = Mem_Strdup(j->key);
	for (const json_t *it = j->child; it; it = it->next) {
		json_t *k = JSON_Clone(it);
		if (c->last)
			c->last->next = k;
		else
			c->child = k;
		c->last = k;
		c->count++;
	}
	return c;
}

bool JSON_Equal(const json_t *a, const json_t *b)
{
	if (!a || !b)
		return a == b;
	if (a->type != b->type)
		return false;
	switch (a->type) {
	case JSON_NUMBER:
		return a->num == b->num;
	case JSON_STRING:
		return !strcmp(a->str, b->str);
	case JSON_ARRAY: {
		const json_t *x = a->child, *y = b->child;
		for (; x && y; x = x->next, y = y->next)
			if (!JSON_Equal(x, y))
				return false;
		return !x && !y;
	}
	case JSON_OBJECT:
		/* member order does not matter for equality */
		if (a->count != b->count)
			return false;
		for (const json_t *x = a->child; x; x = x->next)
			if (!JSON_Equal(x, JSON_Get(b, x->key)))
				return false;
		return true;
	default:
		return true;
	}
}

/* --- access ---------------------------------------------------------- */

bool JSON_IsString(const json_t *j) { return j && j->type == JSON_STRING; }
bool JSON_IsNumber(const json_t *j) { return j && j->type == JSON_NUMBER; }
bool JSON_IsObject(const json_t *j) { return j && j->type == JSON_OBJECT; }
bool JSON_IsArray(const json_t *j) { return j && j->type == JSON_ARRAY; }

json_t *JSON_Get(const json_t *obj, const char *key)
{
	if (!JSON_IsObject(obj))
		return NULL;
	for (json_t *it = obj->child; it; it = it->next)
		if (!strcmp(it->key, key))
			return it;
	return NULL;
}

const char *JSON_GetString(const json_t *obj, const char *key, const char *def)
{
	json_t *v = JSON_Get(obj, key);
	return JSON_IsString(v) ? v->str : def;
}

double JSON_GetNumber(const json_t *obj, const char *key, double def)
{
	json_t *v = JSON_Get(obj, key);
	return JSON_IsNumber(v) ? v->num : def;
}

bool JSON_GetBool(const json_t *obj, const char *key, bool def)
{
	json_t *v = JSON_Get(obj, key);
	if (!v)
		return def;
	if (v->type == JSON_TRUE)
		return true;
	if (v->type == JSON_FALSE)
		return false;
	if (v->type == JSON_NUMBER)
		return v->num != 0;
	return def;
}

static void link_after(json_t *parent, json_t *after, json_t *val)
{
	if (!after) {
		val->next = parent->child;
		parent->child = val;
		if (!parent->last)
			parent->last = val;
	} else {
		val->next = after->next;
		after->next = val;
		if (parent->last == after)
			parent->last = val;
	}
	parent->count++;
}

void JSON_Set(json_t *obj, const char *key, json_t *val)
{
	json_t *prev = NULL;
	if (!JSON_IsObject(obj)) {
		JSON_Free(val);
		return;
	}
	Mem_Free(val->key);
	val->key = Mem_Strdup(key);
	for (json_t *it = obj->child; it; prev = it, it = it->next) {
		if (strcmp(it->key, key))
			continue;
		val->next = it->next;
		if (prev)
			prev->next = val;
		else
			obj->child = val;
		if (obj->last == it)
			obj->last = val;
		it->next = NULL;
		JSON_Free(it);
		return;
	}
	link_after(obj, obj->last, val);
}

void JSON_SetString(json_t *obj, const char *key, const char *s) { JSON_Set(obj, key, JSON_NewString(s)); }
void JSON_SetNumber(json_t *obj, const char *key, double n) { JSON_Set(obj, key, JSON_NewNumber(n)); }
void JSON_SetBool(json_t *obj, const char *key, bool b) { JSON_Set(obj, key, JSON_NewBool(b)); }

bool JSON_Detach(json_t *parent, json_t *child)
{
	json_t *prev = NULL;
	if (!parent)
		return false;
	for (json_t *it = parent->child; it; prev = it, it = it->next) {
		if (it != child)
			continue;
		if (prev)
			prev->next = it->next;
		else
			parent->child = it->next;
		if (parent->last == it)
			parent->last = prev;
		it->next = NULL;
		parent->count--;
		return true;
	}
	return false;
}

bool JSON_Remove(json_t *obj, const char *key)
{
	json_t *v = JSON_Get(obj, key);
	if (!v || !JSON_Detach(obj, v))
		return false;
	JSON_Free(v);
	return true;
}

bool JSON_Rename(json_t *obj, const char *from, const char *to)
{
	json_t *v = JSON_Get(obj, from);
	if (!v)
		return false;
	JSON_Remove(obj, to);
	Mem_Free(v->key);
	v->key = Mem_Strdup(to);
	return true;
}

json_t *JSON_At(const json_t *arr, int index)
{
	if (!arr || index < 0)
		return NULL;
	for (json_t *it = arr->child; it; it = it->next)
		if (!index--)
			return it;
	return NULL;
}

void JSON_Append(json_t *arr, json_t *val)
{
	Mem_Free(val->key);
	val->key = NULL;
	link_after(arr, arr->last, val);
}

void JSON_InsertAfter(json_t *arr, json_t *after, json_t *val)
{
	Mem_Free(val->key);
	val->key = NULL;
	link_after(arr, after, val);
}

/* --- parsing --------------------------------------------------------- */

typedef struct {
	const char *start;
	const char *p;
	char *err;
	size_t errsize;
	bool failed;
} parser_t;

static void fail(parser_t *ps, const char *what)
{
	int line = 1;
	if (ps->failed)
		return;
	for (const char *c = ps->start; c < ps->p; c++)
		if (*c == '\n')
			line++;
	ps->failed = true;
	if (ps->err)
		snprintf(ps->err, ps->errsize, "line %d: %s", line, what);
}

static void skip_ws(parser_t *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
}

static void put_utf8(strbuf_t *sb, unsigned cp)
{
	char b[4];
	size_t n;
	if (cp < 0x80) {
		b[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xc0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xe0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[2] = (char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		b[0] = (char)(0xf0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		b[3] = (char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	SB_Append(sb, b, n);
}

static bool read_hex4(parser_t *ps, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = *ps->p++;
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return false;
	}
	*out = v;
	return true;
}

/* parses a string starting at the opening quote; returns a Mem_Alloc'd copy */
static char *parse_string(parser_t *ps)
{
	strbuf_t sb = {0};
	ps->p++;
	for (;;) {
		const char *run = ps->p;
		while (*ps->p && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20)
			ps->p++;
		SB_Append(&sb, run, (size_t)(ps->p - run));
		if (*ps->p == '"') {
			ps->p++;
			break;
		}
		if (*ps->p != '\\') {
			fail(ps, *ps->p ? "control character in string" : "unterminated string");
			SB_Free(&sb);
			return NULL;
		}
		ps->p++;
		switch (*ps->p++) {
		case '"': SB_Append(&sb, "\"", 1); break;
		case '\\': SB_Append(&sb, "\\", 1); break;
		case '/': SB_Append(&sb, "/", 1); break;
		case 'b': SB_Append(&sb, "\b", 1); break;
		case 'f': SB_Append(&sb, "\f", 1); break;
		case 'n': SB_Append(&sb, "\n", 1); break;
		case 'r': SB_Append(&sb, "\r", 1); break;
		case 't': SB_Append(&sb, "\t", 1); break;
		case 'u': {
			unsigned cp, lo;
			if (!read_hex4(ps, &cp)) {
				fail(ps, "bad \\u escape");
				SB_Free(&sb);
				return NULL;
			}
			if (cp >= 0xd800 && cp < 0xdc00 && ps->p[0] == '\\' && ps->p[1] == 'u') {
				const char *save = ps->p;
				ps->p += 2;
				if (read_hex4(ps, &lo) && lo >= 0xdc00 && lo < 0xe000)
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				else
					ps->p = save;
			}
			put_utf8(&sb, cp);
			break;
		}
		default:
			fail(ps, "bad escape");
			SB_Free(&sb);
			return NULL;
		}
	}
	if (!sb.data)
		return Mem_Strdup("");
	return sb.data;
}

static json_t *parse_value(parser_t *ps, int depth);

static json_t *parse_container(parser_t *ps, int depth, bool object)
{
	json_t *j = JSON_New(object ? JSON_OBJECT : JSON_ARRAY);
	char close = object ? '}' : ']';
	ps->p++;
	skip_ws(ps);
	if (*ps->p == close) {
		ps->p++;
		return j;
	}
	for (;;) {
		char *key = NULL;
		json_t *v;
		skip_ws(ps);
		if (object) {
			if (*ps->p != '"') {
				fail(ps, "expected member name");
				break;
			}
			if (!(key = parse_string(ps)))
				break;
			skip_ws(ps);
			if (*ps->p != ':') {
				Mem_Free(key);
				fail(ps, "expected ':'");
				break;
			}
			ps->p++;
		}
		v = parse_value(ps, depth + 1);
		if (!v) {
			Mem_Free(key);
			break;
		}
		v->key = key;
		link_after(j, j->last, v);
		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == close) {
			ps->p++;
			return j;
		}
		fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
		break;
	}
	JSON_Free(j);
	return NULL;
}

static json_t *parse_number(parser_t *ps)
{
	const char *s = ps->p;
	char buf[128];
	if (*ps->p == '-')
		ps->p++;
	if (*ps->p < '0' || *ps->p > '9') {
		fail(ps, "unexpected character");
		return NULL;
	}
	while (*ps->p >= '0' && *ps->p <= '9')
		ps->p++;
	if (*ps->p == '.') {
		ps->p++;
		while (*ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if (*ps->p == 'e' || *ps->p == 'E') {
		ps->p++;
		if (*ps->p == '+' || *ps->p == '-')
			ps->p++;
		while (*ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if ((size_t)(ps->p - s) >= sizeof(buf)) {
		fail(ps, "number too long");
		return NULL;
	}
	memcpy(buf, s, (size_t)(ps->p - s));
	buf[ps->p - s] = 0;
	return JSON_NewNumberText(buf);
}

static json_t *parse_value(parser_t *ps, int depth)
{
	if (depth > JSON_MAX_DEPTH) {
		fail(ps, "nested too deeply");
		return NULL;
	}
	skip_ws(ps);
	switch (*ps->p) {
	case '{':
		return parse_container(ps, depth, true);
	case '[':
		return parse_container(ps, depth, false);
	case '"': {
		char *s = parse_string(ps);
		json_t *j;
		if (!s)
			return NULL;
		j = JSON_New(JSON_STRING);
		j->str = s;
		return j;
	}
	case 't':
		if (!strncmp(ps->p, "true", 4)) {
			ps->p += 4;
			return JSON_New(JSON_TRUE);
		}
		break;
	case 'f':
		if (!strncmp(ps->p, "false", 5)) {
			ps->p += 5;
			return JSON_New(JSON_FALSE);
		}
		break;
	case 'n':
		if (!strncmp(ps->p, "null", 4)) {
			ps->p += 4;
			return JSON_New(JSON_NULL);
		}
		break;
	case 0:
		fail(ps, "unexpected end of input");
		return NULL;
	default:
		return parse_number(ps);
	}
	fail(ps, "unexpected character");
	return NULL;
}

json_t *JSON_Parse(const char *text, char *err, size_t errsize)
{
	parser_t ps = {text, text, err, errsize, false};
	json_t *j;
	if (err && errsize)
		err[0] = 0;
	if (!text) {
		fail(&ps, "no input");
		return NULL;
	}
	if (!strncmp(ps.p, "\xef\xbb\xbf", 3)) /* UTF-8 byte order mark, common in tool-written files */
		ps.p += 3;
	j = parse_value(&ps, 0);
	if (!j)
		return NULL;
	skip_ws(&ps);
	if (*ps.p) {
		fail(&ps, "trailing characters");
		JSON_Free(j);
		return NULL;
	}
	return j;
}

/* --- writing --------------------------------------------------------- */

static void write_string(strbuf_t *sb, const char *s)
{
	SB_Append(sb, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"': SB_Append(sb, "\\\"", 2); break;
		case '\\': SB_Append(sb, "\\\\", 2); break;
		case '\n': SB_Append(sb, "\\n", 2); break;
		case '\r': SB_Append(sb, "\\r", 2); break;
		case '\t': SB_Append(sb, "\\t", 2); break;
		case '\b': SB_Append(sb, "\\b", 2); break;
		case '\f': SB_Append(sb, "\\f", 2); break;
		default:
			if (c < 0x20)
				SB_Printf(sb, "\\u%04x", c);
			else
				SB_Append(sb, (const char *)s, 1);
		}
	}
	SB_Append(sb, "\"", 1);
}

static void newline(strbuf_t *sb, int indent, int level)
{
	if (indent < 0)
		return;
	SB_Append(sb, "\n", 1);
	for (int i = 0; i < indent * level; i++)
		SB_Append(sb, " ", 1);
}

static void write_value(strbuf_t *sb, const json_t *j, int indent, int level)
{
	switch (j->type) {
	case JSON_NULL: SB_AppendStr(sb, "null"); break;
	case JSON_TRUE: SB_AppendStr(sb, "true"); break;
	case JSON_FALSE: SB_AppendStr(sb, "false"); break;
	case JSON_NUMBER: SB_AppendStr(sb, j->str ? j->str : "0"); break;
	case JSON_STRING: write_string(sb, j->str); break;
	case JSON_ARRAY:
	case JSON_OBJECT: {
		bool obj = j->type == JSON_OBJECT;
		SB_Append(sb, obj ? "{" : "[", 1);
		if (!j->child) {
			SB_Append(sb, obj ? "}" : "]", 1);
			break;
		}
		for (const json_t *it = j->child; it; it = it->next) {
			newline(sb, indent, level + 1);
			if (obj) {
				write_string(sb, it->key);
				SB_AppendStr(sb, indent < 0 ? ":" : ": ");
			}
			write_value(sb, it, indent, level + 1);
			if (it->next)
				SB_Append(sb, ",", 1);
		}
		newline(sb, indent, level);
		SB_Append(sb, obj ? "}" : "]", 1);
		break;
	}
	}
}

void JSON_Write(strbuf_t *sb, const json_t *j, int indent)
{
	if (j)
		write_value(sb, j, indent, 0);
}
