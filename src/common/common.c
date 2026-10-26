/* Bulwark engine - shared helpers. ISC licence, see LICENSE. */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

int developer = 0;
static con_hook_fn con_hook, con_log_hook;
static strbuf_t *con_redirect;

void Con_Redirect(strbuf_t *sb) { con_redirect = sb; }

void Con_SetHook(con_hook_fn fn) { con_hook = fn; }
void Con_SetLogHook(con_hook_fn fn) { con_log_hook = fn; }

static loading_hook_fn loading_hook;

void Host_SetLoadingHook(loading_hook_fn fn) { loading_hook = fn; }

void Host_Loading(const char *stage, const char *map, float frac)
{
	if (loading_hook)
		loading_hook(stage, map, frac);
}

void Con_Print(const char *text)
{
	if (con_hook)
		con_hook(text);
	if (con_log_hook)
		con_log_hook(text);
	if (con_redirect)
		SB_AppendStr(con_redirect, text);
	fputs(text, stdout);
	fflush(stdout);
}

void Con_Printf(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Print(buf);
}

void Con_DPrintf(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	if (!developer)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Con_Print(buf);
}

void Sys_Error(const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "FATAL: %s\n", buf);
	exit(1);
}

double host_time;

double Sys_Seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

double Sys_Milliseconds(void) { return Sys_Seconds() * 1000.0; }

void Sys_Sleep(double seconds)
{
	struct timespec ts;
	if (seconds <= 0)
		return;
	ts.tv_sec = (time_t)seconds;
	ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);
}

void *Mem_Alloc(size_t size)
{
	void *p = calloc(1, size ? size : 1);
	if (!p)
		Sys_Error("out of memory allocating %zu bytes", size);
	return p;
}

void *Mem_Realloc(void *p, size_t size)
{
	void *n = realloc(p, size ? size : 1);
	if (!n)
		Sys_Error("out of memory reallocating %zu bytes", size);
	return n;
}

void Mem_Free(void *p) { free(p); }

char *Mem_Strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = Mem_Alloc(n);
	memcpy(d, s, n);
	return d;
}

size_t Str_Copy(char *dst, const char *src, size_t dstsize)
{
	size_t n = strlen(src);
	if (dstsize) {
		size_t c = n < dstsize - 1 ? n : dstsize - 1;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return n;
}

size_t Str_Cat(char *dst, const char *src, size_t dstsize)
{
	size_t l = strlen(dst);
	if (l >= dstsize)
		return l + strlen(src);
	return l + Str_Copy(dst + l, src, dstsize - l);
}

int Str_ICmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

int Str_NICmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
	return 0;
}

bool Str_NeedsEscape(const char *s)
{
	return strpbrk(s, "\"\\\n\r\t") != NULL;
}

size_t Str_ParseEscaped(const char **pp, char *out, size_t outsize)
{
	const char *p = *pp + 2; /* past \" */
	size_t n = 0;
	while (*p && *p != '"') {
		char c = *p++;
		if (c == '\\' && *p) {
			c = *p++;
			if (c == 'n')
				c = '\n';
			else if (c == 't')
				c = '\t';
			else if (c == 'r')
				c = '\r';
		}
		if (n + 1 < outsize)
			out[n++] = c;
	}
	if (*p == '"')
		p++;
	if (outsize)
		out[n] = 0;
	*pp = p;
	return n;
}

bool Str_WildMatch(const char *p, const char *s, bool nocase)
{
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (*p == '*')
				p++;
			if (!*p)
				return true;
			for (; *s; s++)
				if (Str_WildMatch(p, s, nocase))
					return true;
			return false;
		}
		if (!*s)
			return false;
		if (*p != '?') {
			int a = *p, b = *s;
			if (nocase) {
				a = tolower((unsigned char)a);
				b = tolower((unsigned char)b);
			}
			if (a != b)
				return false;
		}
	}
	return !*s;
}

void SB_Append(strbuf_t *sb, const char *s, size_t n)
{
	if (sb->len + n + 1 > sb->cap) {
		size_t cap = sb->cap ? sb->cap : 64;
		while (cap < sb->len + n + 1)
			cap *= 2;
		sb->data = Mem_Realloc(sb->data, cap);
		sb->cap = cap;
	}
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = 0;
}

void SB_AppendStr(strbuf_t *sb, const char *s) { SB_Append(sb, s, strlen(s)); }

void SB_Printf(strbuf_t *sb, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(buf)) {
		SB_Append(sb, buf, (size_t)n);
	} else {
		char *big = Mem_Alloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		SB_Append(sb, big, (size_t)n);
		Mem_Free(big);
	}
}

void SB_Free(strbuf_t *sb)
{
	Mem_Free(sb->data);
	memset(sb, 0, sizeof(*sb));
}

const char *COM_Parse(const char *data, char *token, size_t tokensize)
{
	size_t len = 0;
	token[0] = 0;
	if (!data)
		return NULL;
	for (;;) {
		while (*data && (unsigned char)*data <= ' ')
			data++;
		if (!*data)
			return NULL;
		if (data[0] == '/' && data[1] == '/') {
			while (*data && *data != '\n')
				data++;
			continue;
		}
		if (data[0] == '/' && data[1] == '*') {
			data += 2;
			while (*data && !(data[0] == '*' && data[1] == '/'))
				data++;
			if (*data)
				data += 2;
			continue;
		}
		break;
	}
	if (*data == '"') {
		data++;
		while (*data && *data != '"') {
			if (len + 1 < tokensize)
				token[len++] = *data;
			data++;
		}
		if (*data == '"')
			data++;
		token[len] = 0;
		return data;
	}
	if (*data == '{' || *data == '}' || *data == '(' || *data == ')') {
		token[0] = *data;
		token[1] = 0;
		return data + 1;
	}
	while ((unsigned char)*data > ' ' && *data != '{' && *data != '}' && *data != '"') {
		if (len + 1 < tokensize)
			token[len++] = *data;
		data++;
	}
	token[len] = 0;
	return data;
}

void AngleVectors(const float *angles, float *forward, float *right, float *up)
{
	const float d2r = (float)(M_PI / 180.0);
	float sy = sinf(angles[1] * d2r), cy = cosf(angles[1] * d2r);
	float sp = sinf(angles[0] * d2r), cp = cosf(angles[0] * d2r);
	float sr = sinf(angles[2] * d2r), cr = cosf(angles[2] * d2r);
	if (forward) {
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}
	if (right) {
		right[0] = -sr * sp * cy + cr * sy;
		right[1] = -sr * sp * sy - cr * cy;
		right[2] = -sr * cp;
	}
	if (up) {
		up[0] = cr * sp * cy + sr * sy;
		up[1] = cr * sp * sy - sr * cy;
		up[2] = cr * cp;
	}
}

float VectorNormalize(float *v)
{
	float len = sqrtf(DotProduct(v, v));
	if (len) {
		float inv = 1.0f / len;
		v[0] *= inv;
		v[1] *= inv;
		v[2] *= inv;
	}
	return len;
}

const char *va(const char *fmt, ...)
{
	static char bufs[8][1024];
	static int which;
	char *b = bufs[which++ & 7];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(bufs[0]), fmt, ap);
	va_end(ap);
	return b;
}
