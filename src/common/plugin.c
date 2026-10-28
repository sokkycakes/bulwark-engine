/* Bulwark engine - native plugins (see bwplug.h): plug_load, plug_close,
 * plug_closeall and plug_list. ISC licence, see LICENSE. */
#include "bwplug.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <dirent.h>
#include "sys_dl.h"
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

typedef enum { FMT_IMAGE, FMT_MATERIAL, FMT_MODEL } fmtkind_t;
typedef struct {
	char ext[16];
	fmtkind_t kind;
	void *fn;
	int owner;
} plugfmt_t;
#define MAX_PLUGFMTS 64

static plugin_t plugins[MAX_PLUGINS];
static plugcmd_t plugcmds[MAX_PLUGCMDS];
static plugfmt_t plugfmts[MAX_PLUGFMTS];
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

static void *api_alloc(size_t size) { return Mem_Alloc(size); }
static void api_free(void *p) { Mem_Free(p); }
static void *api_load_file(const char *path, size_t *len) { return FS_LoadFile(path, len); }
static int api_file_exists(const char *path) { return FS_FileExists(path); }

static void register_format(fmtkind_t kind, const char *ext, void *fn)
{
	if (loading < 0 || !ext || ext[0] != '.' || strlen(ext) >= sizeof(plugfmts[0].ext) || !fn) {
		Con_Printf("plugin: can't add file format %s\n", ext ? ext : "(null)");
		return;
	}
	for (int i = 0; i < MAX_PLUGFMTS; i++)
		if (!plugfmts[i].fn) {
			Str_Copy(plugfmts[i].ext, ext, sizeof(plugfmts[i].ext));
			plugfmts[i].kind = kind;
			plugfmts[i].fn = fn;
			plugfmts[i].owner = loading;
			return;
		}
	Con_Printf("plugin: too many file formats\n");
}

static void api_register_image(const char *ext, bwplug_image_fn fn) { register_format(FMT_IMAGE, ext, *(void **)&fn); }
static void api_register_material(const char *ext, bwplug_material_fn fn)
{
	register_format(FMT_MATERIAL, ext, *(void **)&fn);
}
static void api_register_model(const char *ext, bwplug_model_fn fn) { register_format(FMT_MODEL, ext, *(void **)&fn); }

static const bwplug_api_t api = {BWPLUG_VERSION,     api_print,          api_add_command,       api_add_text,
                                 api_cvar_string,    api_cvar_set,       api_cvar_register,     FS_GameDir,
                                 api_alloc,          api_free,           api_load_file,         api_file_exists,
                                 api_register_image, api_register_material, api_register_model};

/* --- file formats ------------------------------------------------------------ */

static const char *path_ext(const char *path)
{
	const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
	return dot && (!slash || dot > slash) ? dot : "";
}

static void *find_format(fmtkind_t kind, const char *ext)
{
	for (int i = 0; i < MAX_PLUGFMTS; i++)
		if (plugfmts[i].fn && plugfmts[i].kind == kind && !Str_ICmp(plugfmts[i].ext, ext))
			return plugfmts[i].fn;
	return NULL;
}

/* the n-th image extension plugins added (".vtf"), or NULL past the end */
const char *Plug_ImageExt(int n)
{
	for (int i = 0; i < MAX_PLUGFMTS; i++)
		if (plugfmts[i].fn && plugfmts[i].kind == FMT_IMAGE && n-- == 0)
			return plugfmts[i].ext;
	return NULL;
}

/* RGBA pixels (Mem_Free) for an image file a plugin reads, by its extension */
uint8_t *Plug_DecodeImage(const char *path, const void *data, size_t len, int *w, int *h)
{
	bwplug_image_fn fn;
	void *p = find_format(FMT_IMAGE, path_ext(path));
	uint8_t *px;
	if (!p)
		return NULL;
	*(void **)&fn = p;
	*w = *h = 0;
	px = fn(data, len, w, h);
	if (px && (*w <= 0 || *h <= 0 || *w > 16384 || *h > 16384)) {
		Mem_Free(px);
		px = NULL;
	}
	return px;
}

/* a material from a file a plugin reads: base is the name without an
 * extension, tried with each material extension. True when one was read. */
bool Plug_LoadMaterial(const char *base, bwplug_material_t *out)
{
	for (int i = 0; i < MAX_PLUGFMTS; i++) {
		char path[256];
		bwplug_material_fn fn;
		size_t len;
		char *data;
		int ok;
		if (!plugfmts[i].fn || plugfmts[i].kind != FMT_MATERIAL || strlen(base) + sizeof(plugfmts[i].ext) > sizeof(path))
			continue;
		Str_Copy(path, base, sizeof(path));
		Str_Cat(path, plugfmts[i].ext, sizeof(path));
		if (!(data = FS_LoadFile(path, &len)))
			continue;
		memset(out, 0, sizeof(*out));
		*(void **)&fn = plugfmts[i].fn;
		ok = fn(path, data, len, out);
		Mem_Free(data);
		if (ok) {
			out->diffuse[sizeof(out->diffuse) - 1] = out->normal[sizeof(out->normal) - 1] = 0;
			out->fullbright[sizeof(out->fullbright) - 1] = 0;
			return true;
		}
	}
	return false;
}

bool Plug_MaterialExists(const char *base)
{
	for (int i = 0; i < MAX_PLUGFMTS; i++)
		if (plugfmts[i].fn && plugfmts[i].kind == FMT_MATERIAL && FS_FileExists(va("%s%s", base, plugfmts[i].ext)))
			return true;
	return false;
}

/* an IQM file (Mem_Free) made by a plugin from a model the engine can't read */
void *Plug_ConvertModel(const char *path, const void *data, size_t len, size_t *outlen)
{
	bwplug_model_fn fn;
	void *p = find_format(FMT_MODEL, path_ext(path));
	*outlen = 0;
	if (!p)
		return NULL;
	*(void **)&fn = p;
	return fn(path, data, len, outlen);
}

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
	for (int k = 0; k < MAX_PLUGFMTS; k++)
		if (plugfmts[k].fn && plugfmts[k].owner == i)
			plugfmts[k].fn = NULL;
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

static bool (*builtin_hook)(const char *name);
void Plug_SetBuiltinHook(bool (*fn)(const char *name)) { builtin_hook = fn; }

/* plug_load name|path: bwplug_<name>.so in the engine's directory, then the base directory */
static void PlugLoad_f(int argc, const char **argv)
{
	char path[1200];
	if (argc < 2) {
		Con_Printf("Loads a plugin\nplug_load [pluginpath]\nexample pluginpath: blah\nwill load bwplug_blah%s\n", PLUG_EXT);
		return;
	}
	const char *name = argv[1];
	if (builtin_hook && builtin_hook(name))
		return;
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

/* every bwplug_*.so next to the engine (not in the game directories, which
 * downloads can reach): plugins that ship with it, such as the Source file
 * formats, work without a plug_load. Remove the file to leave one out. */
void Plug_AutoLoad(void)
{
	DIR *d = opendir(bindir());
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name), x = strlen(PLUG_EXT);
		char name[64], path[1200];
		if (strncmp(e->d_name, "bwplug_", 7) || n <= 7 + x || n - 7 - x >= sizeof(name) ||
		    strcmp(e->d_name + n - x, PLUG_EXT))
			continue;
		Str_Copy(name, e->d_name + 7, n - 7 - x + 1);
		snprintf(path, sizeof(path), "%s/%s", bindir(), e->d_name);
		if (!try_load(name, path))
			Con_Printf("Couldn't load plugin %s: %s\n", name, dlerror());
	}
	closedir(d);
}

void Plug_Init(void)
{
	Cmd_AddD("plug_load", PlugLoad_f, "plug_load <name|path>: load a native plugin (bwplug_<name>" PLUG_EXT ")");
	Cmd_AddD("plug_close", PlugClose_f, "plug_close <name>: unload a plugin");
	Cmd_AddD("plug_closeall", PlugCloseAll_f, "Unload every plugin");
	Cmd_AddD("plug_list", PlugList_f, "The loaded plugins, and the ones that could be loaded");
}
