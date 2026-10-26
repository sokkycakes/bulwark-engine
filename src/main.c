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
#include "ui/ui.h"

static bool quit_requested;

static void Quit_f(int argc, const char **argv) { quit_requested = true; }

/* commands the game modules registered come before the engine's own */
static bool GamePreHook(const char *line, int argc, const char **argv)
{
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
	bool fullscreen = false;
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
			width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-height") && i + 1 < argc)
			height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-fullscreen"))
			fullscreen = true;
		else if (!strcmp(argv[i], "-window"))
			fullscreen = false;
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
	IN_Init();
	if (!headless && !VID_Init(width, height, fullscreen)) {
		Con_Printf("no video, running headless\n");
		headless = true;
	}
	R_Init(headless);
	S_Init(headless);
	UI_Init();
	UI_InitQC();
	Cmd_Add("quit", Quit_f);
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
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	SB_Free(&cmds);

	double last = Sys_Seconds();
	long frames = 0;
	bool server_was_active = false;
	while (!quit_requested) {
		double now, dt;
		if (maxframes >= 0) {
			/* scripted runs step a fixed frame time without sleeping */
			if (frames++ >= maxframes)
				break;
			dt = 1.0 / 72;
		} else {
			now = Sys_Seconds();
			dt = now - last;
			if (dt < 0.001) {
				Sys_Sleep(0.0005);
				continue;
			}
			last = now;
		}
		if (!headless && !VID_PumpEvents())
			break;
		Cbuf_Execute();

		/* a map started here: join it over loopback */
		if (sv.active && !server_was_active) {
			cl.listen = true;
			CL_Connect("localhost");
		}
		if (!sv.active && server_was_active)
			cl.listen = false;
		server_was_active = sv.active;

		host_time += dt;
		NET_RunDelayed();
		SV_Frame(dt);
		CL_Frame(dt);
		R_BeginFrame();
		{
			int w, h;
			R_GetSize(&w, &h);
			CSQC_Frame(w, h);
		}
		UI_Frame(dt);
		UI_Draw();
		R_EndFrame();
		S_Update(NULL, NULL, NULL, NULL);
	}
	if (maxframes >= 0)
		Cmd_ExecuteString("netstats");
	CL_Disconnect("quit");
	SV_Shutdown();
	UI_Shutdown();
	S_Shutdown();
	R_Shutdown();
	VID_Shutdown();
	return 0;
}
