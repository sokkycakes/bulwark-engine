/* Bulwark engine - game client entry point, with a listen server in the same
 * process. ISC licence, see LICENSE. */
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "client/console.h"
#include "client/vid.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/fs.h"
#include "net/net.h"
#include "render/render.h"
#include "server/sv_local.h"
#include "sound/sound.h"

static bool quit_requested;

static void Path_f(int argc, const char **argv) { FS_PrintPath(); }

static void Quit_f(int argc, const char **argv) { quit_requested = true; }

/* commands the game modules registered come before the engine's own */
static bool GamePreHook(const char *line, int argc, const char **argv)
{
	if (MQC_ConsoleCommand(line))
		return true;
	if (CSQC_ConsoleCommand(line))
		return true;
	return SV_ConsoleCommand(line);
}

static void usage(void)
{
	Con_Printf("usage: bulwark [-basedir dir] [-game dir]... [-headless] [-frames n] [-width w] [-height h]\n"
	           "               [-window] [+command args]...\n");
}

int main(int argc, char **argv)
{
	const char *basedir = ".";
	const char *games[16];
	int numgames = 0;
	long maxframes = -1;
	bool headless = false;
	int width = 1280, height = 720;
	bool fullscreen = false, size_given = false;
	strbuf_t cmds = {0};

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-basedir") && i + 1 < argc)
			basedir = argv[++i];
		else if (!strcmp(argv[i], "-game") && i + 1 < argc && numgames < 16)
			games[numgames++] = argv[++i];
		else if (!strcmp(argv[i], "-frames") && i + 1 < argc)
			maxframes = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-headless"))
			headless = true;
		else if (!strcmp(argv[i], "-width") && i + 1 < argc)
			width = atoi(argv[++i]), size_given = true;
		else if (!strcmp(argv[i], "-height") && i + 1 < argc)
			height = atoi(argv[++i]), size_given = true;
		else if (!strcmp(argv[i], "-fullscreen"))
			fullscreen = true, size_given = true;
		else if (!strcmp(argv[i], "-window"))
			fullscreen = false, size_given = true;
		else if (!strcmp(argv[i], "-developer"))
			developer = 1;
		else if (argv[i][0] == '+') {
			SB_AppendStr(&cmds, argv[i] + 1);
			while (i + 1 < argc && argv[i + 1][0] != '+' && argv[i + 1][0] != '-') {
				SB_AppendStr(&cmds, " \"");
				SB_AppendStr(&cmds, argv[++i]);
				SB_AppendStr(&cmds, "\"");
			}
			SB_AppendStr(&cmds, "\n");
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else {
			Con_Printf("unknown argument %s\n", argv[i]);
			usage();
			return 1;
		}
	}

	Cvar_Init();
	Cmd_Init();
	Con_Init();
	FS_Init(basedir);
	if (!numgames)
		games[numgames++] = "base";
	for (int i = 0; i < numgames; i++)
		FS_AddGameDir(games[i]);
	NET_Init();
	SV_Init();
	CL_Init();
	CL_InitCommands();
	MQC_Init();
	IN_Init();
	if (!headless && !VID_Init(width, height, fullscreen)) {
		Con_Printf("no video, running headless\n");
		headless = true;
	}
	R_Init(headless);
	S_Init(headless);
	Cmd_Add("quit", Quit_f);
	Cmd_AddD("exit", Quit_f, "Quit the game");
	cvar_t *fps_max = Cvar_Get("fps_max", "0", CVAR_ARCHIVE, "Frame rate cap, 0 for none");
	cvar_t *cl_maxfps = Cvar_Get("cl_maxfps", "0", CVAR_ARCHIVE, "Frame rate cap (FTE's name), used when fps_max is 0");
	cvar_t *host_timescale = Cvar_Get("host_timescale", "1", CVAR_CHEAT, "Game speed multiplier for a local server");
	cvar_t *host_framerate = Cvar_Get("host_framerate", "0", 0, "Fixed seconds per frame when above 0, for recording");
	Cmd_Add("path", Path_f);
	Cmd_SetPreHook(GamePreHook);
	Cmd_SetFallback(CL_ForwardToServer);

	if (FS_FileExists("default.cfg"))
		Cbuf_AddText("exec default.cfg\n");
	else if (FS_FileExists("default_controls.cfg"))
		Cbuf_AddText("exec default_controls.cfg\n");
	if (FS_FileExists("config.cfg"))
		Cbuf_AddText("exec config.cfg\n");
	if (FS_FileExists("autoexec.cfg"))
		Cbuf_AddText("exec autoexec.cfg\n");
	/* the front end starts on its menu unless the command line starts a game */
	bool starts_game = false;
	for (const char *l = cmds.data; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
		if (!strncmp(l, "map ", 4) || !strncmp(l, "connect ", 8) || !strncmp(l, "playdemo ", 9))
			starts_game = true;
	}
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	SB_Free(&cmds);
	/* when the command line starts a game, the menu loads once that game is
	 * running, so its init sees clientstate() 2 and skips the background
	 * map and intro it would otherwise queue in front of the game */
	bool menu_pending = starts_game;
	double menu_deadline = Sys_Seconds() + 30;
	if (!starts_game && MQC_Load())
		MQC_Toggle(1);

	double last = Sys_Seconds();
	long frames = 0;
	bool server_was_active = false, mode_applied = false;
	while (!quit_requested) {
		double now, dt;
		if (maxframes >= 0) {
			/* scripted runs step a fixed frame time without sleeping */
			if (frames++ >= maxframes)
				break;
			dt = 1.0 / 72;
		} else {
			float cap = fps_max->value > 0 ? fps_max->value : cl_maxfps->value;
			double mindt = cap > 0 ? 1.0 / BW_MAX(cap, 10) : 0.001;
			now = Sys_Seconds();
			dt = now - last;
			if (dt < mindt) {
				/* sleep most of the gap, then spin the rest for an even rate */
				if (mindt - dt > 0.002)
					Sys_Sleep(mindt - dt - 0.001);
				continue;
			}
			last = now;
			if (host_framerate->value > 0)
				dt = host_framerate->value;
		}
		if (!headless && !VID_PumpEvents())
			break;
		Cbuf_Execute();
		/* the saved video mode applies once the configs have run, unless
		 * the command line chose one */
		if (frames <= 1 && maxframes < 0 && !size_given && !headless && !mode_applied) {
			mode_applied = true;
			VID_ApplyMode();
		}

		/* a map started here: join it over loopback */
		if (sv.active && !server_was_active) {
			cl.listen = true;
			CL_Connect("localhost");
		}
		if (!sv.active && server_was_active)
			cl.listen = false;
		server_was_active = sv.active;
		if (menu_pending && (cl.state == ca_active || Sys_Seconds() > menu_deadline)) {
			menu_pending = false;
			MQC_Load();
		}

		host_time += dt;
		/* a local game can run slow or fast; a remote server sets its own pace */
		double gdt = sv.active && host_timescale->value > 0 ? dt * BW_CLAMP(host_timescale->value, 0.01f, 10) : dt;
		NET_RunDelayed();
		SV_Frame(gdt);
		CL_Frame(gdt);
		R_BeginFrame();
		{
			int w, h;
			R_GetSize(&w, &h);
			CSQC_Frame(w, h);
			MQC_Frame(w, h);
		}
		R_EndFrame();
		S_Update(NULL, NULL, NULL, NULL);
	}
	if (maxframes >= 0)
		Cmd_ExecuteString("netstats");
	/* scripted runs never touch the player's config */
	if (maxframes < 0)
		Cfg_SaveIfModified();
	CL_Disconnect("quit");
	MQC_Shutdown();
	SV_Shutdown();
	S_Shutdown();
	R_Shutdown();
	VID_Shutdown();
	return 0;
}
