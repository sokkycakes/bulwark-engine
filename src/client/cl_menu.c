/* Bulwark engine - menu progs (menu.dat): the front end that runs with or
 * without a game, such as Nuclide's main menu. It gets its own VM and the
 * same 2D drawing builtins as the client game. Entry points and key/mouse
 * routing: docs/spec/menu.md. ISC licence, see LICENSE. */
#include "client.h"
#include "../common/manifest.h"
#include "../net/pkg.h"
#include "../server/sv_local.h"

#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../render/render.h"
#include "../common/fs.h"

#include <stdio.h>

extern const qc_builtin_t r_builtins[];
extern const qc_builtin_t s_builtins[];

/* setkeydest values: who gets keys first */
enum { KEYDEST_GAME = 0, KEYDEST_CONSOLE = 1, KEYDEST_MENU = 2, KEYDEST_MENU_GRABBED = 3 };
/* setmousetarget values */
enum { MOUSETARGET_MENU = 1, MOUSETARGET_CLIENT = 2 };

static struct {
	qcvm_t *vm;
	func_t init, shutdown, draw, drawloading, input, toggle, consolecommand, restarted;
	int time, frametime;
	int keydest, mousetarget;
	char *registered[256];
	int numregistered;
	double lasttime;
} M;

static bool cmd_forced; /* menu_cmd: pass the line on even if unregistered */

static void m_setkeydest(qcvm_t *vm) { M.keydest = (int)QC_PFLOAT(vm, 0); }
static void m_getkeydest(qcvm_t *vm) { QC_ReturnFloat(vm, (float)M.keydest); }
static void m_setmousetarget(qcvm_t *vm) { M.mousetarget = (int)QC_PFLOAT(vm, 0); }
static void m_getmousetarget(qcvm_t *vm) { QC_ReturnFloat(vm, (float)M.mousetarget); }

static void m_registercommand(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	for (int i = 0; i < M.numregistered; i++)
		if (!Str_ICmp(M.registered[i], n))
			return;
	if (M.numregistered < (int)BW_ARRAY_COUNT(M.registered))
		M.registered[M.numregistered++] = Mem_Strdup(n);
}

static void return_maybe(qcvm_t *vm, const char *s)
{
	if (s)
		QC_ReturnString(vm, s);
	else
		vm->globals[OFS_RETURN].i = 0; /* the null string ends the caller's loop */
}

static void m_getgamedirinfo(qcvm_t *vm) { return_maybe(vm, Mf_GameDirInfo((int)QC_PFLOAT(vm, 0), (int)QC_PFLOAT(vm, 1))); }
/* takes ints */
static void m_getpackagemanagerinfo(qcvm_t *vm) { return_maybe(vm, PKG_Info(QC_PINT(vm, 0), QC_PINT(vm, 1))); }

/* menu-only builtins; anything else comes from the shared tables */
static const qc_builtin_t m_builtins[] = {
	{"setkeydest", m_setkeydest}, {"getkeydest", m_getkeydest},
	{"setmousetarget", m_setmousetarget}, {"getmousetarget", m_getmousetarget},
	{"registercommand", m_registercommand},
	{"getgamedirinfo", m_getgamedirinfo}, {"getpackagemanagerinfo", m_getpackagemanagerinfo},
	{NULL, NULL}
};

static void set_time(void)
{
	double t = cl.realtime;
	if (M.time >= 0)
		QC_SetGFloat(M.vm, M.time, (float)t);
	if (M.frametime >= 0)
		QC_SetGFloat(M.vm, M.frametime, (float)(t - M.lasttime));
	M.lasttime = t;
}

static void call(func_t f, int argc)
{
	M.vm->argc = argc;
	QC_Execute(M.vm, f);
}

bool MQC_Active(void)
{
	return M.vm && (M.keydest == KEYDEST_MENU || M.keydest == KEYDEST_MENU_GRABBED);
}

void MQC_Shutdown(void)
{
	if (!M.vm)
		return;
	if (M.shutdown) {
		set_time();
		call(M.shutdown, 0);
	}
	QC_Free(M.vm);
	for (int i = 0; i < M.numregistered; i++)
		Mem_Free(M.registered[i]);
	memset(&M, 0, sizeof(M));
}

bool MQC_Load(void)
{
	static const qc_builtin_t *tables[8];
	size_t len;
	char *data;
	MQC_Shutdown();
	data = FS_LoadFile("menu.dat", &len);
	if (!data)
		return false;
	tables[0] = m_builtins;
	tables[1] = r_builtins;
	tables[2] = s_builtins;
	tables[3] = qc_builtins_core;
	tables[4] = qc_builtins_extra;
	tables[5] = cl_builtins; /* clientstate, key bindings and the like */
	tables[6] = cl_hostcache_builtins;
	tables[7] = NULL;
	M.vm = QC_Load("menu.dat", data, len, QCVM_MENU, tables, 4096, 16 * 1024 * 1024);
	Mem_Free(data);
	if (!M.vm)
		return false;
	M.keydest = KEYDEST_GAME;
	M.mousetarget = MOUSETARGET_CLIENT;
	M.time = QC_GlobalOfs(M.vm, "time");
	M.frametime = QC_GlobalOfs(M.vm, "frametime");
	M.init = QC_FindFunction(M.vm, "m_init");
	M.shutdown = QC_FindFunction(M.vm, "m_shutdown");
	M.draw = QC_FindFunction(M.vm, "m_draw");
	M.drawloading = QC_FindFunction(M.vm, "m_drawloading");
	M.input = QC_FindFunction(M.vm, "Menu_InputEvent");
	M.toggle = QC_FindFunction(M.vm, "m_toggle");
	M.consolecommand = QC_FindFunction(M.vm, "m_consolecommand");
	M.restarted = QC_FindFunction(M.vm, "Menu_RendererRestarted");
	set_time();
	if (M.init)
		call(M.init, 0);
	if (M.restarted && R_Active()) {
		M.vm->globals[OFS_PARM(0)].i = QC_NewString(M.vm, "");
		call(M.restarted, 1);
	}
	return true;
}

void MQC_RendererRestarted(void)
{
	if (!M.vm || !M.restarted || !R_Active())
		return;
	M.vm->globals[OFS_PARM(0)].i = QC_NewString(M.vm, "");
	call(M.restarted, 1);
}

/* mode 1 shows the menu, 0 hides it, -1 flips it */
void MQC_Toggle(int mode)
{
	if (!M.vm || !M.toggle) {
		/* no menu module: the engine's own menu */
		if (mode != 0 && !EM_Active())
			EM_OpenMain();
		else if (mode != 1)
			EM_Close();
		return;
	}
	if (mode < 0)
		mode = !MQC_Active();
	set_time();
	M.vm->globals[OFS_PARM(0)].f = (float)mode;
	call(M.toggle, 1);
}

/* --- loading progress -------------------------------------------------------
 * Host_Loading reports land here. They are kept in three read-only cvars the
 * menu can read (cl_loadstage, cl_loadmap, cl_loadprogress), and each report
 * during a blocking load draws one frame so the loading screen moves. */

static cvar_t *cl_loadstage, *cl_loadmap, *cl_loadprogress;
static bool loading_drawing;

static void loading_hook(const char *stage, const char *map, float frac)
{
	int w, h;
	Cvar_SetDirect(cl_loadstage, stage ? stage : "");
	if (!stage)
		Cvar_SetDirect(cl_loadmap, "");
	else if (map)
		Cvar_SetDirect(cl_loadmap, map);
	Cvar_SetDirect(cl_loadprogress, va("%g", stage ? frac : 0.0f));
	if (!stage || loading_drawing || !R_Active())
		return;
	/* the main loop isn't drawing while a load blocks it: draw from here */
	loading_drawing = true;
	R_BeginFrame();
	R_GetSize(&w, &h);
	MQC_Frame(w, h);
	R_EndFrame();
	loading_drawing = false;
}

bool MQC_Loading(void) { return cl_loadstage && cl_loadstage->string[0]; }

void MQC_Frame(int width, int height)
{
	if (!M.vm)
		return;
	set_time();
	QC_SetGVec(M.vm, OFS_PARM(0), (const float[3]){(float)width, (float)height, 0});
	if ((MQC_Loading() || (cl.state > ca_disconnected && cl.state < ca_active)) && M.drawloading) {
		M.vm->globals[OFS_PARM(1)].f = 0;
		call(M.drawloading, 2);
	} else if (M.draw) {
		/* called with or without focus; the menu decides whether to draw */
		call(M.draw, 1);
	}
	QC_CollectGarbage(M.vm, false);
}

/* keys and mouse go to the menu first while it holds the key focus */
bool MQC_InputEvent(int type, float a, float b, int device)
{
	if (!MQC_Active() || !M.input)
		return false;
	set_time();
	M.vm->globals[OFS_PARM(0)].f = (float)type;
	M.vm->globals[OFS_PARM(1)].f = a;
	M.vm->globals[OFS_PARM(2)].f = b;
	M.vm->globals[OFS_PARM(3)].f = (float)device;
	M.vm->argc = 4;
	if (!QC_Execute(M.vm, M.input))
		return true;
	/* a grabbed menu eats everything, whatever it returns */
	return M.vm->globals[OFS_RETURN].f != 0 || M.keydest == KEYDEST_MENU_GRABBED;
}

bool MQC_ConsoleCommand(const char *line)
{
	cmd_tokens_t *t;
	bool known = cmd_forced;
	if (!M.vm || !M.consolecommand)
		return false;
	t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, line);
	for (int i = 0; t->argc && i < M.numregistered; i++)
		if (!Str_ICmp(M.registered[i], t->argv[0]))
			known = true;
	Mem_Free(t);
	if (!known)
		return false;
	set_time();
	M.vm->globals[OFS_PARM(0)].i = QC_NewString(M.vm, line);
	call(M.consolecommand, 1);
	return true;
}

static void Menu_Toggle_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	MQC_Toggle(-1);
}

static bool load_pending;
static double load_deadline;

/* loads the menu once the game being started is in, so that its init sees
 * clientstate() 2 and skips the background map and intro it would otherwise
 * queue in front of the game */
void MQC_LoadWhenActive(void)
{
	load_pending = true;
	load_deadline = Sys_Seconds() + 30;
}

void MQC_PendingLoad(void)
{
	if (load_pending && (cl.state == ca_active || Sys_Seconds() > load_deadline)) {
		load_pending = false;
		MQC_Load();
	}
}

static void Menu_Restart_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (sv.active && cl.state != ca_active)
		MQC_LoadWhenActive(); /* a map is starting */
	else if (!MQC_Load())
		Con_Printf("no menu.dat\n");
}

void MQC_Init(void)
{
	cl_loadstage = Cvar_Get("cl_loadstage", "", CVAR_READONLY, "The step of the map load in progress, empty when none");
	cl_loadmap = Cvar_Get("cl_loadmap", "", CVAR_READONLY, "The map being loaded");
	cl_loadprogress = Cvar_Get("cl_loadprogress", "0", CVAR_READONLY, "How far the map load is, 0-1, from counted items");
	Host_SetLoadingHook(loading_hook);
	Cmd_Add("togglemenu", Menu_Toggle_f);
	Cmd_Add("menu_restart", Menu_Restart_f);
	/* FTE's reloads a Quake 3 ui.qvm; the menu module here is menu.dat */
	Cmd_AddD("ui_restart", Menu_Restart_f, "Reload the menu module (menu.dat; there is no Quake 3 ui.qvm)");
}

bool MQC_ForceCommand(const char *line)
{
	bool r;
	cmd_forced = true;
	r = MQC_ConsoleCommand(line);
	cmd_forced = false;
	return r;
}
