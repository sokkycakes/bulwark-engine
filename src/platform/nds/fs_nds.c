/* Bulwark engine - the DS build's files: loose files in the base folders,
 * then Quake .pak files in them (so the Quake shareware's pak0.pak works as
 * is). ISC licence, see LICENSE. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nds.h"

#define MAX_PAKS 8

typedef struct {
	char name[56];
	uint32_t pos, len;
} pakfile_t;

typedef struct {
	char path[300];
	pakfile_t *files;
	int numfiles;
} pak_t;

static pak_t paks[MAX_PAKS];
static int numpaks;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void Fs_Init(void)
{
	for (int b = 0; b < NDS_NUMBASEDIRS; b++)
		for (int n = 0; n < 10 && numpaks < MAX_PAKS; n++) {
			pak_t *p = &paks[numpaks];
			uint8_t head[12];
			FILE *f;
			snprintf(p->path, sizeof(p->path), "%spak%d.pak", nds_basedirs[b], n);
			if (!(f = fopen(p->path, "rb")))
				continue;
			if (fread(head, 1, 12, f) == 12 && !memcmp(head, "PACK", 4) && rd32(head + 8) / 64 > 0) {
				int count = (int)(rd32(head + 8) / 64);
				uint8_t *dir = malloc((size_t)count * 64);
				p->files = malloc(sizeof(pakfile_t) * (size_t)count);
				if (dir && p->files && !fseek(f, (long)rd32(head + 4), SEEK_SET) &&
				    fread(dir, 64, (size_t)count, f) == (size_t)count) {
					for (int i = 0; i < count; i++) {
						memcpy(p->files[i].name, dir + i * 64, 55);
						p->files[i].name[55] = 0;
						p->files[i].pos = rd32(dir + i * 64 + 56);
						p->files[i].len = rd32(dir + i * 64 + 60);
					}
					p->numfiles = count;
					numpaks++;
				} else {
					free(p->files);
					p->files = NULL;
				}
				free(dir);
			}
			fclose(f);
		}
}

int Fs_NumPaks(void) { return numpaks; }

static uint8_t *read_range(const char *path, long pos, long len, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	if (!f)
		return NULL;
	if (len < 0) {
		fseek(f, 0, SEEK_END);
		len = ftell(f);
	}
	buf = malloc((size_t)len + 1);
	if (!buf || fseek(f, pos, SEEK_SET) || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[len] = 0;
	if (size)
		*size = (size_t)len;
	return buf;
}

/* a path with a device ("fat:/...") as is; otherwise relative to the base
 * folders, then inside the paks (later paks first, as Quake does) */
uint8_t *Fs_Load(const char *name, size_t *size)
{
	char path[300];
	uint8_t *buf;
	if (strstr(name, ":/"))
		return read_range(name, 0, -1, size);
	for (int b = 0; b < NDS_NUMBASEDIRS; b++) {
		snprintf(path, sizeof(path), "%s%s", nds_basedirs[b], name);
		if ((buf = read_range(path, 0, -1, size)))
			return buf;
	}
	for (int p = numpaks - 1; p >= 0; p--)
		for (int i = 0; i < paks[p].numfiles; i++)
			if (!strcasecmp(paks[p].files[i].name, name))
				return read_range(paks[p].path, (long)paks[p].files[i].pos, (long)paks[p].files[i].len, size);
	return NULL;
}

/* part of a file, as Fs_Load finds it: len bytes from pos (len < 0: to the end) */
uint8_t *Fs_LoadRange(const char *name, long pos, long len, size_t *size)
{
	char path[300];
	if (strstr(name, ":/"))
		return len < 0 ? read_range(name, 0, -1, size) : read_range(name, pos, len, size);
	for (int b = 0; b < NDS_NUMBASEDIRS; b++) {
		FILE *f;
		snprintf(path, sizeof(path), "%s%s", nds_basedirs[b], name);
		if (!(f = fopen(path, "rb")))
			continue;
		fclose(f);
		return read_range(path, pos, len, size);
	}
	for (int p = numpaks - 1; p >= 0; p--)
		for (int i = 0; i < paks[p].numfiles; i++)
			if (!strcasecmp(paks[p].files[i].name, name)) {
				long flen = (long)paks[p].files[i].len;
				if (pos > flen)
					return NULL;
				if (len < 0 || pos + len > flen)
					len = flen - pos;
				return read_range(paks[p].path, (long)paks[p].files[i].pos + pos, len, size);
			}
	return NULL;
}

/* a file's size, as Fs_Load finds it, or -1 */
long Fs_Size(const char *name)
{
	char path[300];
	for (int b = strstr(name, ":/") ? -1 : 0; b < NDS_NUMBASEDIRS; b++) {
		FILE *f;
		long n;
		snprintf(path, sizeof(path), "%s%s", b < 0 ? "" : nds_basedirs[b], name);
		if (!(f = fopen(path, "rb")))
			continue;
		n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
		fclose(f);
		return n;
	}
	for (int p = numpaks - 1; p >= 0; p--)
		for (int i = 0; i < paks[p].numfiles; i++)
			if (!strcasecmp(paks[p].files[i].name, name))
				return (long)paks[p].files[i].len;
	return -1;
}

/* calls fn for each file in dir ending in ext: loose files (full paths) then pak entries */
void Fs_List(const char *dir, const char *ext, void (*fn)(const char *name, void *user), void *user)
{
	size_t el = strlen(ext), dl = strlen(dir);
	for (int b = 0; b < NDS_NUMBASEDIRS; b++) {
		char path[300];
		DIR *d;
		struct dirent *e;
		snprintf(path, sizeof(path), "%s%s", nds_basedirs[b], dir);
		if (!(d = opendir(path)))
			continue;
		while ((e = readdir(d))) {
			size_t n = strlen(e->d_name);
			char full[600];
			if (n > el && !strcasecmp(e->d_name + n - el, ext)) {
				snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
				fn(full, user);
			}
		}
		closedir(d);
	}
	for (int p = 0; p < numpaks; p++)
		for (int i = 0; i < paks[p].numfiles; i++) {
			const char *nm = paks[p].files[i].name;
			size_t n = strlen(nm);
			if (n > dl + 1 + el && !strncasecmp(nm, dir, dl) && nm[dl] == '/' && !strchr(nm + dl + 1, '/') &&
			    !strcasecmp(nm + n - el, ext))
				fn(nm, user);
		}
}
