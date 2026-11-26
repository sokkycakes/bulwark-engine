/* Bulwark engine - game client entry point, with a listen server in the same
 * process. ISC licence, see LICENSE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "client/console.h"
#include "client/lobby.h"
#include "client/vid.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/fs.h"
#include "common/manifest.h"
#include "common/worker.h"
#include "net/net.h"
#include "net/pkg.h"
#include "platform/platform.h"
#include "render/render.h"
#include "server/sv_local.h"
#include "sound/sound.h"
#include "ui/ui.h"

static bool quit_requested;

static void Path_f(int argc, const char **argv) { FS_PrintPath(); }

static void Quit_f(int argc, const char **argv) { quit_requested = true; }

/* set when the command line starts a game: the menu's start-up request for
 * its background map (Nuclide: map_background) is dropped once, otherwise it
 * replaces the game the command line asked for */
static bool drop_menu_map;

/* commands the game modules registered come before the engine's own */
static bool GamePreHook(const char *line, int argc, const char **argv)
{
	/* the UI first: the native one turns the rules' hero menu request into
	 * its own picker before the client game opens its mouse-only one */
	if (UI_PreCommand(argc, argv))
		return true;
	if (drop_menu_map && argc > 0 && !strcmp(argv[0], "map_background")) {
		drop_menu_map = false;
		Con_DPrintf("the command line starts a game: menu background map skipped\n");
		return true;
	}
	if (MQC_ConsoleCommand(line))
		return true;
	if (CSQC_ConsoleCommand(line))
		return true;
	return SV_ConsoleCommand(line);
}

static void usage(void)
{
	Con_Printf("usage: bulwark [-basedir dir] [-game dir]... [-enginedata dir] [-headless] [-frames n] [-width w] [-height h]\n"
	           "               [-window] [+command args]...\n");
}

static cvar_t *host_speeds;

static void host_speeds_add(const double *t)
{
	static const char *names[7] = {"misc", "server", "client", "csqc+3d", "ui", "2d+swap", "sound"};
	static double sum[7], since, worst[8], worst_files, worst_sv[SVP_COUNT], worst_cmd;
	static char worst_cmd_who[64];
	static int frames, worst_ticks, worst_loads, noticks, moreticks;
	int loads;
	double files;
	FS_TakeLoadStats(&loads, &files);
	for (int i = 0; i < 7; i++)
		sum[i] += t[i + 1] - t[i];
	/* the slowest frame's parts, to tell what makes the spikes */
	if (t[7] - t[0] > worst[7]) {
		for (int i = 0; i < 7; i++)
			worst[i] = t[i + 1] - t[i];
		worst[7] = t[7] - t[0];
		worst_ticks = sv_frameticks;
		worst_files = files, worst_loads = loads;
		memcpy(worst_sv, sv_phase_seconds, sizeof(worst_sv));
		worst_cmd = sv_slowest_cmd;
		Str_Copy(worst_cmd_who, sv_slowest_cmd_who, sizeof(worst_cmd_who));
	}
	if (sv.active) {
		noticks += sv_frameticks == 0;
		moreticks += sv_frameticks > 1;
	}
	frames++;
	if (!since)
		since = t[0];
	if (t[7] - since >= 5 && frames) {
		char line[384];
		int o = snprintf(line, sizeof(line), "host_speeds: %.1f fps;", frames / (t[7] - since));
		for (int i = 0; i < 7 && o < (int)sizeof(line); i++)
			o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.1f", names[i], sum[i] * 1000 / frames);
		Con_Printf("%s ms\n", line);
		/* not "host_speeds:", which scripts count reports by */
		o = snprintf(line, sizeof(line),
		             "host_speeds slowest %.1f ms (%d server frames, %d files %.1f ms; %d frames ran no server frame, "
		             "%d more than one):",
		             worst[7] * 1000, worst_ticks, worst_loads, worst_files * 1000, noticks, moreticks);
		for (int i = 0; i < 7 && o < (int)sizeof(line); i++)
			o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.1f", names[i], worst[i] * 1000);
		Con_Printf("%s\n", line);
		o = snprintf(line, sizeof(line), "host_speeds slowest server:");
		for (int i = 0; i < SVP_COUNT && o < (int)sizeof(line); i++)
			o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.1f", sv_phase_names[i], worst_sv[i] * 1000);
		if (worst_cmd > 0 && o < (int)sizeof(line))
			snprintf(line + o, sizeof(line) - (size_t)o, "; longest command %.1f (%s)", worst_cmd * 1000, worst_cmd_who);
		Con_Printf("%s\n", line);
		memset(sum, 0, sizeof(sum));
		memset(worst, 0, sizeof(worst));
		noticks = moreticks = 0;
		frames = 0;
		since = t[7];
	}
}

int main(int argc, char **argv)
{
	const char *basedir;
	const char *games[16];
	int numgames = 0;
	long maxframes = -1;
	bool headless = false;
	int width = 1280, height = 720;
	bool fullscreen = false, size_given = false;
	strbuf_t cmds = {0};

	Plat_Init(&argc, &argv);
	basedir = Plat_DefaultBaseDir();

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-basedir") && i + 1 < argc)
			basedir = argv[++i];
		else if (!strcmp(argv[i], "-game") && i + 1 < argc && numgames < 16)
			games[numgames++] = argv[++i];
		else if (!strcmp(argv[i], "-enginedata") && i + 1 < argc)
			FS_SetEngineDir(argv[++i]);
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
	Mf_Startup(games, numgames);
	NET_Init();
	PKG_Init();
	SV_Init();
	QC_InitDebugCommands();
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
	UI_Init();
	UI_InitQC();
	WC_Init();
	Lobby_Init();
	Cmd_Add("quit", Quit_f);
	Cmd_AddD("exit", Quit_f, "Quit the game");
	cvar_t *fps_max = Cvar_Get("fps_max", "0", CVAR_ARCHIVE, "Frame rate cap, 0 for none");
	cvar_t *cl_maxfps = Cvar_Get("cl_maxfps", "0", CVAR_ARCHIVE, "Frame rate cap (FTE's name), used when fps_max is 0");
	cvar_t *host_timescale = Cvar_Get("host_timescale", "1", CVAR_CHEAT, "Game speed multiplier for a local server");
	host_speeds = Cvar_Get("host_speeds", "0", 0, "Print where the frame time goes (milliseconds per frame) every 5 seconds");
	cvar_t *host_framerate = Cvar_Get("host_framerate", "0", 0, "Fixed seconds per frame when above 0, for recording");
	Cmd_Add("path", Path_f);
	Cmd_DescribeAll();
	Cmd_ProtectLocal();
	Plug_AutoLoad();
	Cmd_SetPreHook(GamePreHook);
	Cmd_SetFallback(CL_ForwardToServer);

	Mf_ExecConfigs();
#ifdef BW_XBOX
	/* the console's own settings over the game's (data/xbox_ui.cfg in the
	 * engine data the disc carries): after autoexec.cfg, before the title */
	if (FS_FileExists("xbox_ui.cfg"))
		Cbuf_AddText("exec xbox_ui.cfg\n");
#endif
	/* the front end starts on its menu unless the command line starts a game */
	bool starts_game = false;
	for (const char *l = cmds.data; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
		if (!strncmp(l, "map ", 4) || !strncmp(l, "connect ", 8) || !strncmp(l, "playdemo ", 9))
			starts_game = true;
	}
	drop_menu_map = starts_game;
	WC_SetStartsGame(starts_game);
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	SB_Free(&cmds);
	/* when the command line starts a game, the menu loads once that game is
	 * running, so its init sees clientstate() 2 and skips the background
	 * map and intro it would otherwise queue in front of the game */
	if (starts_game)
		MQC_LoadWhenActive();
	else if (MQC_Load())
		MQC_Toggle(1);

	double last = Sys_Seconds();
	long frames = 0;
	bool server_was_active = false, mode_applied = false;
	int seen_spawncount = 0;
	while (!quit_requested) {
		double now, dt;
		if (maxframes >= 0) {
			/* scripted runs step a fixed frame time without sleeping */
			if (frames++ >= maxframes)
				break;
			dt = 1.0 / 72;
		} else {
#ifdef BW_XBOX
			/* the flip paces frames to vblanks (vid_xbox_swapinterval); a
			 * cap here as well (a config's fps_max 30) beats against it,
			 * sleeping past the vblank a frame was due for */
			float cap = 0;
			(void)fps_max, (void)cl_maxfps;
#else
			float cap = fps_max->value > 0 ? fps_max->value : cl_maxfps->value;
#endif
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

		/* a map started here: join it over loopback. That includes a new
		 * map started after "disconnect", which leaves the server running */
		if (sv.active && (!server_was_active || (sv_spawncount != seen_spawncount && cl.state == ca_disconnected))) {
			cl.listen = true;
			CL_Connect("localhost");
		}
		seen_spawncount = sv_spawncount;
		if (!sv.active && server_was_active)
			cl.listen = false;
		server_was_active = sv.active;
		MQC_PendingLoad();

		host_time += dt;
		/* a local game can run slow or fast; a remote server sets its own pace */
		double gdt = sv.active && host_timescale->value > 0 ? dt * BW_CLAMP(host_timescale->value, 0.01f, 10) : dt;
		/* host_speeds 1: where the frame goes, every 5 seconds */
		double hs[8];
#define HS(i) (hs[i] = host_speeds->ivalue ? Sys_Seconds() : 0)
		if (host_speeds->ivalue) {
			int loads;
			double files;
			FS_TakeLoadStats(&loads, &files); /* count from the frame's start */
		}
		memset(sv_phase_seconds, 0, sizeof(sv_phase_seconds));
		sv_slowest_cmd = 0;
		sv_frameticks = 0;
		HS(0);
		NET_RunDelayed();
		PKG_Frame();
		Worker_Frame();
		Plug_Frame();
		QC_URIFrame();
		HS(1);
		SV_Frame(gdt);
		HS(2);
		CL_Frame(gdt);
		Lobby_Frame();
		WC_Frame();
		HS(3);
		R_BeginFrame();
		{
			int w, h;
			R_GetSize(&w, &h);
			CSQC_Frame(w, h);
			MQC_Frame(w, h);
		}
		HS(4);
		UI_Frame(dt);
		UI_Draw();
		HS(5);
		R_EndFrame();
		HS(6);
		S_Update(NULL, NULL, NULL, NULL);
		HS(7);
#undef HS
		if (host_speeds->ivalue)
			host_speeds_add(hs);
	}
	if (maxframes >= 0)
		Cmd_ExecuteString("netstats");
	/* scripted runs never touch the player's config */
	if (maxframes < 0)
		Cfg_SaveIfModified();
	CL_Disconnect("quit");
	MQC_Shutdown();
	Plug_Shutdown();
	if (sv.active)
		SV_DropClients("server shut down");
	SV_Shutdown();
	QC_URIDrain(3);
	UI_Shutdown();
	S_Shutdown();
	R_Shutdown();
	VID_Shutdown();
	Plat_Shutdown();
	return 0;
}
