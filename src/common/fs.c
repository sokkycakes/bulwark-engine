/* Bulwark engine - virtual file system. ISC licence, see LICENSE. */
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define FS_MAX_PATHS 16

static char fs_basedir[1024] = ".";
static char fs_gamedirs[FS_MAX_PATHS][64];
static int fs_numgamedirs;

void FS_Init(const char *basedir)
{
	Str_Copy(fs_basedir, basedir && *basedir ? basedir : ".", sizeof(fs_basedir));
	fs_numgamedirs = 0;
}

const char *FS_BaseDir(void) { return fs_basedir; }

void FS_AddGameDir(const char *gamedir)
{
	for (int i = 0; i < fs_numgamedirs; i++)
		if (!strcmp(fs_gamedirs[i], gamedir))
			return;
	if (fs_numgamedirs >= FS_MAX_PATHS || !FS_SafeName(gamedir))
		return;
	Str_Copy(fs_gamedirs[fs_numgamedirs++], gamedir, sizeof(fs_gamedirs[0]));
}

const char *FS_GameDir(void) { return fs_numgamedirs ? fs_gamedirs[fs_numgamedirs - 1] : ""; }

bool FS_SafeName(const char *path)
{
	if (!path || !*path || path[0] == '/' || path[0] == '\\' || strchr(path, ':'))
		return false;
	for (const char *p = path; *p; p++) {
		if (*p == '\\')
			return false;
		if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') && (p[2] == '/' || !p[2]))
			return false;
	}
	return true;
}

static bool os_path(int dir, const char *path, char *out, size_t outsize)
{
	int n = snprintf(out, outsize, "%s/%s/%s", fs_basedir, fs_gamedirs[dir], path);
	return n > 0 && (size_t)n < outsize;
}

static char *load_os_file(const char *ospath, size_t *len)
{
	FILE *f = fopen(ospath, "rb");
	long size;
	char *buf;
	struct stat st;
	if (!f)
		return NULL;
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	buf = Mem_Alloc((size_t)size + 1);
	if (size && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		Mem_Free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[size] = 0;
	if (len)
		*len = (size_t)size;
	return buf;
}

char *FS_LoadFile(const char *path, size_t *len)
{
	char ospath[2048];
	if (!FS_SafeName(path))
		return NULL;
	for (int i = fs_numgamedirs - 1; i >= 0; i--) {
		char *buf;
		if (!os_path(i, path, ospath, sizeof(ospath)))
			continue;
		buf = load_os_file(ospath, len);
		if (buf)
			return buf;
	}
	return NULL;
}

const char *FS_WhichPath(const char *path)
{
	char ospath[2048];
	struct stat st;
	if (!FS_SafeName(path))
		return NULL;
	for (int i = fs_numgamedirs - 1; i >= 0; i--)
		if (os_path(i, path, ospath, sizeof(ospath)) && !stat(ospath, &st) && S_ISREG(st.st_mode))
			return fs_gamedirs[i];
	return NULL;
}

bool FS_FileExists(const char *path) { return FS_WhichPath(path) != NULL; }

bool FS_WritePath(const char *path, char *out, size_t outsize)
{
	if (!fs_numgamedirs || !FS_SafeName(path))
		return false;
	return os_path(fs_numgamedirs - 1, path, out, outsize);
}

bool FS_CreatePath(const char *ospath)
{
	char tmp[2048];
	Str_Copy(tmp, ospath, sizeof(tmp));
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) && errno != EEXIST)
				return false;
			*p = '/';
		}
	}
	return true;
}

typedef struct {
	char **names;
	int count, cap;
} namelist_t;

static bool namelist_has(namelist_t *l, const char *n)
{
	for (int i = 0; i < l->count; i++)
		if (!strcmp(l->names[i], n))
			return true;
	return false;
}

static void walk(const char *root, const char *rel, const char *pattern, bool nocase, namelist_t *seen,
                 const char *searchpath, fs_list_fn fn, void *ctx, int depth)
{
	char dirpath[2048];
	DIR *d;
	struct dirent *de;
	if (depth > 16)
		return;
	snprintf(dirpath, sizeof(dirpath), "%s/%s", root, rel);
	d = opendir(dirpath);
	if (!d)
		return;
	while ((de = readdir(d))) {
		char relname[1024], full[3072];
		struct stat st;
		if (de->d_name[0] == '.')
			continue;
		snprintf(relname, sizeof(relname), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
		snprintf(full, sizeof(full), "%s/%s", root, relname);
		if (stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			/* '*' also matches '/', so any subdirectory may hold matches
			 * once the pattern names a directory */
			if (strchr(pattern, '/'))
				walk(root, relname, pattern, nocase, seen, searchpath, fn, ctx, depth + 1);
		} else if (Str_WildMatch(pattern, relname, nocase) && !namelist_has(seen, relname)) {
			if (seen->count >= seen->cap) {
				seen->cap = seen->cap ? seen->cap * 2 : 64;
				seen->names = Mem_Realloc(seen->names, sizeof(char *) * seen->cap);
			}
			seen->names[seen->count++] = Mem_Strdup(relname);
			fn(relname, searchpath, ctx);
		}
	}
	closedir(d);
}

void FS_List(const char *pattern, bool nocase, fs_list_fn fn, void *ctx)
{
	namelist_t seen = {0};
	/* start the walk at the deepest directory that has no wildcard */
	char base[1024] = "";
	const char *slash = NULL;
	for (const char *p = pattern; *p && *p != '*' && *p != '?'; p++)
		if (*p == '/')
			slash = p;
	if (slash) {
		size_t n = (size_t)(slash - pattern);
		if (n >= sizeof(base))
			n = sizeof(base) - 1;
		memcpy(base, pattern, n);
		base[n] = 0;
	}
	if (!FS_SafeName(pattern))
		return;
	for (int i = fs_numgamedirs - 1; i >= 0; i--) {
		char root[1100];
		snprintf(root, sizeof(root), "%s/%s", fs_basedir, fs_gamedirs[i]);
		walk(root, base, pattern, nocase, &seen, fs_gamedirs[i], fn, ctx, 0);
	}
	for (int i = 0; i < seen.count; i++)
		Mem_Free(seen.names[i]);
	Mem_Free(seen.names);
}
