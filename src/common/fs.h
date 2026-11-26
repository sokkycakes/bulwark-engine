/* Bulwark engine - virtual file system. ISC licence, see LICENSE.
 * Search paths are game directories under the base directory; later game
 * directories override earlier ones. Each directory's .pk3 and .pak packages
 * are searched after its loose files, the alphabetically last first. */
#ifndef BW_FS_H
#define BW_FS_H

#include "common.h"

void FS_Init(const char *basedir);
void FS_AddGameDir(const char *gamedir);
const char *FS_GameDir(void); /* the writable (most recently added) game directory */
const char *FS_BaseDir(void);
/* Bulwark's own data (data/ in the source tree, or beside the executable),
 * searched after every game directory; NULL picks the default */
void FS_SetEngineDir(const char *path);
const char *FS_EngineDir(void);
void FS_Restart(void); /* rescan packages */
/* changes whenever the search path does (game directories, packages), so
   results derived from it can tell they are stale */
int FS_Generation(void);
int FS_NumGameDirs(void);
const char *FS_GameDirAt(int i); /* 0 is the base game, searched last */
void FS_SetGameDirs(const char *const *dirs, int count);

/* returns a NUL-terminated Mem_Alloc'd buffer, or NULL */
char *FS_LoadFile(const char *path, size_t *len);
/* the calling thread's FS_LoadFile calls and the seconds they took since
 * the last call, for host_speeds' slowest frame */
void FS_TakeLoadStats(int *loads, double *seconds);
bool FS_FileExists(const char *path);
/* name of the search path that provides path, or NULL */
const char *FS_WhichPath(const char *path);
/* a file may have been created, written or removed: forgets the lookups
 * the Xbox keeps (called by its fopen for writing, remove and rename) */
void FS_NoteWrite(void);
/* index of the game directory that provides path (0 is the base game), or
 * -1 if none does */
int FS_WhichGameDir(const char *path);

/* full OS path for writing path inside the writable game directory; false if
 * the name is unsafe (absolute, contains "..", etc) */
bool FS_WritePath(const char *path, char *out, size_t outsize);
bool FS_CreatePath(const char *ospath);

/* calls fn for each distinct file matching the wildcard pattern (for example every .bsp under maps/) */
typedef void (*fs_list_fn)(const char *name, const char *searchpath, void *ctx);
void FS_List(const char *pattern, bool nocase, fs_list_fn fn, void *ctx);

bool FS_SafeName(const char *path);

/* the search path, newest first, with each game dir's packages */
void FS_PrintPath(void);

#endif
