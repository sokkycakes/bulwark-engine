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
#define BW_CLAMP(v, lo, hi) BW_MIN(BW_MAX((v), (lo)), (hi))

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
/* a second receiver of all output, for the console log file */
void Con_SetLogHook(con_hook_fn fn);
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
/* Escaped strings: \"text" with \n \t \r \" and \\ inside, the form
 * sprintf's %S uses for text that a plain quoted string can't hold.
 * Str_NeedsEscape says whether s needs it; Str_ParseEscaped reads one at *p
 * (which points at the backslash) into out, returns its length and moves *p
 * past the closing quote. */
bool Str_NeedsEscape(const char *s);
size_t Str_ParseEscaped(const char **p, char *out, size_t outsize);

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
/* while set, console output is also collected into sb (rcon replies) */
void Con_Redirect(strbuf_t *sb);

/* tokeniser shared by the console and the entity parser: handles quotes and
 * // comments, and returns the next token or NULL at end of input. */
const char *COM_Parse(const char *data, char *token, size_t tokensize);

static inline void VectorCopy(const float *a, float *b) { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; }
static inline void VectorClear(float *a) { a[0] = a[1] = a[2] = 0; }
static inline float DotProduct(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float VectorLength(const float *a) { return sqrtf(DotProduct(a, a)); }
static inline void CrossProduct(const float *a, const float *b, float *c)
{
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
}
void AngleVectors(const float *angles, float *forward, float *right, float *up);
float VectorNormalize(float *v);

#endif
