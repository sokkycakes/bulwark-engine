/* Bulwark engine - the dedicated server host: start-up (the game's configs,
 * then server.cfg, then the command line), the console typed on the
 * terminal (stdin), and the frame loop. bulwark-ded runs it, and so does
 * "bulwark -dedicated", so testers can host with the game's own binary.
 * Behaviour is described in docs/spec/dedicated.md. ISC licence, see
 * LICENSE. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <pthread.h>
#elif defined(__3DS__) || defined(__NDS__) || defined(__EMSCRIPTEN__)
#define NO_CONSOLE_INPUT
#else
#include <poll.h>
#include <unistd.h>
#endif

#include "../common/common.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/manifest.h"
#include "../common/worker.h"
#include "../net/net.h"
#include "../net/live.h"
#include "../net/pkg.h"
#include "sv_local.h"

static volatile sig_atomic_t quit_requested;

static void Path_f(int argc, const char **argv) { FS_PrintPath(); }

static void Quit_f(int argc, const char **argv) { quit_requested = 1; }

/* Ctrl+C (or a service manager's SIGTERM) shuts down as "quit" does, so the
 * players are told; a second one doesn't wait */
static void on_signal(int sig)
{
	if (quit_requested)
		_Exit(1);
	quit_requested = 1;
	signal(sig, on_signal);
}

/* --- the typed console ------------------------------------------------------
 * Lines typed on the terminal (or piped in) run as console commands. Reading
 * never blocks the frame: POSIX polls fd 0, Windows reads it on a thread
 * (a console handle can't be polled for whole lines). End of input stops
 * reading; the server keeps running. */
#define CON_LINE 1024

#if defined(NO_CONSOLE_INPUT)
static void console_input(void) {}
#elif defined(_WIN32)
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static strbuf_t input_lines;
static bool input_started;

static void *input_thread(void *arg)
{
	char line[CON_LINE];
	(void)arg;
	while (fgets(line, sizeof(line), stdin)) {
		pthread_mutex_lock(&input_lock);
		SB_AppendStr(&input_lines, line);
		if (!strchr(line, '\n'))
			SB_AppendStr(&input_lines, "\n");
		pthread_mutex_unlock(&input_lock);
	}
	return NULL;
}

static void console_input(void)
{
	if (!input_started) {
		pthread_t t;
		input_started = true;
		if (!pthread_create(&t, NULL, input_thread, NULL))
			pthread_detach(t);
	}
	pthread_mutex_lock(&input_lock);
	if (input_lines.data && input_lines.data[0]) {
		Cbuf_AddText(input_lines.data);
		SB_Free(&input_lines);
	}
	pthread_mutex_unlock(&input_lock);
}
#else
static bool input_closed;
static char input_part[CON_LINE];
static size_t input_len;

static void console_input(void)
{
	char buf[512];
	while (!input_closed) {
		struct pollfd p = {0, POLLIN, 0};
		if (poll(&p, 1, 0) <= 0 || !(p.revents & (POLLIN | POLLHUP)))
			return;
		/* a server put in the background of a shell would be stopped
		 * (SIGTTIN) by reading the terminal: leave it to the foreground */
		if (isatty(0) && tcgetpgrp(0) != getpgrp())
			return;
		ssize_t n = read(0, buf, sizeof(buf));
		if (n <= 0) {
			input_closed = true;
			return;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\r')
				continue;
			if (buf[i] == '\n' || input_len + 1 >= sizeof(input_part)) {
				input_part[input_len] = 0;
				Cbuf_AddText(input_part);
				Cbuf_AddText("\n");
				input_len = 0;
				if (buf[i] == '\n')
					continue;
			}
			input_part[input_len++] = buf[i];
		}
	}
}
#endif

/* --- start-up order ----------------------------------------------------------
 * The game's configs run first (quake.rc and what it execs, as the client
 * does), then server.cfg, then the command line. A map that the configs or
 * server.cfg start is held until the command line has run, so +set on the
 * command line still applies to it; when the command line starts a map of
 * its own, the held one is dropped and only one map loads. That counts any
 * map run after server.cfg and before the held one is released: a literal
 * +map, or a map inside a cfg the command line execs ("+exec lobby.cfg"). */
static bool holding_maps, cmdline_has_map;
static char held_map[CON_LINE];

static bool starts_map(const char *cmd)
{
	return !Str_ICmp(cmd, "map") || !Str_ICmp(cmd, "changelevel") || !Str_ICmp(cmd, "devmap") ||
	       !Str_ICmp(cmd, "spmap") || !Str_ICmp(cmd, "gamemap");
}

static void CfgDone_f(int argc, const char **argv) { holding_maps = false; }

static void StartHeldMap_f(int argc, const char **argv)
{
	if (held_map[0]) {
		if (cmdline_has_map)
			Con_Printf("server.cfg: \"%s\" skipped, the command line starts a map\n", held_map);
		else
			Cbuf_InsertText(va("%s\n", held_map));
		held_map[0] = 0;
	} else if (!cmdline_has_map && !sv.active) {
		/* nothing started a map: the game's own start-up alias, as FTE's
		 * dedicated server runs it (Stiletto's server.cfg defines
		 * startmap_dm, so +map on the command line replaces it) */
		static const char *const starts[] = {"dedicated_start", "startmap_dm", "startmap_sp"};
		int level;
		for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++)
			if (Cmd_AliasLevel(starts[i], &level)) {
				Con_Printf("No map on the command line: running %s\n", starts[i]);
				Cbuf_InsertText(va("%s\n", starts[i]));
				return;
			}
		Con_Printf("No map is running. Type \"map <name>\" here (\"maps\" lists them), "
		           "or define a startmap_dm alias in server.cfg.\n");
	}
}

/* --- commands ------------------------------------------------------------- */

/* the game modules' registered commands first (see console.md), and maps
 * held during start-up */
static bool DedPreHook(const char *line, int argc, const char **argv)
{
	if (holding_maps && argc > 1 && starts_map(argv[0])) {
		Str_Copy(held_map, line, sizeof(held_map));
		Con_DPrintf("start-up: \"%s\" held until the command line has run\n", line);
		return true;
	}
	/* the command line's own map, however it got there (+map, or a map
	 * line in an +exec'd cfg): server.cfg's held map is dropped */
	if (!holding_maps && held_map[0] && argc > 1 && starts_map(argv[0]))
		cmdline_has_map = true;
	return SV_ConsoleCommand(line);
}

/* anything the engine doesn't know goes to the server game's ConsoleCmd, as
 * typed: Nuclide's addBot, way*, listEntityDef ... work without "sv" */
static bool DedFallback(const char *line, int argc, const char **argv)
{
	return SV_GameCommand(line);
}

/* client-only commands that shared configs (default_controls.cfg,
 * autoexec.cfg, a player's config.cfg) use: nothing to do on a server */
static void ClientOnly_f(int argc, const char **argv) {}

static void add_client_stubs(void)
{
	static const char *const names[] = {
		"bind", "unbind", "unbindall", "bindlevel", "in_bind", "setinfo", "menu_restart", "menu_cmd",
		"cl_cmd", "togglemenu", "toggleconsole", "vid_restart", "snd_restart", "r_restart", "cfg_load",
		"cfg_save", "showscores", "messagemode", "messagemode2", "headmodel", "team_model", "team_headmodel",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!Cmd_Exists(names[i]) && !Cvar_Find(names[i]))
			Cmd_AddD(names[i], ClientOnly_f, "A client command: does nothing on a dedicated server");
}

static void usage(void)
{
	Con_Printf("usage: bulwark-ded [-basedir dir] [-game dir]... [-enginedata dir] [-userdir dir] [-port n]\n"
	           "                   [-dedicated [maxplayers]] [-frames n] [-developer] [+command args]...\n"
	           "       bulwark -dedicated [same options]\n"
	           "Type console commands on the terminal; \"quit\" stops the server.\n");
}

/* Bulwark Live: whether this game is in a match's game */
static bool live_in_game(void)
{
	return sv.active;
}

static bool is_number(const char *s)
{
	if (!s || !*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

int Ded_Main(int argc, char **argv)
{
	const char *basedir = ".";
	const char *games[16];
	int numgames = 0;
	long maxframes = -1;
	strbuf_t cmds = {0}, early = {0};

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
		else if (!strcmp(argv[i], "-developer"))
			developer = 1;
		else if ((!strcmp(argv[i], "-port") || !strcmp(argv[i], "-udpport")) && i + 1 < argc)
			SB_Printf(&early, "set sv_port \"%s\"\n", argv[++i]);
		else if (!strcmp(argv[i], "-dedicated")) {
			/* Quake's "-dedicated 16": the slot count */
			if (i + 1 < argc && is_number(argv[i + 1]))
				SB_Printf(&early, "set maxplayers \"%s\"\n", argv[++i]);
		} else if (!strcmp(argv[i], "-headless") || !strcmp(argv[i], "-window") || !strcmp(argv[i], "-fullscreen"))
			; /* the client's options, so one launcher line serves both */
		else if ((!strcmp(argv[i], "-width") || !strcmp(argv[i], "-height")) && i + 1 < argc)
			i++;
		else if (argv[i][0] == '+') {
			/* "+map foo +set x 1" -> one command per '+' */
			const char *cmd = argv[i] + 1;
			if (starts_map(cmd))
				cmdline_has_map = true;
			SB_AppendStr(&cmds, cmd);
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

	/* a terminal shows the console's text, not its markup */
	Con_SetPlainStdout(true);
	Cvar_Init();
	Cmd_Init();
	FS_Init(basedir);
	Mf_Startup(games, numgames);
	NET_Init();
	Live_Init();
	Live_SetGameProbe(live_in_game);
	PKG_Init();
	sv_dedicated = true;
	SV_Init();
	QC_InitDebugCommands();
	Cmd_AddD("quit", Quit_f, "Stop the server");
	if (!Cmd_Exists("exit"))
		Cmd_AddD("exit", Quit_f, "Stop the server");
	Cmd_Add("path", Path_f);
	Cmd_Add("__ded_cfgdone", CfgDone_f);
	Cmd_Add("__ded_startmap", StartHeldMap_f);
	add_client_stubs();
	Cmd_DescribeAll();
	Cmd_ProtectLocal();
	Plug_AutoLoad();
	Cmd_SetPreHook(DedPreHook);
	Cmd_SetFallback(DedFallback);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	holding_maps = true;
	/* -port and -dedicated n apply before the configs and again after,
	 * so the command line wins over them */
	if (early.data)
		Cbuf_AddText(early.data);
	Mf_ExecConfigs();
	if (FS_FileExists("server.cfg"))
		Cbuf_AddText("exec server.cfg\n");
	Cbuf_AddText("__ded_cfgdone\n");
	if (early.data)
		Cbuf_AddText(early.data);
	if (cmds.data)
		Cbuf_AddText(cmds.data);
	Cbuf_AddText("__ded_startmap\n");
	SB_Free(&cmds);
	SB_Free(&early);
	Cbuf_Execute();

	double last = Sys_Seconds();
	long frames = 0;
	while (!quit_requested) {
		double now, dt;
		console_input();
		Cbuf_Execute();
		if (quit_requested)
			break;
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
