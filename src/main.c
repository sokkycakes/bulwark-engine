/* Bulwark engine - game client entry point, with a listen server in the same
 * process. ISC licence, see LICENSE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

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
#include "net/live.h"
#include "net/pkg.h"
#include "platform/platform.h"
#include "render/render.h"
#include "server/sv_local.h"
#include "sound/sound.h"
#include "ui/ui.h"

static bool quit_requested;
/* SIGINT/SIGTERM in a headless client: nothing pumps SDL's events there, so
 * the SDL_QUIT that SDL's own handlers queue would never be read */
static volatile sig_atomic_t quit_signalled;

static void Path_f(int argc, const char **argv) { FS_PrintPath(); }

/* the first signal quits as "quit" does; a second one doesn't wait */
static void on_signal(int sig)
{
	if (quit_signalled)
		_Exit(1);
	quit_signalled = 1;
	signal(sig, on_signal);
}

static void Quit_f(int argc, const char **argv) { quit_requested = true; }

/* set when the command line starts a game: the menu's start-up request for
 * its background map (Nuclide: map_background) is dropped once, otherwise it
 * replaces the game the command line asked for */
static bool drop_menu_map;

/* the menu module loads once the startup configs have run, as FTE starts
 * menu.dat after quake.rc: a config's "set forceqmenu 1" then keeps it from
 * loading at all, and the module's own start-up commands (Nuclide's profile
 * cfg_load) come after autoexec.cfg instead of replacing what it set */
static bool menu_startup_starts_game, menu_startup_done;
static void MenuStartup_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (menu_startup_done)
		return;
	menu_startup_done = true;
	/* when the command line starts a game, the menu loads once that game is
	 * running, so its init sees clientstate() 2 and skips the background
	 * map and intro it would otherwise queue in front of the game */
	if (menu_startup_starts_game)
		MQC_LoadWhenActive();
	else if (MQC_VM())
		MQC_Toggle(1); /* a config's menu_restart already loaded it */
	else if (MQC_Load())
		MQC_Toggle(1);
}

/* commands the game modules registered come before the engine's own */
static bool GamePreHook(const char *line, int argc, const char **argv)
{
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
	Con_Printf("usage: bulwark [-basedir dir] [-game dir]... [-enginedata dir] [-userdir dir] [-headless] [-frames n] [-width w] [-height h]\n"
	           "               [-window] [+command args]...\n"
	           "       bulwark -dedicated [maxplayers] [-port n] ...: run a dedicated server (see bulwark-ded -h)\n");
}

/* Bulwark Live: whether this game is in a match's game */
static bool live_in_game(void)
{
	return sv.active || cl.state == ca_active;
}

/* --- the frame loop ----------------------------------------------------------
 * main() sets up, then runs Host_Frame until it returns false. In a browser
 * the page's animation frames call it instead (Host_BrowserFrame). */
static double host_last;
static long host_maxframes = -1, host_frames;
static bool host_headless, host_size_given, host_mode_applied, host_server_was_active;
static int host_seen_spawncount;
static cvar_t *host_fps_max, *host_cl_maxfps, *host_timescale_cvar, *host_framerate_cvar, *host_idlefps, *host_speeds;
/* host_speeds: summed milliseconds of each part of the frame, printed each second */
static double speeds_sv, speeds_cl, speeds_gfx, speeds_snd, speeds_since;
static int speeds_frames;

static void Host_Shutdown(void);

/* one frame; false once the game should quit */
static bool Host_Frame(void)
{
	double now, dt;
	if (quit_signalled)
		quit_requested = true;
	if (quit_requested)
		return false;
	if (host_maxframes >= 0) {
		/* scripted runs step a fixed frame time without sleeping */
		if (host_frames++ >= host_maxframes)
			return false;
		dt = 1.0 / 72;
	} else {
		float cap = host_fps_max->value > 0 ? host_fps_max->value : host_cl_maxfps->value;
		/* in the background (alt-tabbed, minimised) frames cost the
		 * machine and show nothing: cl_idlefps caps them, as FTE does */
		if (!host_headless && host_idlefps->value > 0 && !VID_Focused() && (cap <= 0 || cap > host_idlefps->value))
			cap = host_idlefps->value;
		double mindt = cap > 0 ? 1.0 / BW_MAX(cap, 10) : 0.001;
		now = Sys_Seconds();
		dt = now - host_last;
#ifdef __EMSCRIPTEN__
		/* the browser paces frames to the display; a cap below its rate
		 * skips refreshes, and nothing may sleep */
		if (cap > 0 && dt < mindt - 0.002)
			return true;
		if (dt <= 0)
			return true;
		/* a background tab gets no frames: don't simulate the gap in one step */
		if (dt > 0.25)
			dt = 0.25;
#else
		if (dt < mindt) {
			/* sleep most of the gap, then spin the rest for an even rate */
			if (mindt - dt > 0.002)
				Sys_Sleep(mindt - dt - 0.001);
			return true;
		}
#endif
		host_last = now;
		host_frames++;
		if (host_framerate_cvar->value > 0)
			dt = host_framerate_cvar->value;
	}
	if (!host_headless && !VID_PumpEvents())
		return false;
	Cbuf_Execute();
	/* the saved video mode applies once the configs have run, unless
	 * the command line chose one */
	if (host_frames <= 1 && host_maxframes < 0 && !host_size_given && !host_headless && !host_mode_applied) {
		host_mode_applied = true;
		VID_ApplyMode();
	}

	/* a map started here: join it over loopback. That includes a new
	 * map started after "disconnect", which leaves the server running */
	if (sv.active && (!host_server_was_active || (sv_spawncount != host_seen_spawncount && cl.state == ca_disconnected))) {
		cl.listen = true;
		CL_Connect("localhost");
	}
	host_seen_spawncount = sv_spawncount;
	if (!sv.active && host_server_was_active)
		cl.listen = false;
	host_server_was_active = sv.active;
	MQC_PendingLoad();

	host_time += dt;
	/* a local game can run slow or fast; a remote server sets its own pace */
	double gdt = sv.active && host_timescale_cvar->value > 0 ? dt * BW_CLAMP(host_timescale_cvar->value, 0.01f, 10) : dt;
	NET_RunDelayed();
	Live_Frame();
	PKG_Frame();
	Worker_Frame();
	Plug_Frame();
	QC_URIFrame();
	double t0 = host_speeds->ivalue ? Sys_Seconds() : 0;
	SV_Frame(gdt);
	double t1 = host_speeds->ivalue ? Sys_Seconds() : 0;
	CL_Frame(gdt);
	Lobby_Frame();
	WC_Frame();
	double t2 = host_speeds->ivalue ? Sys_Seconds() : 0;
	R_BeginFrame();
	{
		int w, h;
		R_GetSize(&w, &h);
		CSQC_Frame(w, h);
		MQC_Frame(w, h);
	}
	UI_Frame(dt);
	UI_Draw();
	R_EndFrame();
	double t3 = host_speeds->ivalue ? Sys_Seconds() : 0;
	S_Update(NULL, NULL, NULL, NULL);
	if (host_speeds->ivalue) {
		double t4 = Sys_Seconds();
		speeds_sv += t1 - t0, speeds_cl += t2 - t1, speeds_gfx += t3 - t2, speeds_snd += t4 - t3;
		if (!speeds_since)
			speeds_since = t0;
		if (++speeds_frames, t4 - speeds_since >= 1) {
			double n = speeds_frames;
			Con_Printf("host_speeds: %d fps, %.2f ms/frame: server %.2f client %.2f gfx %.2f snd %.2f\n", speeds_frames,
			           (t4 - speeds_since) * 1000 / n, speeds_sv * 1000 / n, speeds_cl * 1000 / n, speeds_gfx * 1000 / n,
			           speeds_snd * 1000 / n);
			speeds_sv = speeds_cl = speeds_gfx = speeds_snd = 0;
			speeds_frames = 0;
			speeds_since = t4;
		}
	} else
		speeds_since = 0, speeds_frames = 0;
	return !quit_requested;
}

#ifdef __EMSCRIPTEN__
/* for the page (tools/web/shell): */
/* it's being hidden or closed: save the config now, as quitting would */
EMSCRIPTEN_KEEPALIVE void bulwark_page_hidden(void)
{
	if (host_maxframes < 0)
		Cfg_SaveIfModified();
}

/* a console command from the page */
EMSCRIPTEN_KEEPALIVE void bulwark_command(const char *text)
{
	Cbuf_AddText(text);
	Cbuf_AddText("\n");
}

/* for the page's tests: 0 disconnected .. 3 in the game (cl.state), and the
 * frames run */
EMSCRIPTEN_KEEPALIVE int bulwark_client_state(void) { return (int)cl.state; }
EMSCRIPTEN_KEEPALIVE double bulwark_frames(void) { return (double)host_frames; }

static void Host_BrowserFrame(void)
{
	if (Host_Frame())
		return;
	emscripten_cancel_main_loop();
	Host_Shutdown();
	Plat_Quit();
}
#endif

static void Host_Shutdown(void)
{
	if (host_maxframes >= 0)
		Cmd_ExecuteString("netstats");
	/* scripted runs never touch the player's config */
	if (host_maxframes < 0)
		Cfg_SaveIfModified();
	CL_Disconnect("quit");
	MQC_Shutdown();
	Plug_Shutdown();
	if (sv.active)
		SV_DropClients("server shut down");
	SV_Shutdown();
	Live_Shutdown();
	QC_URIDrain(3);
	UI_Shutdown();
	S_Shutdown();
	R_Shutdown();
	VID_Shutdown();
	Plat_Shutdown();
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

#ifndef __EMSCRIPTEN__
	/* "-dedicated", as in FTE: run as a dedicated server, no window */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-dedicated"))
			return Ded_Main(argc, argv);
#endif
	Plat_Init(&argc, &argv);
	basedir = Plat_DefaultBaseDir();

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-basedir") && i + 1 < argc)
			basedir = argv[++i];
		else if (!strcmp(argv[i], "-game") && i + 1 < argc && numgames < 16)
			games[numgames++] = argv[++i];
		else if (!strcmp(argv[i], "-enginedata") && i + 1 < argc)
			FS_SetEngineDir(argv[++i]);
		else if (!strcmp(argv[i], "-userdir") && i + 1 < argc)
			FS_SetUserDir(argv[++i]);
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
	Live_Init();
	Live_SetGameProbe(live_in_game);
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
	/* A headless client handles Ctrl+C and SIGTERM itself, as the dedicated
	 * server does. SDL may already have put in handlers that only queue
	 * SDL_QUIT (the microphone probe starts its audio, and with it the event
	 * system), and a windowed client pumps those events, so it keeps them. */
	if (headless) {
		signal(SIGINT, on_signal);
		signal(SIGTERM, on_signal);
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
	host_speeds = Cvar_Get("host_speeds", "0", 0, "1 prints, each second, the average milliseconds a frame spends in the server, the client, drawing and sound");
	Cvar_Get("gl_clear", "0", 0, "Accepted for FTE configs; Bulwark clears the frame before drawing anyway");
	host_idlefps = Cvar_Get("cl_idlefps", "60", CVAR_ARCHIVE, "Frame rate cap while the window is in the background (alt-tabbed or minimised), 0 for none");
	cvar_t *host_timescale = Cvar_Get("host_timescale", "1", CVAR_CHEAT, "Game speed multiplier for a local server");
	cvar_t *host_framerate = Cvar_Get("host_framerate", "0", 0, "Fixed seconds per frame when above 0, for recording");
	Cmd_Add("path", Path_f);
	Cmd_DescribeAll();
	Cmd_ProtectLocal();
	Plug_AutoLoad();
	Cmd_SetPreHook(GamePreHook);
	Cmd_SetFallback(CL_ForwardToServer);

	Mf_ExecConfigs();
	/* the front end starts on its menu unless the command line starts a game */
	bool starts_game = false;
	for (const char *l = cmds.data; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
		if (!strncmp(l, "map ", 4) || !strncmp(l, "connect ", 8) || !strncmp(l, "playdemo ", 9))
			starts_game = true;
	}
	drop_menu_map = starts_game;
	WC_SetStartsGame(starts_game);
	menu_startup_starts_game = starts_game;
	Cmd_AddD("_menu_startup", MenuStartup_f, "Engine start-up step: loads the menu module once the startup configs have run");
	Cbuf_AddText("_menu_startup\n");
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	SB_Free(&cmds);

	host_last = Sys_Seconds();
	host_maxframes = maxframes;
	host_headless = headless;
	host_size_given = size_given;
	host_fps_max = fps_max;
	host_cl_maxfps = cl_maxfps;
	host_timescale_cvar = host_timescale;
	host_framerate_cvar = host_framerate;
#ifdef __EMSCRIPTEN__
	/* the browser owns the loop: one frame per display refresh, and the
	 * page keeps running after main returns */
	if (maxframes < 0) {
		emscripten_set_main_loop(Host_BrowserFrame, 0, 0);
		return 0;
	}
#endif
	while (Host_Frame())
		;
	Host_Shutdown();
	return 0;
}
