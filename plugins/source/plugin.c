/* Bulwark Source formats plugin: VTF textures, VMT materials and studio
 * models (MDL version 44 to 49, with their VVD and VTX files), turned into
 * what the engine reads. See docs/spec/source-formats.md. Loaded on its
 * own when bwplug_source.so sits next to the engine.
 * ISC licence, see LICENSE. */
#include "source.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const bwplug_api_t *api;

void srcprint(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	api->print(buf);
}

void src_cleanpath(char *s)
{
	char *o = s, *start = s;
	while (*s == '/' || *s == '\\')
		s++;
	for (; *s; s++) {
		char c = *s == '\\' ? '/' : (char)tolower((unsigned char)*s);
		if (c == '/' && o > start && o[-1] == '/')
			continue; /* a doubled separator counts once */
		*o++ = c;
	}
	*o = 0;
}

static unsigned char *image(const void *data, size_t len, int *w, int *h) { return VTF_Decode(data, len, w, h); }

static void *model(const char *path, const void *data, size_t len, size_t *outlen)
{
	return MDL_Convert(path, data, len, outlen);
}

__attribute__((visibility("default"))) int bwplug_init(const bwplug_api_t *a)
{
	api = a;
	if (a->version < 2) {
		a->print("source plugin: this engine has no file format hooks\n");
		return 0;
	}
	a->register_image(".vtf", image);
	a->register_material(".vmt", VMT_Load);
	a->register_model(".mdl", model);
	return 1;
}
