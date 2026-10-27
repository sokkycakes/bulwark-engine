/* Bulwark engine - virtual file system: game directories and the packages
 * in them (.pk3 zip archives, Quake .pak files). Zip layout from PKWARE's
 * APPNOTE; inflate by stb_image (public domain). ISC licence, see LICENSE. */
#include "fs.h"
#include "../thirdparty/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define FS_MAX_PATHS 16

static char fs_basedir[1024] = ".";
/* Bulwark's own files (UI layouts and the like), searched after every game
 * directory: loose files only, see FS_SetEngineDir */
static char fs_enginedir[1024];
static char fs_gamedirs[FS_MAX_PATHS][64];
static int fs_numgamedirs;
static int fs_generation; /* bumped whenever the search path changes */

/* --- packages ---------------------------------------------------------------- */

typedef struct {
	char *name;
	uint32_t offset;      /* of the local header (zip) or the data (pak) */
	uint32_t csize, size;
	uint16_t method;      /* 0 stored, 8 deflated */
	int next;             /* hash chain */
} packfile_t;

typedef struct {
	char path[1400];      /* OS path of the archive */
	char name[128];       /* as shown: gamedir/file.pk3 */
	int gamedir;
	bool zip;
	packfile_t *files;
	int numfiles;
	int *hash;
	int hashsize;
} pack_t;

#define FS_MAX_PACKS 256
static pack_t *packs[FS_MAX_PACKS]; /* in search order within each game dir: later wins */
static int numpacks;

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32u(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static unsigned name_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
	return h;
}

static void pack_add(pack_t *pk, const char *name, uint32_t offset, uint32_t csize, uint32_t size, uint16_t method)
{
	packfile_t *f;
	if (!*name || name[strlen(name) - 1] == '/')
		return;
	if (pk->numfiles % 256 == 0)
		pk->files = Mem_Realloc(pk->files, sizeof(packfile_t) * (pk->numfiles + 256));
	f = &pk->files[pk->numfiles++];
	f->name = Mem_Strdup(name);
	for (char *c = f->name; *c; c++)
		if (*c == '\\')
			*c = '/';
	f->offset = offset;
	f->csize = csize;
	f->size = size;
	f->method = method;
}

static void pack_index(pack_t *pk)
{
	pk->hashsize = 64;
	while (pk->hashsize < pk->numfiles * 2)
		pk->hashsize *= 2;
	pk->hash = Mem_Alloc(sizeof(int) * pk->hashsize);
	for (int i = 0; i < pk->hashsize; i++)
		pk->hash[i] = -1;
	/* later entries with the same name win, as in the archive's own order */
	for (int i = 0; i < pk->numfiles; i++) {
		unsigned h = name_hash(pk->files[i].name) & (pk->hashsize - 1);
		pk->files[i].next = pk->hash[h];
		pk->hash[h] = i;
	}
}

static packfile_t *pack_find(pack_t *pk, const char *name)
{
	for (int i = pk->hash[name_hash(name) & (pk->hashsize - 1)]; i >= 0; i = pk->files[i].next)
		if (!Str_ICmp(pk->files[i].name, name))
			return &pk->files[i];
	return NULL;
}

/* zip: the end-of-central-directory record, then the central directory */
static bool open_zip(pack_t *pk, FILE *f)
{
	uint8_t tail[65536 + 22], *eocd = NULL;
	long size, start;
	size_t n;
	uint32_t count, cdsize, cdofs;
	uint8_t *cd;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	start = size > (long)sizeof(tail) ? size - (long)sizeof(tail) : 0;
	fseek(f, start, SEEK_SET);
	n = fread(tail, 1, (size_t)(size - start), f);
	for (long i = (long)n - 22; i >= 0; i--)
		if (rd32u(tail + i) == 0x06054b50) {
			eocd = tail + i;
			break;
		}
	if (!eocd)
		return false;
	count = rd16(eocd + 10);
	cdsize = rd32u(eocd + 12);
	cdofs = rd32u(eocd + 16);
	if ((long)cdofs + (long)cdsize > size)
		return false;
	cd = Mem_Alloc(cdsize + 1);
	fseek(f, (long)cdofs, SEEK_SET);
	if (fread(cd, 1, cdsize, f) != cdsize) {
		Mem_Free(cd);
		return false;
	}
	for (uint32_t i = 0, p = 0; i < count && p + 46 <= cdsize; i++) {
		uint8_t *e = cd + p;
		uint32_t nlen = rd16(e + 28), xlen = rd16(e + 30), clen = rd16(e + 32);
		char name[256];
		if (rd32u(e) != 0x02014b50 || p + 46 + nlen > cdsize)
			break;
		if (nlen < sizeof(name) && !(rd16(e + 8) & 1)) { /* skip encrypted entries */
			memcpy(name, e + 46, nlen);
			name[nlen] = 0;
			pack_add(pk, name, rd32u(e + 42), rd32u(e + 20), rd32u(e + 24), (uint16_t)rd16(e + 10));
		}
		p += 46 + nlen + xlen + clen;
	}
	Mem_Free(cd);
	pk->zip = true;
	return true;
}

/* Quake .pak: "PACK", directory offset and length; 64-byte entries */
static bool open_pak(pack_t *pk, FILE *f)
{
	uint8_t h[12], *dir;
	uint32_t ofs, len;
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "PACK", 4))
		return false;
	ofs = rd32u(h + 4);
	len = rd32u(h + 8);
	if (len % 64 || len > (64u << 20))
		return false;
	dir = Mem_Alloc(len + 1);
	fseek(f, (long)ofs, SEEK_SET);
	if (fread(dir, 1, len, f) != len) {
		Mem_Free(dir);
		return false;
	}
	for (uint32_t i = 0; i < len / 64; i++) {
		char name[57];
		memcpy(name, dir + i * 64, 56);
		name[56] = 0;
		pack_add(pk, name, rd32u(dir + i * 64 + 56), rd32u(dir + i * 64 + 60), rd32u(dir + i * 64 + 60), 0);
	}
	Mem_Free(dir);
	return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* every .pak and .pk3 in a game directory, alphabetically: pak0 before pak1 */
static void add_packs(int gamedir)
{
	char dirpath[1100];
	char *names[512];
	int n = 0;
	DIR *d;
	struct dirent *de;
	snprintf(dirpath, sizeof(dirpath), "%s/%s", fs_basedir, fs_gamedirs[gamedir]);
	if (!(d = opendir(dirpath)))
		return;
	while ((de = readdir(d)) && n < (int)BW_ARRAY_COUNT(names)) {
		size_t l = strlen(de->d_name);
		if (l > 4 && (!Str_ICmp(de->d_name + l - 4, ".pk3") || !Str_ICmp(de->d_name + l - 4, ".pak")))
			names[n++] = Mem_Strdup(de->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof(char *), cmp_names);
	for (int i = 0; i < n; i++) {
		pack_t *pk = Mem_Alloc(sizeof(*pk));
		FILE *f;
		bool ok = false;
		snprintf(pk->path, sizeof(pk->path), "%s/%s", dirpath, names[i]);
		snprintf(pk->name, sizeof(pk->name), "%s/%s", fs_gamedirs[gamedir], names[i]);
		pk->gamedir = gamedir;
		if ((f = fopen(pk->path, "rb"))) {
			size_t l = strlen(names[i]);
			ok = !Str_ICmp(names[i] + l - 4, ".pak") ? open_pak(pk, f) : open_zip(pk, f);
			fclose(f);
		}
		if (ok && numpacks < FS_MAX_PACKS) {
			pack_index(pk);
			packs[numpacks++] = pk;
			Con_DPrintf("added %s (%d files)\n", pk->name, pk->numfiles);
		} else {
			Con_Printf("^3%s: not a usable package\n", pk->name);
			for (int k = 0; k < pk->numfiles; k++)
				Mem_Free(pk->files[k].name);
			Mem_Free(pk->files);
			Mem_Free(pk);
		}
		Mem_Free(names[i]);
	}
}

static char *pack_read(pack_t *pk, packfile_t *pf, size_t *len)
{
	FILE *f = fopen(pk->path, "rb");
	uint8_t lh[30];
	uint32_t ofs = pf->offset;
	char *out = NULL;
	if (!f)
		return NULL;
	if (pk->zip) {
		/* the local header's name and extra lengths may differ from the
		 * central directory's */
		fseek(f, (long)ofs, SEEK_SET);
		if (fread(lh, 1, 30, f) != 30 || rd32u(lh) != 0x04034b50)
			goto done;
		ofs += 30 + rd16(lh + 26) + rd16(lh + 28);
	}
	fseek(f, (long)ofs, SEEK_SET);
	out = Mem_Alloc((size_t)pf->size + 1);
	if (pf->method == 0) {
		if (pf->csize != pf->size || fread(out, 1, pf->size, f) != pf->size)
			Mem_Free(out), out = NULL;
	} else if (pf->method == 8) {
		char *comp = Mem_Alloc((size_t)pf->csize + 1);
		if (fread(comp, 1, pf->csize, f) != pf->csize ||
		    stbi_zlib_decode_noheader_buffer(out, (int)pf->size, comp, (int)pf->csize) != (int)pf->size)
			Mem_Free(out), out = NULL;
		Mem_Free(comp);
	} else {
		Con_Printf("^3%s in %s: compression method %u is not supported\n", pf->name, pk->name, pf->method);
		Mem_Free(out), out = NULL;
	}
	if (out) {
		out[pf->size] = 0;
		if (len)
			*len = pf->size;
	}
done:
	fclose(f);
	return out;
}

/* the newest package of game dir `dir` holding path */
static packfile_t *find_in_packs(int dir, const char *path, pack_t **pk)
{
	for (int i = numpacks - 1; i >= 0; i--) {
		packfile_t *pf;
		if (packs[i]->gamedir != dir)
			continue;
		if ((pf = pack_find(packs[i], path))) {
			*pk = packs[i];
			return pf;
		}
	}
	return NULL;
}

static void free_packs(void)
{
	for (int i = 0; i < numpacks; i++) {
		for (int k = 0; k < packs[i]->numfiles; k++)
			Mem_Free(packs[i]->files[k].name);
		Mem_Free(packs[i]->files);
		Mem_Free(packs[i]->hash);
		Mem_Free(packs[i]);
	}
	numpacks = 0;
}

void FS_PrintPath(void)
{
	for (int i = fs_numgamedirs - 1; i >= 0; i--) {
		Con_Printf("%s/%s\n", fs_basedir, fs_gamedirs[i]);
		for (int k = numpacks - 1; k >= 0; k--)
			if (packs[k]->gamedir == i)
				Con_Printf("  %s (%d files)\n", packs[k]->name, packs[k]->numfiles);
	}
	if (fs_enginedir[0])
		Con_Printf("%s (engine data)\n", fs_enginedir);
}

void FS_Init(const char *basedir)
{
	Str_Copy(fs_basedir, basedir && *basedir ? basedir : ".", sizeof(fs_basedir));
	fs_numgamedirs = 0;
	if (!fs_enginedir[0])
		FS_SetEngineDir(NULL);
	free_packs();
	fs_generation++;
}

const char *FS_BaseDir(void) { return fs_basedir; }

static bool is_dir(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* an explicit directory, or the data directory beside the executable, or
 * the source tree's when running from a build directory inside it */
void FS_SetEngineDir(const char *path)
{
	char exe[1024], try[1100];
	ssize_t n;
	fs_enginedir[0] = 0;
	if (path && *path) {
		if (is_dir(path))
			Str_Copy(fs_enginedir, path, sizeof(fs_enginedir));
		else
			Con_Printf("engine data directory %s doesn't exist\n", path);
		return;
	}
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	exe[n > 0 ? n : 0] = 0;
	if (strrchr(exe, '/'))
		*strrchr(exe, '/') = 0;
	else
		Str_Copy(exe, ".", sizeof(exe));
	snprintf(try, sizeof(try), "%s/data", exe);
	if (!is_dir(try))
		snprintf(try, sizeof(try), "%s/../data", exe);
#ifdef BW_SOURCE_DATADIR
	if (!is_dir(try))
		Str_Copy(try, BW_SOURCE_DATADIR, sizeof(try));
#endif
	if (is_dir(try))
		Str_Copy(fs_enginedir, try, sizeof(fs_enginedir));
}

const char *FS_EngineDir(void) { return fs_enginedir; }

void FS_AddGameDir(const char *gamedir)
{
	for (int i = 0; i < fs_numgamedirs; i++)
		if (!strcmp(fs_gamedirs[i], gamedir))
			return;
	if (fs_numgamedirs >= FS_MAX_PATHS || !FS_SafeName(gamedir))
		return;
	Str_Copy(fs_gamedirs[fs_numgamedirs++], gamedir, sizeof(fs_gamedirs[0]));
	add_packs(fs_numgamedirs - 1);
	fs_generation++;
}

/* rescans the packages of every game directory, for files added while running */
void FS_Restart(void)
{
	free_packs();
	for (int i = 0; i < fs_numgamedirs; i++)
		add_packs(i);
	fs_generation++;
}

int FS_Generation(void) { return fs_generation; }

int FS_NumGameDirs(void) { return fs_numgamedirs; }
const char *FS_GameDirAt(int i) { return i >= 0 && i < fs_numgamedirs ? fs_gamedirs[i] : NULL; }

/* replaces the game directories (first is searched last) and rescans */
void FS_SetGameDirs(const char *const *dirs, int count)
{
	free_packs();
	fs_numgamedirs = 0;
	fs_generation++;
	for (int i = 0; i < count; i++)
		FS_AddGameDir(dirs[i]);
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

/* GoldSrc content names files in any case ("resource/Quest/QuestClosed")
 * and was made on a case-insensitive file system. When the name as given
 * isn't there, the all-lowercase name is tried, which is how extracted
 * archives are laid out. */
static bool os_path_lower(int dir, const char *path, char *out, size_t outsize)
{
	size_t base;
	if (!os_path(dir, "", out, outsize))
		return false;
	base = strlen(out);
	if (base + strlen(path) + 1 > outsize)
		return false;
	for (size_t i = 0; path[i]; i++)
		out[base + i] = (char)(path[i] >= 'A' && path[i] <= 'Z' ? path[i] + 32 : path[i]);
	out[base + strlen(path)] = 0;
	return strcmp(out + base, path) != 0; /* nothing new to try otherwise */
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
		if (!buf && os_path_lower(i, path, ospath, sizeof(ospath)))
			buf = load_os_file(ospath, len);
		if (buf)
			return buf;
		/* loose files override the game dir's own packages, which makes
		 * trying out a changed file easy */
		{
			pack_t *pk;
			packfile_t *pf = find_in_packs(i, path, &pk);
			if (pf && (buf = pack_read(pk, pf, len)))
				return buf;
		}
	}
	if (fs_enginedir[0]) {
		int n = snprintf(ospath, sizeof(ospath), "%s/%s", fs_enginedir, path);
		if (n > 0 && (size_t)n < sizeof(ospath))
			return load_os_file(ospath, len);
	}
	return NULL;
}

const char *FS_WhichPath(const char *path)
{
	char ospath[2048];
	struct stat st;
	if (!FS_SafeName(path))
		return NULL;
	for (int i = fs_numgamedirs - 1; i >= 0; i--) {
		pack_t *pk;
		if (os_path(i, path, ospath, sizeof(ospath)) && !stat(ospath, &st) && S_ISREG(st.st_mode))
			return fs_gamedirs[i];
		if (os_path_lower(i, path, ospath, sizeof(ospath)) && !stat(ospath, &st) && S_ISREG(st.st_mode))
			return fs_gamedirs[i];
		if (find_in_packs(i, path, &pk))
			return pk->name;
	}
	if (fs_enginedir[0]) {
		int n = snprintf(ospath, sizeof(ospath), "%s/%s", fs_enginedir, path);
		if (n > 0 && (size_t)n < sizeof(ospath) && !stat(ospath, &st) && S_ISREG(st.st_mode))
			return "(engine)";
	}
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

/* a file name against a search pattern: '*' and '?' stay within one path
 * component. Nuclide lists subfolders with patterns of their own (maps in
 * "maps/" and one level below), and Stiletto's decls/def/heroes.def
 * includes its subfolder because the decls/def search doesn't reach it. */
static bool path_match(const char *p, const char *s, bool nocase)
{
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (*p == '*')
				p++;
			for (;; s++) {
				if (path_match(p, s, nocase))
					return true;
				if (!*s || *s == '/')
					return false;
			}
		}
		if (!*s || (*p == '?' && *s == '/'))
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

static int count_slashes(const char *s)
{
	int n = 0;
	for (; *s; s++)
		n += *s == '/';
	return n;
}

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
			/* only as deep as the pattern has components */
			if (count_slashes(relname) < count_slashes(pattern))
				walk(root, relname, pattern, nocase, seen, searchpath, fn, ctx, depth + 1);
		} else if (path_match(pattern, relname, nocase) && !namelist_has(seen, relname)) {
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
		for (int k = numpacks - 1; k >= 0; k--) {
			pack_t *pk = packs[k];
			if (pk->gamedir != i)
				continue;
			for (int f = 0; f < pk->numfiles; f++) {
				const char *n = pk->files[f].name;
				if (!path_match(pattern, n, nocase) || namelist_has(&seen, n))
					continue;
				if (seen.count >= seen.cap) {
					seen.cap = seen.cap ? seen.cap * 2 : 64;
					seen.names = Mem_Realloc(seen.names, sizeof(char *) * seen.cap);
				}
				seen.names[seen.count++] = Mem_Strdup(n);
				fn(n, pk->name, ctx);
			}
		}
	}
	if (fs_enginedir[0])
		walk(fs_enginedir, base, pattern, nocase, &seen, "(engine)", fn, ctx, 0);
	for (int i = 0; i < seen.count; i++)
		Mem_Free(seen.names[i]);
	Mem_Free(seen.names);
}
