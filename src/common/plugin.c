/* Bulwark engine - native plugins (see bwplug.h): plug_load, plug_close,
 * plug_closeall and plug_list. ISC licence, see LICENSE. */
#include "bwplug.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(_WIN32)
#define PLUG_EXT ".dll"
#elif defined(__APPLE__)
#define PLUG_EXT ".dylib"
#else
#define PLUG_EXT ".so"
#endif
#define MAX_PLUGINS 32
#define MAX_PLUGCMDS 256

typedef struct {
	char name[64];
	char path[1024];
	void *handle;
	void (*frame)(double realtime);
	void (*shutdown)(void);
} plugin_t;

typedef struct {
	char name[64];
	bwplug_cmd_fn fn;
	int owner;
} plugcmd_t;

static plugin_t plugins[MAX_PLUGINS];
static plugcmd_t plugcmds[MAX_PLUGCMDS];
static int loading = -1; /* the plugin whose bwplug_init is running */

static void api_print(const char *text) { Con_Printf("%s", text); }

static void plugin_cmd(int argc, const char **argv)
{
	for (int i = 0; i < MAX_PLUGCMDS; i++)
		if (plugcmds[i].fn && !Str_ICmp(plugcmds[i].name, argv[0])) {
			plugcmds[i].fn(argc, argv);
			return;
		}
}

static void api_add_command(const char *name, bwplug_cmd_fn fn, const char *description)
{
	if (loading < 0 || !name || !fn || Cmd_Exists(name)) {
		Con_Printf("plugin: can't add command %s\n", name ? name : "(null)");
		return;
	}
	for (int i = 0; i < MAX_PLUGCMDS; i++)
		if (!plugcmds[i].fn) {
			Str_Copy(plugcmds[i].name, name, sizeof(plugcmds[i].name));
			plugcmds[i].fn = fn;
			plugcmds[i].owner = loading;
			Cmd_AddD(plugcmds[i].name, plugin_cmd, description);
			return;
		}
}

static void api_add_text(const char *text) { Cbuf_AddText(text); }

static const char *api_cvar_string(const char *name)
{
	cvar_t *v = Cvar_Find(name);
	return v ? v->string : "";
}

static void api_cvar_set(const char *name, const char *value) { Cvar_Set(name, value); }

static void api_cvar_register(const char *name, const char *value, const char *description)
{
	Cvar_Get(Mem_Strdup(name), value, 0, description ? Mem_Strdup(description) : NULL);
}

static const bwplug_api_t api = {BWPLUG_VERSION, api_print, api_add_command, api_add_text, api_cvar_string,
                                 api_cvar_set, api_cvar_register, FS_GameDir};

static const char *bindir(void)
{
	static char dir[1024];
	if (!dir[0]) {
		ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
		dir[n > 0 ? n : 0] = 0;
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = 0;
		else
			Str_Copy(dir, ".", sizeof(dir));
	}
	return dir;
}

static void unload(int i)
{
	plugin_t *p = &plugins[i];
	if (!p->handle)
		return;
	if (p->shutdown)
		p->shutdown();
	for (int k = 0; k < MAX_PLUGCMDS; k++)
		if (plugcmds[k].fn && plugcmds[k].owner == i) {
			Cmd_Remove(plugcmds[k].name);
			plugcmds[k].fn = NULL;
		}
	dlclose(p->handle);
	Con_Printf("Unloaded plugin %s\n", p->name);
	memset(p, 0, sizeof(*p));
}

static bool try_load(const char *name, const char *path)
{
	int slot = -1;
	for (int i = 0; i < MAX_PLUGINS; i++) {
		if (plugins[i].handle && !Str_ICmp(plugins[i].name, name)) {
			Con_Printf("%s is already loaded\n", name);
			return true;
		}
		if (!plugins[i].handle && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return false;
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h)
		return false;
	int (*init)(const bwplug_api_t *) = NULL;
	*(void **)&init = dlsym(h, "bwplug_init");
	if (!init) {
		Con_Printf("%s isn't a Bulwark plugin (no bwplug_init)\n", path);
		dlclose(h);
		return true;
	}
	plugin_t *p = &plugins[slot];
	Str_Copy(p->name, name, sizeof(p->name));
	Str_Copy(p->path, path, sizeof(p->path));
	p->handle = h;
	*(void **)&p->frame = dlsym(h, "bwplug_frame");
	*(void **)&p->shutdown = dlsym(h, "bwplug_shutdown");
	loading = slot;
	int ok = init(&api);
	loading = -1;
	if (!ok) {
		Con_Printf("Plugin %s didn't want to load\n", name);
		p->shutdown = NULL;
		unload(slot);
	}
	return true;
}

/* plug_load name|path: bwplug_<name>.so in the engine's directory, then the base directory */
static void PlugLoad_f(int argc, const char **argv)
{
	char path[1200];
	if (argc < 2) {
		Con_Printf("Loads a plugin\nplug_load [pluginpath]\nexample pluginpath: blah\nwill load bwplug_blah%s\n", PLUG_EXT);
		return;
	}
	const char *name = argv[1];
	if (strchr(name, '/')) { /* a path: the name is its file's */
		const char *base = strrchr(name, '/') + 1;
		char n[64];
		Str_Copy(n, !strncmp(base, "bwplug_", 7) ? base + 7 : base, sizeof(n));
		if (strchr(n, '.'))
			*strchr(n, '.') = 0;
		if (!FS_SafeName(name) && name[0] != '/') {
			Con_Printf("Couldn't load plugin %s\n", name);
			return;
		}
		if (!try_load(n, name))
			Con_Printf("Couldn't load plugin %s: %s\n", name, dlerror());
		return;
	}
	if (!FS_SafeName(name)) {
		Con_Printf("Couldn't load plugin %s\n", name);
		return;
	}
	const char *dirs[] = {bindir(), FS_BaseDir()};
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/bwplug_%s%s", dirs[i], name, PLUG_EXT);
		if (access(path, R_OK) == 0) {
			if (!try_load(name, path))
				Con_Printf("Couldn't load plugin %s: %s\n", name, dlerror());
			return;
		}
	}
	Con_Printf("Couldn't load plugin %s\n", name);
}

static void PlugClose_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Close which plugin?\n");
		return;
	}
	for (int i = 0; i < MAX_PLUGINS; i++)
		if (plugins[i].handle && !Str_ICmp(plugins[i].name, argv[1])) {
			unload(i);
			return;
		}
	Con_Printf("Plugin %s isn't loaded\n", argv[1]);
}

static void PlugCloseAll_f(int argc, const char **argv)
{
	for (int i = 0; i < MAX_PLUGINS; i++)
		unload(i);
}

static void scan(const char *label, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	Con_Printf("Scanning for plugins at %s/:\n", label);
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name), x = strlen(PLUG_EXT);
		if (strncmp(e->d_name, "bwplug_", 7) || n <= 7 + x || strcmp(e->d_name + n - x, PLUG_EXT))
			continue;
		char name[64];
		Str_Copy(name, e->d_name + 7, BW_MIN(sizeof(name), n - 7 - x + 1));
		bool loaded = false;
		for (int i = 0; i < MAX_PLUGINS; i++)
			loaded |= plugins[i].handle && !Str_ICmp(plugins[i].name, name);
		Con_Printf("%s%s\n", name, loaded ? " (loaded)" : "");
	}
	closedir(d);
}

static void PlugList_f(int argc, const char **argv)
{
	Con_Printf("Loaded plugins:\n");
	for (int i = 0; i < MAX_PLUGINS; i++)
		if (plugins[i].handle)
			Con_Printf("%s: loaded\n", plugins[i].name);
	scan("$bindir", bindir());
	scan("$basedir", FS_BaseDir());
}

void Plug_Frame(void)
{
	for (int i = 0; i < MAX_PLUGINS; i++)
		if (plugins[i].handle && plugins[i].frame)
			plugins[i].frame(Sys_Seconds());
}

void Plug_Shutdown(void) { PlugCloseAll_f(0, NULL); }

void Plug_Init(void)
{
	Cmd_AddD("plug_load", PlugLoad_f, "plug_load <name|path>: load a native plugin (bwplug_<name>" PLUG_EXT ")");
	Cmd_AddD("plug_close", PlugClose_f, "plug_close <name>: unload a plugin");
	Cmd_AddD("plug_closeall", PlugCloseAll_f, "Unload every plugin");
	Cmd_AddD("plug_list", PlugList_f, "The loaded plugins, and the ones that could be loaded");
}
