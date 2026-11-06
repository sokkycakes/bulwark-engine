/* Bulwark engine - dedicated server entry point. ISC licence, see LICENSE. */
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "common/cmd.h"
#include "common/cvar.h"
#include "common/fs.h"
#include "common/manifest.h"
#include "common/worker.h"
#include "net/net.h"
#include "net/live.h"
#include "net/pkg.h"
#include "server/sv_local.h"

static bool quit_requested;

static void Path_f(int argc, const char **argv) { FS_PrintPath(); }

static void Quit_f(int argc, const char **argv) { quit_requested = true; }

static bool ServerFallback(const char *line, int argc, const char **argv) { return SV_ConsoleCommand(line); }

static void usage(void)
{
	Con_Printf("usage: bulwark-ded [-basedir dir] [-game dir]... [-enginedata dir] [-frames n] [+command args]...\n");
}

int main(int argc, char **argv)
{
	const char *basedir = ".";
	const char *games[16];
	int numgames = 0;
	long maxframes = -1;
	strbuf_t cmds = {0};

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-basedir") && i + 1 < argc)
			basedir = argv[++i];
		else if (!strcmp(argv[i], "-game") && i + 1 < argc && numgames < 16)
			games[numgames++] = argv[++i];
		else if (!strcmp(argv[i], "-enginedata") && i + 1 < argc)
			FS_SetEngineDir(argv[++i]);
		else if (!strcmp(argv[i], "-frames") && i + 1 < argc)
			maxframes = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-developer"))
			developer = 1;
		else if (argv[i][0] == '+') {
			/* "+map foo +set x 1" -> one command per '+' */
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
	FS_Init(basedir);
	Mf_Startup(games, numgames);
	NET_Init();
	Live_Init();
	PKG_Init();
	SV_Init();
	QC_InitDebugCommands();
	Cmd_Add("quit", Quit_f);
	Cmd_Add("path", Path_f);
	Cmd_DescribeAll();
	Cmd_ProtectLocal();
	Plug_AutoLoad();
	Cmd_SetPreHook(ServerFallback);

	if (FS_FileExists("server.cfg"))
		Cbuf_AddText("exec server.cfg\n");
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	SB_Free(&cmds);
	Cbuf_Execute();

	double last = Sys_Seconds();
	long frames = 0;
	while (!quit_requested) {
		double now, dt;
		Cbuf_Execute();
		if (maxframes >= 0) {
			/* headless test runs step a fixed tick without sleeping */
			if (frames++ >= maxframes)
				break;
			dt = 1.0 / (Cvar_Value("sv_tickrate") > 0 ? Cvar_Value("sv_tickrate") : 72);
		} else {
			Sys_Sleep(0.001);
			now = Sys_Seconds();
			dt = now - last;
			last = now;
		}
		host_time += dt;
		NET_RunDelayed();
		Live_Frame();
		PKG_Frame();
		Worker_Frame();
		Plug_Frame();
		QC_URIFrame();
		SV_Frame(dt);
	}
	if (maxframes >= 0)
		Cmd_ExecuteString("status");
	Plug_Shutdown();
	if (sv.active)
		SV_DropClients("server shut down");
	SV_Shutdown();
	Live_Shutdown();
	QC_URIDrain(3);
	return 0;
}
