/* Bulwark engine - shared types and helpers. ISC licence, see LICENSE. */
#ifndef BW_COMMON_H
#define BW_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

typedef float vec3_t[3];

#define BW_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define BW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BW_MAX(a, b) ((a) > (b) ? (a) : (b))

#if defined(__GNUC__) || defined(__clang__)
#define BW_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define BW_NORETURN __attribute__((noreturn))
#else
#define BW_PRINTF(f, a)
#define BW_NORETURN
#endif

/* console output; everything goes through here so a client console can hook it */
typedef void (*con_hook_fn)(const char *text);
void Con_SetHook(con_hook_fn fn);

/* Loading progress. The server and client report each step of bringing up a
 * map; the client shows it (a menu's loading screen) and clears it with stage
 * NULL once in game. frac is 0-1 within the whole load, measured from counted
 * items, not guessed from time. Without a hook the reports go nowhere. */
typedef void (*loading_hook_fn)(const char *stage, const char *map, float frac);
void Host_SetLoadingHook(loading_hook_fn fn);
void Host_Loading(const char *stage, const char *map, float frac);
void Con_Printf(const char *fmt, ...) BW_PRINTF(1, 2);
void Con_DPrintf(const char *fmt, ...) BW_PRINTF(1, 2);
void Con_Print(const char *text);
extern int developer;

BW_NORETURN void Sys_Error(const char *fmt, ...) BW_PRINTF(1, 2);
double Sys_Milliseconds(void);
double Sys_Seconds(void);
/* the host clock: advanced by the main loop each frame, so scripted runs that
 * step frames faster than real time still see consistent timing */
extern double host_time;
void Sys_Sleep(double seconds);

void *Mem_Alloc(size_t size);        /* zeroed, aborts on failure */
void *Mem_Realloc(void *p, size_t size);
void Mem_Free(void *p);
char *Mem_Strdup(const char *s);

/* bounded string helpers */
size_t Str_Copy(char *dst, const char *src, size_t dstsize);
size_t Str_Cat(char *dst, const char *src, size_t dstsize);
int Str_ICmp(const char *a, const char *b);
/* printf into one of a few rotating static buffers, for short-lived strings */
const char *va(const char *fmt, ...) BW_PRINTF(1, 2);
int Str_NICmp(const char *a, const char *b, size_t n);
bool Str_WildMatch(const char *pattern, const char *s, bool nocase);

/* growable byte buffer */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} strbuf_t;
void SB_Append(strbuf_t *sb, const char *s, size_t n);
void SB_AppendStr(strbuf_t *sb, const char *s);
void SB_Printf(strbuf_t *sb, const char *fmt, ...) BW_PRINTF(2, 3);
void SB_Free(strbuf_t *sb);

/* tokeniser shared by the console and the entity parser: handles quotes and
 * // comments, and returns the next token or NULL at end of input. */
const char *COM_Parse(const char *data, char *token, size_t tokensize);

static inline void VectorCopy(const float *a, float *b) { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; }
static inline void VectorClear(float *a) { a[0] = a[1] = a[2] = 0; }
static inline float DotProduct(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float VectorLength(const float *a) { return sqrtf(DotProduct(a, a)); }
static inline void VectorSubtract(const float *a, const float *b, float *c)
{
	c[0] = a[0] - b[0];
	c[1] = a[1] - b[1];
	c[2] = a[2] - b[2];
}
static inline void VectorScale(const float *a, float s, float *b)
{
	b[0] = a[0] * s;
	b[1] = a[1] * s;
	b[2] = a[2] * s;
}
/* c = a + b * s */
static inline void VectorMA(const float *a, float s, const float *b, float *c)
{
	c[0] = a[0] + b[0] * s;
	c[1] = a[1] + b[1] * s;
	c[2] = a[2] + b[2] * s;
}
static inline void CrossProduct(const float *a, const float *b, float *c)
{
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
}
void AngleVectors(const float *angles, float *forward, float *right, float *up);
float VectorNormalize(float *v);

#endif
