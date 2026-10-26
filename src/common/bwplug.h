/* Bulwark engine - the native plugin interface. ISC licence, see LICENSE.
 *
 * A plugin is a shared library named bwplug_<name>.so (.dll, .dylib)
 * in the engine's directory or the base directory, loaded with
 * "plug_load <name>". It exports
 *     int bwplug_init(const bwplug_api_t *api);    nonzero to stay loaded
 * and may export
 *     void bwplug_frame(double realtime);          every frame
 *     void bwplug_shutdown(void);                  before it is unloaded
 * Commands a plugin adds are removed when it is unloaded. */
#ifndef BW_PLUG_H
#define BW_PLUG_H

#define BWPLUG_VERSION 1

typedef void (*bwplug_cmd_fn)(int argc, const char **argv);

typedef struct {
	int version; /* BWPLUG_VERSION */
	void (*print)(const char *text);
	void (*add_command)(const char *name, bwplug_cmd_fn fn, const char *description);
	void (*add_text)(const char *text); /* appended to the command buffer */
	const char *(*cvar_string)(const char *name); /* "" for none */
	void (*cvar_set)(const char *name, const char *value);
	void (*cvar_register)(const char *name, const char *value, const char *description);
	const char *(*gamedir)(void);
} bwplug_api_t;

#endif
