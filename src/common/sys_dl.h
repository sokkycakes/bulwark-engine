/* Bulwark engine - dynamic loading, or its absence. ISC licence, see LICENSE.
 *
 * Plugins and the optional system libraries (libcurl, sqlite, opus) are
 * loaded with dlopen where the platform has it. The 3DS and DS have no
 * dynamic linker: there every dlopen fails with a message saying so, and
 * the callers take the paths they already have for a missing library. */
#ifndef BW_SYS_DL_H
#define BW_SYS_DL_H

#if defined(__3DS__) || defined(__NDS__) || defined(BW_XBOX)
#define BW_NO_DLOPEN 1
#endif

#ifndef BW_NO_DLOPEN
#include <dlfcn.h>
#else
#include <stddef.h>
#define RTLD_NOW 0
#define RTLD_LOCAL 0
static inline void *bw_dlopen(const char *path, int mode) { (void)path, (void)mode; return NULL; }
static inline void *bw_dlsym(void *h, const char *name) { (void)h, (void)name; return NULL; }
static inline int bw_dlclose(void *h) { (void)h; return 0; }
static inline const char *bw_dlerror(void) { return "no dynamic loading on this platform"; }
#define dlopen bw_dlopen
#define dlsym bw_dlsym
#define dlclose bw_dlclose
#define dlerror bw_dlerror
#endif

#endif
