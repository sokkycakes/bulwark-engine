/* Bulwark engine - the plugin API the Source skeleton reader needs, for
 * host tools: files come from a folder. ISC licence, see LICENSE. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../plugins/source/source.h"

static const char *root = ".";

static void *st_alloc(size_t n) { return calloc(1, n ? n : 1); }
static void st_free(void *p) { free(p); }
static void *st_load(const char *path, size_t *len)
{
	char full[1024];
	FILE *f;
	long n;
	unsigned char *d;
	snprintf(full, sizeof(full), "%s/%s", root, path);
	*len = 0;
	if (!(f = fopen(full, "rb")))
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = calloc(1, (size_t)n + 1);
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	if (d)
		*len = (size_t)n;
	return d;
}

static bwplug_api_t stub = {.alloc = st_alloc, .free = st_free, .load_file = st_load};
const bwplug_api_t *api = &stub;

void skelstub_root(const char *dir) { root = dir; }

void srcprint(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void src_cleanpath(char *s)
{
	char *o = s, *start = s;
	while (*s == '/' || *s == '\\')
		s++;
	for (; *s; s++) {
		char c = *s == '\\' ? '/' : (char)tolower((unsigned char)*s);
		if (c == '/' && o > start && o[-1] == '/')
			continue;
		*o++ = c;
	}
	*o = 0;
}

#include "../../plugins/source/mdlskel.h"

const char *str_at(const blob_t *b, size_t ofs)
{
	if (!in(b, ofs, 1) || !memchr(b->d + ofs, 0, b->len - ofs))
		return "";
	return (const char *)b->d + ofs;
}
