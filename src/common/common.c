/* Bulwark engine - shared helpers. ISC licence, see LICENSE. */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#ifdef __3DS__
void CTR_PrintMemory(const char *when);
#endif
#ifdef BW_XBOX
void XBX_Log(const char *text);
void XBX_NoteFreeMemory(const void *from);
#endif

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
#ifdef BW_XBOX
	XBX_Log(text); /* no stdout: the log file and the kernel debugger */
#else
	fputs(text, stdout);
	fflush(stdout);
#endif
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
#ifdef __3DS__
	CTR_PrintMemory("at the error");
#endif
#ifdef BW_XBOX
	XBX_Log("FATAL: ");
	XBX_Log(buf);
	XBX_Log("\n");
	XBX_PrintMemory("at the error");
	XBX_FatalScreen(buf);
#endif
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

#ifdef BW_MEMTRACE
/* BW_MEMTRACE builds remember every live allocation of 1 KB or more and who
 * made it; "memtrace_live" sums them by caller. Debug only: no lock. */
#define TRACE_SLOTS (1 << 17)
typedef struct {
	void *p, *from;
	size_t n;
} trace_t;
static trace_t *trace;

static void trace_add(void *p, size_t n, void *from)
{
	if (!p || n < 1024)
		return;
	if (!trace && !(trace = calloc(TRACE_SLOTS, sizeof(*trace))))
		return;
	for (size_t i = ((uintptr_t)p >> 4) & (TRACE_SLOTS - 1), k = 0; k < TRACE_SLOTS; k++, i = (i + 1) & (TRACE_SLOTS - 1))
		if (!trace[i].p || trace[i].p == (void *)1) {
			trace[i] = (trace_t){p, from, n};
			return;
		}
}

static void trace_remove(void *p)
{
	if (!p || !trace)
		return;
	for (size_t i = ((uintptr_t)p >> 4) & (TRACE_SLOTS - 1), k = 0; k < TRACE_SLOTS; k++, i = (i + 1) & (TRACE_SLOTS - 1)) {
		if (!trace[i].p)
			return;
		if (trace[i].p == p) {
			trace[i].p = (void *)1; /* a tombstone keeps later probes going */
			trace[i].n = 0;
			return;
		}
	}
}

void Mem_TraceReport(void)
{
	enum { MAXC = 512 };
	static void *from[MAXC];
	static size_t bytes[MAXC], count[MAXC];
	int nc = 0;
	size_t total = 0;
	if (!trace)
		return;
	for (size_t i = 0; i < TRACE_SLOTS; i++) {
		if ((uintptr_t)trace[i].p <= 1)
			continue;
		int c;
		for (c = 0; c < nc && from[c] != trace[i].from; c++)
			;
		if (c == nc) {
			if (nc == MAXC)
				continue;
			from[nc] = trace[i].from, bytes[nc] = count[nc] = 0, nc++;
		}
		bytes[c] += trace[i].n, count[c]++;
		total += trace[i].n;
	}
	for (int done = 0; done < 60 && done < nc; done++) {
		int best = done;
		for (int c = done + 1; c < nc; c++)
			if (bytes[c] > bytes[best])
				best = c;
		void *f = from[best];
		size_t b = bytes[best], n = count[best];
		from[best] = from[done], bytes[best] = bytes[done], count[best] = count[done];
		from[done] = f, bytes[done] = b, count[done] = n;
		/* the offset from Mem_Alloc places the caller in a relocated binary */
		Con_Printf("memlive %7u KB %5u from %p (Mem_Alloc%+ld)\n", (unsigned)(b >> 10), (unsigned)n, f,
		           (long)((char *)f - (char *)(void *)Mem_Alloc));
	}
	Con_Printf("memlive total %u KB in blocks of 1 KB or more\n", (unsigned)(total >> 10));
}
#endif

void *Mem_Alloc(size_t size)
{
	void *p = calloc(1, size ? size : 1);
#ifdef BW_MEMTRACE
	trace_add(p, size, __builtin_return_address(0));
#endif
#ifdef BW_XBOX
	if (size >= (64u << 10))
		XBX_NoteFreeMemory(__builtin_return_address(0));
#endif
	if (!p)
		Sys_Error("out of memory allocating %zu bytes (from %p)", size, __builtin_return_address(0));
	return p;
}

void *Mem_Realloc(void *p, size_t size)
{
#ifdef BW_MEMTRACE
	trace_remove(p); /* a failed realloc is fatal below anyway */
#endif
	void *n = realloc(p, size ? size : 1);
#ifdef BW_MEMTRACE
	trace_add(n, size, __builtin_return_address(0));
#endif
#ifdef BW_XBOX
	if (size >= (64u << 10))
		XBX_NoteFreeMemory(__builtin_return_address(0));
#endif
	if (!n)
		Sys_Error("out of memory reallocating %zu bytes", size);
	return n;
}

void Mem_Free(void *p)
{
#ifdef BW_MEMTRACE
	trace_remove(p);
#endif
	free(p);
}

#ifndef BW_XBOX
void *Mem_PageAlloc(size_t size, size_t commit) { return (void)commit, Mem_Alloc(size); }
void Mem_PageFree(void *p) { Mem_Free(p); }
bool Mem_PageCommit(void *p, size_t size) { return (void)p, (void)size, true; }
void Mem_PageDecommit(void *p, size_t size) { (void)p, (void)size; }
#endif

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

/* tolower as the C locale the engine runs in has it; folding only on a
 * mismatch keeps the equal-byte path free of calls */
static inline int ascii_lower(int c) { return (unsigned)c - 'A' < 26u ? c + ('a' - 'A') : c; }

int Str_ICmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca != cb) {
			ca = ascii_lower(ca);
			cb = ascii_lower(cb);
			if (ca != cb)
				return ca - cb;
		}
		if (!ca)
			return 0;
	}
}

int Str_NICmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca != cb) {
			ca = ascii_lower(ca);
			cb = ascii_lower(cb);
			if (ca != cb)
				return ca - cb;
		}
		if (!ca)
			return 0;
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

/* the sine and cosine of a in degrees; a zero angle (most rolls and many
 * pitches) gives what sinf and cosf give for it, without the calls */
static inline void sincos_deg(float a, float *s, float *c)
{
	float r = a * (float)(M_PI / 180.0);
	if (r == 0) {
		*s = r; /* keeps the sign of zero, as sinf does */
		*c = 1;
	} else {
		*s = sinf(r);
		*c = cosf(r);
	}
}

void AngleVectors(const float *angles, float *forward, float *right, float *up)
{
	float sy, cy, sp, cp, sr, cr;
	sincos_deg(angles[1], &sy, &cy);
	sincos_deg(angles[0], &sp, &cp);
	sincos_deg(angles[2], &sr, &cr);
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
