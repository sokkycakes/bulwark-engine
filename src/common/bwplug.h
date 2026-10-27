/* Bulwark engine - the native plugin interface. ISC licence, see LICENSE.
 *
 * A plugin is a shared library named bwplug_<name>.so (.dll, .dylib)
 * in the engine's directory or the base directory, loaded with
 * "plug_load <name>". It exports
 *     int bwplug_init(const bwplug_api_t *api);    nonzero to stay loaded
 * and may export
 *     void bwplug_frame(double realtime);          every frame
 *     void bwplug_shutdown(void);                  before it is unloaded
 * Commands and file formats a plugin adds are removed when it is unloaded.
 *
 * Version 2 adds file formats. A plugin turns a file into something the
 * engine already reads: an image into RGBA pixels, a material into a
 * bwplug_material_t, a model into an IQM version 2 file in memory. What a
 * converter returns must come from api->alloc; the engine frees it. Files
 * are read through the game's filesystem (api->load_file), so packages and
 * game directories work as for everything else. */
#ifndef BW_PLUG_H
#define BW_PLUG_H

#include <stddef.h>

#define BWPLUG_VERSION 2

typedef void (*bwplug_cmd_fn)(int argc, const char **argv);

/* a material, as the engine builds it: image names are game paths, with or
 * without an extension */
enum {
	BWPLUG_MAT_TRANSLUCENT = 1 << 0, /* alpha blended */
	BWPLUG_MAT_ADDITIVE = 1 << 1,
	BWPLUG_MAT_ALPHATEST = 1 << 2,
	BWPLUG_MAT_TWOSIDED = 1 << 3,
	BWPLUG_MAT_NODRAW = 1 << 4,
	BWPLUG_MAT_UNLIT = 1 << 5,
	BWPLUG_MAT_SKY = 1 << 6,
	BWPLUG_MAT_WATER = 1 << 7
};
typedef struct {
	char diffuse[192];
	char normal[192];
	char fullbright[192]; /* glow, added on top */
	int flags;            /* BWPLUG_MAT_ */
	float alpha;          /* 0 means 1 */
} bwplug_material_t;

/* RGBA pixels (api->alloc) and the size, or NULL */
typedef unsigned char *(*bwplug_image_fn)(const void *data, size_t len, int *width, int *height);
/* fills out for the material file at path; nonzero on success */
typedef int (*bwplug_material_fn)(const char *path, const void *data, size_t len, bwplug_material_t *out);
/* an IQM version 2 file (api->alloc) for the model at path, or NULL */
typedef void *(*bwplug_model_fn)(const char *path, const void *data, size_t len, size_t *outlen);

typedef struct {
	int version; /* BWPLUG_VERSION */
	void (*print)(const char *text);
	void (*add_command)(const char *name, bwplug_cmd_fn fn, const char *description);
	void (*add_text)(const char *text); /* appended to the command buffer */
	const char *(*cvar_string)(const char *name); /* "" for none */
	void (*cvar_set)(const char *name, const char *value);
	void (*cvar_register)(const char *name, const char *value, const char *description);
	const char *(*gamedir)(void);
	/* version 2 */
	void *(*alloc)(size_t size); /* zeroed */
	void (*free)(void *p);
	void *(*load_file)(const char *path, size_t *len); /* NUL terminated; free with api->free */
	int (*file_exists)(const char *path);
	/* ext includes the dot (".vtf"). A model converter is asked only for
	 * files the engine can't read itself. */
	void (*register_image)(const char *ext, bwplug_image_fn fn);
	void (*register_material)(const char *ext, bwplug_material_fn fn);
	void (*register_model)(const char *ext, bwplug_model_fn fn);
} bwplug_api_t;

#endif
