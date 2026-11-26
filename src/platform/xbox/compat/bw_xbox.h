/* Bulwark engine - included before every C file of the Xbox build (see
 * xbox.cmake). ISC licence, see LICENSE.
 *
 * nxdk gives a C library (PDCLib) and a slice of the Win32 API, but not
 * POSIX. The engine is written against POSIX, so this header and the ones
 * beside it fill the gaps the engine uses, implemented in posix_xbox.c:
 *
 * - File names: the kernel takes DOS-style names ("D:\maps\x.bsp"); the
 *   engine builds them with '/'. fopen, remove, rename, stat, mkdir and
 *   opendir go through bwx_ospath(), which turns the slashes around and
 *   resolves relative names against the base directory.
 * - Threads (pthread.h), directories (dirent.h), file status (sys/stat.h),
 *   clocks (clock_gettime, usleep) and a few string functions.
 * - Sockets come from lwIP's BSD API (sys/socket.h and friends here). */
#ifndef BW_XBOX_COMPAT_H
#define BW_XBOX_COMPAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <math.h>

/* nxdk's own unistd.h is empty and sits earlier on the include path than
 * compat/: take ours by name */
#include "sys/types.h"
#include "unistd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a name the kernel understands, in out (at least 260 bytes); returns out */
char *bwx_ospath(const char *path, char *out, size_t size);

FILE *bwx_fopen(const char *path, const char *mode);
int bwx_remove(const char *path);
int bwx_rename(const char *from, const char *to);
#ifndef __cplusplus
#define fopen(p, m) bwx_fopen(p, m)
#define remove(p) bwx_remove(p)
#define rename(a, b) bwx_rename(a, b)
#else
/* C++ (PanoramaEngine): std::remove and std::filesystem::rename are other
 * things, so only fopen is renamed, as a name, so std::fopen follows it
 * through libc++'s using-declarations */
#define fopen bwx_fopen
#endif

/* PDCLib's FILE has no descriptor; fileno hands back the FILE itself as a
 * number, which fstat (sys/stat.h) understands */
int bwx_fileno(FILE *f);
#define fileno bwx_fileno

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
typedef int clockid_t;
int clock_gettime(clockid_t id, struct timespec *ts);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_SQRT2 1.41421356237309504880
#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#endif

int _stricmp(const char *s1, const char *s2);
int _strnicmp(const char *s1, const char *s2, size_t n);
#define strcasecmp _stricmp
#define strncasecmp _strnicmp

int nanosleep(const struct timespec *req, struct timespec *rem);
ssize_t readlink(const char *path, char *buf, size_t size);

char *strndup(const char *s, size_t n);
size_t strnlen(const char *s, size_t n);
char *strtok_r(char *s, const char *delim, char **save);
char *strsep(char **sp, const char *delim);
char *strcasestr(const char *hay, const char *needle);
int setenv(const char *name, const char *value, int overwrite);

/* PDCLib prints and scans no floating point and its strtod stops the
 * program: fmt_xbox.c has the whole families */
int bwx_snprintf(char *s, size_t n, const char *f, ...);
int bwx_vsnprintf(char *s, size_t n, const char *f, va_list ap);
int bwx_sprintf(char *s, const char *f, ...);
int bwx_vsprintf(char *s, const char *f, va_list ap);
int bwx_fprintf(FILE *fp, const char *f, ...);
int bwx_vfprintf(FILE *fp, const char *f, va_list ap);
int bwx_printf(const char *f, ...);
int bwx_vprintf(const char *f, va_list ap);
int bwx_sscanf(const char *s, const char *f, ...);
int bwx_vsscanf(const char *s, const char *f, va_list ap);
double bwx_strtod(const char *s, char **end);
float bwx_strtof(const char *s, char **end);
long double bwx_strtold(const char *s, char **end);
double bwx_atof(const char *s);
#define snprintf bwx_snprintf
#define vsnprintf bwx_vsnprintf
#define sprintf bwx_sprintf
#define vsprintf bwx_vsprintf
#define fprintf bwx_fprintf
#define vfprintf bwx_vfprintf
#define printf bwx_printf
#define vprintf bwx_vprintf
#define sscanf bwx_sscanf
#define vsscanf bwx_vsscanf
#define strtod bwx_strtod
#define strtof bwx_strtof
#define strtold bwx_strtold
#define atof bwx_atof

/* and the maths functions it leaves as stubs that stop the program
 * (math_xbox.c) */
long bwx_lrint(double x);
long bwx_lrintf(float x);
long long bwx_llrint(double x);
long long bwx_llrintf(float x);
long bwx_lround(double x);
long bwx_lroundf(float x);
long long bwx_llround(double x);
long long bwx_llroundf(float x);
double bwx_log2(double x);
float bwx_log2f(float x);
#define lrint bwx_lrint
#define lrintf bwx_lrintf
#define llrint bwx_llrint
#define llrintf bwx_llrintf
#define lround bwx_lround
#define lroundf bwx_lroundf
#define llround bwx_llround
#define llroundf bwx_llroundf
#define log2 bwx_log2
#define log2f bwx_log2f
double bwx_scalbn(double x, int n);
float bwx_scalbnf(float x, int n);
double bwx_cbrt(double x);
double bwx_log1p(double x);
double bwx_expm1(double x);
double bwx_asinh(double x);
double bwx_acosh(double x);
double bwx_atanh(double x);
#define scalbn bwx_scalbn
#define scalbnf bwx_scalbnf
#define cbrt bwx_cbrt
#define log1p bwx_log1p
#define expm1 bwx_expm1
#define asinh bwx_asinh
#define acosh bwx_acosh
#define atanh bwx_atanh

/* the engine's own diagnostic hooks for the Xbox (sys_xbox.c) */
void XBX_PrintMemory(const char *when);
void XBX_Log(const char *text);
void XBX_FatalScreen(const char *text);
size_t XBX_FreeMemory(void);
size_t XBX_TotalMemory(void);

#ifdef __cplusplus
}
#endif

/* nxdk-cc compiles freestanding, which turns the compiler's builtins off
 * whatever -fbuiltin says. Named as builtins, a small fixed-size memcpy is
 * a few moves and sqrtf one sqrtss instead of a call (to an x87 fsqrt:
 * both round correctly, so the results are the same); anything else is
 * still a call to string_xbox.c's functions, which undefine these. */
#ifndef __cplusplus
#define memcpy(d, s, n) __builtin_memcpy(d, s, n)
#define memset(d, c, n) __builtin_memset(d, c, n)
#define memcmp(a, b, n) __builtin_memcmp(a, b, n)
#define strlen(s) __builtin_strlen(s)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabsf(x) __builtin_fabsf(x)
#define fabs(x) __builtin_fabs(x)
#endif

#endif
