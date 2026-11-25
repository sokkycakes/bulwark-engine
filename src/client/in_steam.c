/* Bulwark engine - Steam Input: the game's action manifest driving the
 * engine's keys, axes and look. ISC licence, see LICENSE.
 *
 * The game ships steam_input/steam_input_manifest.vdf (Valve's In-Game
 * Actions format) and the controller configs it names. Each digital action
 * becomes a key the engine binds like any other:
 *   - an action named key_<KEY> presses that engine key ("key_gp_a" is
 *     GP_A), so the menus' controller handling works unchanged;
 *   - any other action presses SI_<ACTION>, bound by default to the command
 *     steam_input/bindings.txt gives it ("jump +jump").
 * Analog actions: "move" and "menu_move" are the left stick, "look" turns
 * the view (as a stick, or as mouse deltas from trackpads and gyro).
 * Two action sets switch by themselves: "Menu" while a menu, the console or
 * a loading screen has the input, "InGame" otherwise. docs/spec/steam-input.md */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "in_pads.h"
#include "steaminput.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/keyvalues.h"
#include "../ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MANIFEST "steam_input/steam_input_manifest.vdf"
#define BINDINGS "steam_input/bindings.txt"
#define MAX_ACTIONS 160
#define MAX_SETS 16

bool EM_Active(void);

static cvar_t *in_steaminput, *steam_input_appid, *in_steamlook;

static struct {
	char name[64];
	int set;      /* index in sets[] */
	bool analog;
	int key;      /* digital: the engine key it presses */
	si_handle_t h;
} acts[MAX_ACTIONS];
static int nacts;

static struct {
	char name[64];
	bool layer;
	si_handle_t h;
} sets[MAX_SETS];
static int nsets;

static struct {
	si_handle_t h;
	int slot;
	int set;          /* active set index, -1 none yet */
	bool layers[MAX_SETS];
	bool down[MAX_ACTIONS];
	float axis[4];
} ctrls[SI_MAX_CONTROLLERS];
static int nctrls;

static char manifest_path[1024];
static char forced_set[64];
static bool layer_wanted[MAX_SETS];
static bool tried;
static char exe_dir[1024];

static int find_set(const char *name)
{
	for (int i = 0; i < nsets; i++)
		if (!Str_ICmp(sets[i].name, name))
			return i;
	return -1;
}

static int find_action(const char *name)
{
	for (int i = 0; i < nacts; i++)
		if (!Str_ICmp(acts[i].name, name))
			return i;
	return -1;
}

/* --- the manifest ------------------------------------------------------------- */

static bool absolute(const char *in, char *out, size_t size)
{
#ifdef _WIN32
	return _fullpath(out, in, size) != NULL;
#else
	char *r = realpath(in, NULL);
	if (!r)
		return false;
	snprintf(out, size, "%s", r);
	free(r);
	return true;
#endif
}

static bool regular_file(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISREG(st.st_mode);
}

/* Steam reads the manifest and its configs from disk, so only a loose file
 * will do: the newest game directory's, then Bulwark's own data */
static bool find_manifest(char *out, size_t size)
{
	char p[1024];
	for (int i = FS_NumGameDirs() - 1; i >= 0; i--) {
		snprintf(p, sizeof(p), "%s/%s/%s", FS_BaseDir(), FS_GameDirAt(i), MANIFEST);
		if (regular_file(p))
			return absolute(p, out, size);
	}
	if (FS_EngineDir()) {
		snprintf(p, sizeof(p), "%s/%s", FS_EngineDir(), MANIFEST);
		if (regular_file(p))
			return absolute(p, out, size);
	}
	return false;
}

static void read_sets(const kv_t *block, bool layer)
{
	for (const kv_t *s = block ? block->child : NULL; s; s = s->next) {
		int set;
		if (!KV_IsBlock(s) || nsets >= MAX_SETS)
			continue;
		set = nsets++;
		snprintf(sets[set].name, sizeof(sets[0].name), "%s", s->key);
		sets[set].layer = layer;
		for (const kv_t *g = s->child; g; g = g->next) {
			bool analog = !Str_ICmp(g->key, "StickPadGyro") || !Str_ICmp(g->key, "AnalogTrigger");
			if (!KV_IsBlock(g) || (!analog && Str_ICmp(g->key, "Button")))
				continue;
			for (const kv_t *a = g->child; a && nacts < MAX_ACTIONS; a = a->next) {
				snprintf(acts[nacts].name, sizeof(acts[0].name), "%s", a->key);
				acts[nacts].set = set;
				acts[nacts].analog = analog;
				nacts++;
			}
		}
	}
}

/* the default command of each SI_ key, applied when nothing binds it */
static void apply_default_bindings(void)
{
	size_t len;
	char *text = FS_LoadFile(BINDINGS, &len);
	char *line, *save = NULL;
	if (!text) {
		FILE *f;
		char p[1100];
		/* beside a manifest that came from Bulwark's own data */
		snprintf(p, sizeof(p), "%s/%s", FS_EngineDir() ? FS_EngineDir() : ".", BINDINGS);
		if ((f = fopen(p, "rb"))) {
			long n;
			fseek(f, 0, SEEK_END);
			n = ftell(f);
			fseek(f, 0, SEEK_SET);
			text = Mem_Alloc(n + 1);
			len = fread(text, 1, n, f);
			text[len] = 0;
			fclose(f);
		}
	}
	if (!text)
		return;
	for (line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		char name[64];
		int n = 0, a;
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || line[0] == '/' || line[0] == '#')
			continue;
		if (sscanf(line, "%63s %n", name, &n) < 1 || !n || (a = find_action(name)) < 0 || acts[a].analog)
			continue;
		if (acts[a].key >= K_SI_FIRST && !*Key_GetBinding(acts[a].key))
			Key_SetBinding(acts[a].key, line + n);
	}
	Mem_Free(text);
}

static void load_manifest(void)
{
	size_t len;
	FILE *f;
	char *text = NULL;
	kv_t *kv, *root;
	int next_key = K_SI_FIRST;
	nacts = nsets = 0;
	if (!find_manifest(manifest_path, sizeof(manifest_path))) {
		manifest_path[0] = 0;
		return;
	}
	if ((f = fopen(manifest_path, "rb"))) {
		long n;
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		text = Mem_Alloc(n + 1);
		len = fread(text, 1, n, f);
		fclose(f);
	}
	if (!text || !(kv = KV_Parse((const uint8_t *)text, len, 0, NULL))) {
		Con_Printf("Steam Input: can't read %s\n", manifest_path);
		Mem_Free(text);
		manifest_path[0] = 0;
		return;
	}
	root = kv;
	read_sets(KV_Find(root, "actions"), false);
	read_sets(KV_Find(root, "action_layers"), true);
	KV_Free(kv);
	Mem_Free(text);
	for (int i = 0; i < nacts; i++) {
		if (acts[i].analog)
			continue;
		if (!Str_NICmp(acts[i].name, "key_", 4)) {
			acts[i].key = Key_NumForName(acts[i].name + 4);
			if (acts[i].key <= 0)
				Con_Printf("Steam Input: action %s names no engine key\n", acts[i].name);
			continue;
		}
		/* an action in several sets keeps one key */
		for (int j = 0; j < i; j++)
			if (!acts[j].analog && !Str_ICmp(acts[j].name, acts[i].name))
				acts[i].key = acts[j].key;
		if (!acts[i].key && next_key <= K_SI_LAST) {
			acts[i].key = next_key++;
			Key_SetSteamActionName(acts[i].key, acts[i].name);
		}
	}
	apply_default_bindings();
	Con_DPrintf("Steam Input: %s: %d action sets, %d actions\n", manifest_path, nsets, nacts);
}

/* --- start and stop ----------------------------------------------------------- */

static void si_log(const char *msg) { Con_Printf("%s\n", msg); }

static void start(void)
{
	const char *dirs[3] = {exe_dir[0] ? exe_dir : ".", FS_BaseDir(), NULL};
	si_config_t cfg = {0};
	tried = true;
	if (!in_steaminput->ivalue)
		return;
	cfg.libdirs = dirs;
	cfg.manifest = manifest_path[0] ? manifest_path : NULL;
	cfg.appid = (unsigned)BW_MAX(0, steam_input_appid->ivalue);
	cfg.log = si_log;
	if (!cfg.manifest)
		Con_Printf("Steam Input: no %s in the game or Bulwark's data, so Steam's own layout applies\n", MANIFEST);
	si_init(&cfg);
}

static void release_slot(int i)
{
	for (int a = 0; a < nacts; a++)
		if (ctrls[i].down[a] && acts[a].key > 0)
			IN_DeviceKeyEvent(ctrls[i].slot + 1, acts[a].key, false, 0);
	for (int ax = 0; ax < 4; ax++)
		if (ctrls[i].axis[ax])
			IN_JoyAxis(ctrls[i].slot, ax, 0);
	Pad_Release(ctrls[i].slot);
	IN_ControllerRemoved(ctrls[i].slot);
}

static void stop(void)
{
	for (int i = 0; i < nctrls; i++)
		release_slot(i);
	nctrls = 0;
	si_shutdown();
	for (int i = 0; i < nsets; i++)
		sets[i].h = 0;
	for (int i = 0; i < nacts; i++)
		acts[i].h = 0;
}

static void SteamInputRestart_f(int argc, const char **argv)
{
	stop();
	load_manifest();
	start();
}

static void SteamInputStatus_f(int argc, const char **argv)
{
	Con_Printf("launched: %s\n", si_launch_name(si_launch()));
	Con_Printf("Steam Input: %s\n", si_status());
	Con_Printf("manifest: %s\n", manifest_path[0] ? manifest_path : "(none)");
	for (int i = 0; i < nctrls; i++)
		Con_Printf("controller %d: Steam Input handle %llu, %s glyphs, set %s\n", ctrls[i].slot,
		           (unsigned long long)ctrls[i].h, si_set_name(si_set_for_controller(ctrls[i].h)),
		           ctrls[i].set >= 0 ? sets[ctrls[i].set].name : "(none)");
}

static void SteamInputSet_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("steaminput_set <action set> | auto: pin an action set, or let the menus pick\n");
		return;
	}
	snprintf(forced_set, sizeof(forced_set), "%s", Str_ICmp(argv[1], "auto") ? argv[1] : "");
}

static void SteamInputLayer_f(int argc, const char **argv)
{
	int l = argc > 2 ? find_set(argv[1]) : -1;
	if (l < 0 || !sets[l].layer) {
		Con_Printf("steaminput_layer <layer> 0|1: a layer from the manifest's action_layers\n");
		return;
	}
	layer_wanted[l] = atoi(argv[2]) != 0;
}

static void SteamInputBindings_f(int argc, const char **argv)
{
	for (int i = 0; i < nctrls; i++)
		if (!si_show_binding_panel(ctrls[i].h))
			Con_Printf("Steam's binding panel didn't open (it needs the Steam overlay)\n");
	if (!nctrls)
		Con_Printf("no Steam Input controller\n");
}

void INS_Init(const char *exedir)
{
	snprintf(exe_dir, sizeof(exe_dir), "%s", exedir ? exedir : "");
	in_steaminput = Cvar_Get("in_steaminput", "1", CVAR_ARCHIVE,
	                         "Steam Input: 1 uses it whenever Steam is running, also as a non-Steam game; 0 uses "
	                         "plain SDL controllers (in_steaminput_restart applies it)");
	steam_input_appid = Cvar_Get("steam_input_appid", "480", 0,
	                             "App id for Steam Input when the game wasn't started as a Steam app (480 is "
	                             "Valve's Spacewar test app); 0 needs a real Steam launch or steam_appid.txt");
	in_steamlook = Cvar_Get("in_steamlook", "1", CVAR_ARCHIVE,
	                        "Scale of Steam Input's mouse-like look (trackpads, gyro) on top of sensitivity");
	Cmd_AddD("in_steaminput_restart", SteamInputRestart_f, "Reload the Steam Input manifest and restart Steam Input");
	Cmd_AddD("in_steaminput_status", SteamInputStatus_f, "How the game was started and what Steam Input is doing");
	Cmd_AddD("steaminput_set", SteamInputSet_f, "Pin a Steam Input action set, or auto");
	Cmd_AddD("steaminput_layer", SteamInputLayer_f, "Turn a Steam Input action layer on or off");
	Cmd_AddD("steaminput_bindings", SteamInputBindings_f, "Open Steam's controller binding panel");
	load_manifest();
	start();
}

void INS_Shutdown(void) { stop(); }

bool INS_Active(void) { return si_active() && nctrls > 0; }

/* --- each frame --------------------------------------------------------------- */

static void resolve_handles(void)
{
	for (int i = 0; i < nsets; i++)
		if (!sets[i].h)
			sets[i].h = si_action_set(sets[i].name);
	for (int i = 0; i < nacts; i++)
		if (!acts[i].h)
			acts[i].h = acts[i].analog ? si_analog_action(acts[i].name) : si_digital_action(acts[i].name);
}

static void sync_controllers(void)
{
	si_handle_t hs[SI_MAX_CONTROLLERS];
	int n = si_controllers(hs);
	/* gone */
	for (int i = 0; i < nctrls;) {
		bool still = false;
		for (int k = 0; k < n; k++)
			still |= hs[k] == ctrls[i].h;
		if (still) {
			i++;
			continue;
		}
		Con_Printf("controller %d disconnected (Steam Input)\n", ctrls[i].slot);
		release_slot(i);
		ctrls[i] = ctrls[--nctrls];
	}
	/* new */
	for (int k = 0; k < n && nctrls < SI_MAX_CONTROLLERS; k++) {
		bool known = false;
		int slot;
		for (int i = 0; i < nctrls; i++)
			known |= ctrls[i].h == hs[k];
		if (known || (slot = Pad_ClaimSteam(hs[k])) < 0)
			continue;
		memset(&ctrls[nctrls], 0, sizeof(ctrls[0]));
		ctrls[nctrls].h = hs[k];
		ctrls[nctrls].slot = slot;
		ctrls[nctrls].set = -1;
		nctrls++;
		Con_Printf("controller %d: Steam Input (%s glyphs)\n", slot, si_set_name(si_set_for_controller(hs[k])));
		IN_ControllerAdded(slot);
	}
}

static int wanted_set(void)
{
	int s;
	if (forced_set[0] && (s = find_set(forced_set)) >= 0)
		return s;
	if (cl.state != ca_active || UI_WantsInput() || Con_IsActive() || EM_Active())
		s = find_set("Menu");
	else
		s = find_set("InGame");
	/* a manifest without those names: its first set */
	if (s < 0)
		for (s = 0; s < nsets && sets[s].layer; s++)
			;
	return s < nsets ? s : -1;
}

static bool action_live(int c, int a)
{
	int s = acts[a].set;
	return s == ctrls[c].set || (sets[s].layer && ctrls[c].layers[s]);
}

static void poll_controller(int c)
{
	int slot = ctrls[c].slot;
	for (int a = 0; a < nacts; a++) {
		bool down;
		if (!acts[a].h)
			continue;
		if (acts[a].analog) {
			float x, y;
			int mode;
			bool active;
			if (!action_live(c, a))
				continue;
			si_analog(ctrls[c].h, acts[a].h, &x, &y, &mode, &active);
			if (!Str_ICmp(acts[a].name, "move") || !Str_ICmp(acts[a].name, "menu_move")) {
				/* Steam's y is up, a stick axis's is down */
				if (x != ctrls[c].axis[0] || -y != ctrls[c].axis[1]) {
					ctrls[c].axis[0] = x, ctrls[c].axis[1] = -y;
					IN_JoyAxis(slot, 0, x);
					IN_JoyAxis(slot, 1, -y);
					if (x || y)
						Pad_Touched(slot);
				}
			} else if (!Str_ICmp(acts[a].name, "look")) {
				/* k_EInputSourceMode_JoystickMove: a stick held at a
				 * deflection; anything else (trackpad, gyro, joystick
				 * camera) arrives as mouse-like deltas */
				if (mode == 6) {
					if (x != ctrls[c].axis[2] || -y != ctrls[c].axis[3]) {
						ctrls[c].axis[2] = x, ctrls[c].axis[3] = -y;
						IN_JoyAxis(slot, 2, x);
						IN_JoyAxis(slot, 3, -y);
					}
				} else {
					if (ctrls[c].axis[2] || ctrls[c].axis[3]) {
						ctrls[c].axis[2] = ctrls[c].axis[3] = 0;
						IN_JoyAxis(slot, 2, 0);
						IN_JoyAxis(slot, 3, 0);
					}
					IN_PadLook(slot, x * in_steamlook->value, y * in_steamlook->value);
				}
				if (x || y)
					Pad_Touched(slot);
			}
			continue;
		}
		down = action_live(c, a) && si_digital(ctrls[c].h, acts[a].h, NULL);
		if (down == ctrls[c].down[a])
			continue;
		ctrls[c].down[a] = down;
		if (acts[a].key > 0) {
			Con_DPrintf("Steam Input: controller %d %s %s (%s)\n", slot, acts[a].name, down ? "down" : "up",
			            Key_NameForNum(acts[a].key));
			if (down)
				Pad_Touched(slot);
			IN_DeviceKeyEvent(slot + 1, acts[a].key, down, 0);
		}
	}
}

void INS_Frame(void)
{
	int want;
	if (!tried)
		return;
	if (!in_steaminput->ivalue && si_active()) {
		stop();
		return;
	}
	if (!si_active())
		return;
	si_frame();
	resolve_handles();
	sync_controllers();
	want = wanted_set();
	for (int c = 0; c < nctrls; c++) {
		if (want >= 0 && sets[want].h && ctrls[c].set != want) {
			/* what the old set held lets go before the new one counts */
			for (int a = 0; a < nacts; a++)
				if (ctrls[c].down[a] && acts[a].key > 0 && acts[a].set == ctrls[c].set) {
					ctrls[c].down[a] = false;
					IN_DeviceKeyEvent(ctrls[c].slot + 1, acts[a].key, false, 0);
				}
			si_activate_set(ctrls[c].h, sets[want].h);
			ctrls[c].set = want;
		}
		for (int l = 0; l < nsets; l++)
			if (sets[l].layer && sets[l].h && ctrls[c].layers[l] != layer_wanted[l]) {
				si_activate_layer(ctrls[c].h, sets[l].h, layer_wanted[l]);
				ctrls[c].layers[l] = layer_wanted[l];
			}
		poll_controller(c);
	}
}

/* --- slots --------------------------------------------------------------------- */

static int ctrl_for_slot(int slot)
{
	for (int i = 0; i < nctrls; i++)
		if (ctrls[i].slot == slot)
			return i;
	return -1;
}

uint64_t INS_SlotHandle(int slot)
{
	int c = ctrl_for_slot(slot);
	return c >= 0 ? ctrls[c].h : 0;
}

int INS_SlotSet(int slot)
{
	int c = ctrl_for_slot(slot);
	return c >= 0 ? (int)si_set_for_controller(ctrls[c].h) : -1;
}

void INS_Rumble(int slot, float low, float high, float seconds)
{
	int c = ctrl_for_slot(slot);
	/* ISteamInput's vibration runs until changed; zero stops it */
	if (c >= 0)
		si_rumble(ctrls[c].h, seconds > 0 ? low : 0, seconds > 0 ? high : 0);
}

void INS_LED(int slot, const float *rgb)
{
	int c = ctrl_for_slot(slot);
	if (c >= 0)
		si_led(ctrls[c].h, rgb[0], rgb[1], rgb[2]);
}

/* --- prompts ------------------------------------------------------------------- */

/* the action that "what" means: SI_<ACTION>, a key name a key_ action
 * presses, or a command an action's key is bound to */
static int action_for(const char *what, int set)
{
	int key = Key_NumForName(what);
	int best = -1;
	for (int a = 0; a < nacts; a++) {
		if (acts[a].analog || acts[a].key <= 0)
			continue;
		if (key > 0 ? acts[a].key == key : !Str_ICmp(Key_GetBinding(acts[a].key), what)) {
			if (acts[a].set == set)
				return a; /* the active set's own wins */
			if (best < 0)
				best = a;
		}
	}
	if (best < 0 && key <= 0)
		for (int a = 0; a < nacts; a++) /* "+jump" may also name the analog-free action "jump" */
			if (!acts[a].analog && what[0] == '+' && !Str_ICmp(acts[a].name, what + 1))
				return a;
	return best;
}

bool INS_Glyph(int slot, const char *what, char *path, size_t pathsize, char *label, size_t labelsize)
{
	int c = ctrl_for_slot(slot), a, origins[SI_MAX_ORIGINS], n;
	si_set_t set;
	si_button_t btn;
	if (c < 0 || (a = action_for(what, ctrls[c].set)) < 0 || !acts[a].h)
		return false;
	n = si_digital_origins(ctrls[c].h, sets[acts[a].set].h, acts[a].h, origins);
	if (n <= 0)
		return false;
	if (si_origin_glyph(origins[0], &set, &btn)) {
		snprintf(path, pathsize, "{images}/glyphs/%s/%s.png", si_set_name(set), si_button_name(btn));
		snprintf(label, labelsize, "%s", si_button_label(set, btn));
	} else {
		/* an origin newer than this build: Steam's words for it */
		const char *s = si_origin_string(origins[0]);
		path[0] = 0;
		snprintf(label, labelsize, "%s", s ? s : "?");
	}
	return true;
}
