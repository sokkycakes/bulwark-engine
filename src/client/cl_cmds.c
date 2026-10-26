/* Bulwark engine - client console commands: key binding helpers, config
 * hooks, view and screen commands, and the cvars for the on-screen
 * readouts (fps, position, net graph) that r_overlay.c draws. Names and
 * behaviour follow the public documentation of the Quake-family and
 * Source consoles. ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../server/sv_local.h"
#include "../render/render.h"
#include "../net/net.h"
#include "../sound/sound.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

clscreen_t cl_screen;

/* --- config hooks ------------------------------------------------------------ */

static void write_binds(strbuf_t *sb)
{
	SB_AppendStr(sb, "unbindall\n");
	Key_WriteBindings(sb);
}

static void reset_binds(void)
{
	for (int k = 0; k < K_MAX; k++)
		Key_SetBinding(k, NULL);
}

/* --- binding helpers --------------------------------------------------------- */

/* BindToggle <key> <cvar> [values...]: the key toggles the cvar */
static void BindToggle_f(int argc, const char **argv)
{
	char cmd[1024];
	int k;
	if (argc < 3) {
		Con_Printf("usage: bindtoggle <key> <cvar> [values...]\n");
		return;
	}
	k = Key_NumForName(argv[1]);
	if (k < 0) {
		Con_Printf("\"%s\" isn't a valid key\n", argv[1]);
		return;
	}
	snprintf(cmd, sizeof(cmd), "toggle %s", argv[2]);
	for (int i = 3; i < argc; i++) {
		Str_Cat(cmd, " ", sizeof(cmd));
		Str_Cat(cmd, argv[i], sizeof(cmd));
	}
	Key_SetBinding(k, cmd);
}

/* key_findbinding <text>: the keys whose binding holds the text */
static void FindBinding_f(int argc, const char **argv)
{
	int n = 0;
	if (argc < 2) {
		Con_Printf("usage: key_findbinding <command>\n");
		return;
	}
	for (int k = 0; k < K_MAX; k++) {
		const char *b = Key_GetBinding(k);
		if (*b && strstr(b, argv[1])) {
			Con_Printf("  %-12s \"%s\"\n", Key_NameForNum(k), b);
			n++;
		}
	}
	if (!n)
		Con_Printf("no key is bound to \"%s\"\n", argv[1]);
}

static void ListBoundKeys_f(int argc, const char **argv)
{
	for (int k = 0; k < K_MAX; k++)
		if (*Key_GetBinding(k))
			Con_Printf("\"%s\" = \"%s\"\n", Key_NameForNum(k), Key_GetBinding(k));
}

/* in_bind <device> <key> <command>: one device's own binding. Bulwark
 * shares bindings across devices, so the device is accepted and ignored */
static void InBind_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: in_bind <device> <key> [command]\n");
		return;
	}
	if (argc == 3)
		Cbuf_InsertText(va("bind \"%s\"\n", argv[2]));
	else
		Cbuf_InsertText(va("bind \"%s\" \"%s\"\n", argv[2], argv[3]));
}

/* --- view ---------------------------------------------------------------------- */

static void CenterView_f(int argc, const char **argv) { cl.viewangles[0] = 0; }

/* setang (FTE/Source): the client's view angles, "setang pitch yaw [roll]" */
static void SetAng_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: %s <pitch> <yaw> [roll]\n", argv[0]);
		return;
	}
	cl.viewangles[0] = BW_CLAMP((float)atof(argv[1]), -89, 89);
	cl.viewangles[1] = (float)atof(argv[2]);
	cl.viewangles[2] = argc > 3 ? (float)atof(argv[3]) : 0;
}

static void Retry_f(int argc, const char **argv)
{
	if (cl.servername[0])
		Cbuf_InsertText(va("connect \"%s\"\n", cl.servername));
	else
		Con_Printf("no server to retry\n");
}

static void SizeUp_f(int argc, const char **argv) { Cvar_SetValue("viewsize", BW_MIN(Cvar_Value("viewsize") + 10, 120)); }
static void SizeDown_f(int argc, const char **argv) { Cvar_SetValue("viewsize", BW_MAX(Cvar_Value("viewsize") - 10, 30)); }

/* --- screen tints ---------------------------------------------------------------- */

/* bf: the short gold flash Quake gives on item pickups */
static void BonusFlash_f(int argc, const char **argv)
{
	cl_screen.flash[0] = 215 / 255.0f;
	cl_screen.flash[1] = 186 / 255.0f;
	cl_screen.flash[2] = 69 / 255.0f;
	cl_screen.flash[3] = 50 / 255.0f;
	cl_screen.flash_fade = 100 / 255.0f; /* alpha lost per second */
}

/* v_cshift <r> <g> <b> <percent>: a steady tint, 0-255 colours */
static void CShift_f(int argc, const char **argv)
{
	if (argc < 5) {
		memset(cl_screen.cshift, 0, sizeof(cl_screen.cshift));
		return;
	}
	for (int i = 0; i < 3; i++)
		cl_screen.cshift[i] = (float)atof(argv[i + 1]) / 255.0f;
	cl_screen.cshift[3] = BW_CLAMP((float)atof(argv[4]) / 255.0f, 0, 1);
}

/* fadein/fadeout [seconds] [r g b]: a full-screen fade, as Source's */
static void Fade_f(int argc, const char **argv)
{
	float t = argc > 1 ? (float)atof(argv[1]) : 2;
	cl_screen.fade_color[0] = argc > 4 ? (float)atof(argv[2]) / 255.0f : 0;
	cl_screen.fade_color[1] = argc > 4 ? (float)atof(argv[3]) / 255.0f : 0;
	cl_screen.fade_color[2] = argc > 4 ? (float)atof(argv[4]) / 255.0f : 0;
	cl_screen.fade_start = cl.realtime;
	cl_screen.fade_time = t > 0 ? t : 0.001f;
	cl_screen.fade_out = !Str_ICmp(argv[0], "fadeout");
	cl_screen.fading = true;
}

/* cprint/conecho_center <text>: centre print, through the game if it draws them */
static void CPrint_f(int argc, const char **argv)
{
	char text[1024];
	const char *s = Cmd_Args();
	/* "\n" written in the text breaks the line */
	size_t o = 0;
	for (; *s && o + 1 < sizeof(text); s++) {
		if (s[0] == '\\' && s[1] == 'n') {
			text[o++] = '\n';
			s++;
		} else if (*s != '"') {
			text[o++] = *s;
		}
	}
	text[o] = 0;
	if (!CSQC_CenterPrint(text)) {
		Str_Copy(cl_screen.centerprint, text, sizeof(cl_screen.centerprint));
		cl_screen.centerprint_time = cl.realtime;
	}
}

static void ConEcho_f(int argc, const char **argv) { Con_Printf("%s\n", Cmd_Args()); }

/* say and say_team go to the server we're playing on; with no game
 * running, a listen server's console "say" speaks as the console */
static void Say_f(int argc, const char **argv)
{
	if (cl.state >= ca_loading) {
		CL_SendStringCmd(va("%s %s", argv[0], Cmd_Args()));
		return;
	}
	if (!Str_ICmp(argv[0], "say") && SV_ConsoleSay(Cmd_Args()))
		return;
	Con_Printf("not connected\n");
}

/* pause: the local server's, or asked of the server we're on */
static void Pause_f(int argc, const char **argv)
{
	if (sv.active) {
		if (!SV_RequestPause(0, !Str_ICmp(argv[0], "unpause") ? false : !Str_ICmp(argv[0], "setpause") ? true : !sv.paused))
			Con_Printf("the game didn't allow that\n");
	} else if (cl.state >= ca_loading) {
		CL_SendStringCmd("pause");
	} else {
		Con_Printf("not playing\n");
	}
}

static cvar_t *rcon_address, *rcon_password_cl;

/* rcon <command>: run a command on a server we have the password for */
static void Rcon_f(int argc, const char **argv)
{
	netadr_t to;
	if (argc < 2) {
		Con_Printf("usage: rcon <command> (set rcon_password, and rcon_address when not connected)\n");
		return;
	}
	if (!Cvar_String("rcon_password")[0]) {
		Con_Printf("set rcon_password first\n");
		return;
	}
	if (rcon_address->string[0]) {
		if (!NET_StringToAdr(rcon_address->string, &to, PORT_SERVER)) {
			Con_Printf("bad rcon_address\n");
			return;
		}
	} else if (cl.state >= ca_connecting) {
		to = cl.server;
	} else {
		Con_Printf("not connected; set rcon_address\n");
		return;
	}
	NET_OpenClient();
	NET_OutOfBandPrint(NS_CLIENT, &to, "rcon \"%s\" %s", Cvar_String("rcon_password"), Cmd_Args());
}

/* serverinfo [key value]: lists the server's keys, or sets one on ours */
static void Serverinfo_f(int argc, const char **argv)
{
	if (argc >= 3) {
		if (!SV_SetServerinfoKey(argv[1], argv[2]))
			Con_Printf("no local server to set it on\n");
		return;
	}
	CL_PrintServerinfo();
}

/* game commands the server handles (cheats, kill, joining a team); listed
 * here so they show in cmdlist and complete in the console */
static void ForwardToServer_f(int argc, const char **argv)
{
	if (cl.state < ca_loading) {
		Con_Printf("%s needs a game running\n", argv[0]);
		return;
	}
	CL_SendStringCmd(argc > 1 ? va("%s %s", argv[0], Cmd_Args()) : argv[0]);
}

/* cl_status: the connection and what the client loaded */
static void ClStatus_f(int argc, const char **argv)
{
	static const char *const states[] = {"disconnected", "challenging", "connecting", "loading", "active"};
	Con_Printf("state            : %s\n", cl.state >= 0 && cl.state <= ca_active ? states[cl.state] : "?");
	if (cl.state >= ca_loading) {
		Con_Printf("server           : %s%s\n", cl.servername, cl.demoplayback ? " (demo)" : "");
		Con_Printf("packets,bytes/sec: in %.1f B/s  out %.1f B/s, rtt %d ms, loss %d%%\n", cl.chan.bytes_in_sec,
		           cl.chan.bytes_out_sec, (int)(cl.chan.rtt * 1000), (int)(cl.chan.loss * 100));
		Con_Printf("map              : %s\n", cl.mapname);
	}
	Con_Printf("csqc             : %s\n", cl.csqc ? cl.csprogsname : "not loaded");
	Con_Printf("gamedir          : %s\n", FS_GameDir());
}

/* packet <destination> <contents>: one connectionless packet; replies are
 * printed as they come */
static void Packet_f(int argc, const char **argv)
{
	netadr_t to;
	char buf[2048];
	size_t n = 0;
	if (argc < 3) {
		Con_Printf("usage: packet <destination> <contents>\n");
		return;
	}
	if (!NET_StringToAdr(argv[1], &to, 27500)) {
		Con_Printf("bad address %s\n", argv[1]);
		return;
	}
	NET_OpenClient();
	memcpy(buf, "\xff\xff\xff\xff", 4);
	n = 4;
	/* \n and \\ as in FTE, so multi-line packets can be typed */
	for (const char *p = argv[2]; *p && n + 1 < sizeof(buf); p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			buf[n++] = '\n';
			p++;
		} else if (p[0] == '\\' && p[1] == '\\') {
			buf[n++] = '\\';
			p++;
		} else {
			buf[n++] = *p;
		}
	}
	NET_SendPacket(NS_CLIENT, buf, n, &to);
}

/* changing: the server says a level change is coming (QuakeWorld) */
static void Changing_f(int argc, const char **argv)
{
	if (cl.state < ca_loading) {
		Con_Printf("Changing while not connected\n");
		return;
	}
	cl.state = ca_loading;
	S_StopAllSounds();
}

/* timerefresh: draw 128 frames turning once around, and time them */
static void Timerefresh_f(int argc, const char **argv)
{
	float yaw = cl.viewangles[1];
	double start = Sys_Seconds(), t;
	for (int i = 0; i < 128; i++) {
		int w, h;
		cl.viewangles[1] = yaw + i * (360.0f / 128);
		R_BeginFrame();
		R_GetSize(&w, &h);
		CSQC_Frame(w, h);
		R_EndFrame();
	}
	cl.viewangles[1] = yaw;
	t = Sys_Seconds() - start;
	Con_Printf("%f seconds (%f fps)\n", t, t > 0 ? 128 / t : 0);
}

/* me <action>: an emote in chat */
static void Me_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: me <action>\n");
		return;
	}
	Cmd_ExecuteString(va("say \"* %s %s\"", Cvar_String("name"), Cmd_Args()));
}

/* cd <play|loop|stop|pause|resume|info> [track]: the CD-audio command,
 * played through the music player (tracks are music/track<n>) */
static void Cd_f(int argc, const char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	if (!Str_ICmp(c, "play") || !Str_ICmp(c, "loop")) {
		if (argc < 3)
			Con_Printf("cd %s <track>\n", c);
		else
			Cmd_ExecuteString(va("music %s%s", argv[2], !Str_ICmp(c, "play") ? " -" : ""));
	} else if (!Str_ICmp(c, "stop") || !Str_ICmp(c, "off") || !Str_ICmp(c, "pause")) {
		Cmd_ExecuteString("stopmusic");
	} else if (!Str_ICmp(c, "info") || !*c) {
		Con_Printf("No cd drive detected; cd play <track> plays music/track<n> instead.\n");
	} else {
		Con_Printf("cd: play, loop, stop, pause or info\n");
	}
}

/* mod <command>: a command for the game modules, whichever takes it */
static void Mod_f(int argc, const char **argv)
{
	const char *line = Cmd_Args();
	if (argc < 2 || !(MQC_ForceCommand(line) || CSQC_ForceCommand(line) || SV_ConsoleCommand(line)))
		Con_Printf("Mod-specific command \"%s\" not known\n", argc > 1 ? argv[1] : "");
}

/* sendcvar <name>: a server asks for a cvar's value; the answer goes back
 * as "sentcvar <name> <value>" for the game to read */
static void Sendcvar_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 2 || cl.state < ca_loading)
		return;
	v = Cvar_Find(argv[1]);
	CL_SendStringCmd(va("sentcvar %s \"%s\"", argv[1], v ? v->string : ""));
}

/* cl_cmd and menu_cmd: a command for the client game or the menu, even one
 * it didn't register */
static void ClCmd_f(int argc, const char **argv)
{
	bool menu = !Str_ICmp(argv[0], "menu_cmd");
	if (argc < 2) {
		Con_Printf("usage: %s <command> [args]\n", argv[0]);
		return;
	}
	if (!(menu ? MQC_ForceCommand(Cmd_Args()) : CSQC_ForceCommand(Cmd_Args())))
		Con_Printf("the %s isn't running or doesn't know \"%s\"\n", menu ? "menu" : "client game", argv[1]);
}

void CL_InitCommands(void)
{
	{
		static const char *const fwd[][2] = {
			{"god", "Toggle invulnerability (cheat)"}, {"buddha", "Take damage but don't die (cheat)"},
			{"noclip", "Toggle flying through walls (cheat)"}, {"notarget", "Toggle being ignored by monsters (cheat)"},
			{"fly", "Toggle flying (cheat)"}, {"give", "give <item or health> [amount]: (cheat)"},
			{"setpos", "setpos <x> <y> <z> [pitch yaw roll]: move yourself (cheat)"}, {"kill", "Kill yourself"},
			{"join", "Join the game from spectating"}, {"observe", "Spectate"},
			{"sayone", "sayone <player> <text>: a private message"},
			{"bestweapon", "bestweapon <impulse> [impulse...]: switch to the first of these weapons you have"},
			{"6dof", "Toggle six-degrees-of-freedom movement: no pitch limit, roll with +rollleft/+rollright (cheat)"},
			{"spiderpig", "Toggle walking up walls (cheat)"},
		};
		for (size_t i = 0; i < sizeof(fwd) / sizeof(fwd[0]); i++)
			if (!Cmd_Exists(fwd[i][0]))
				Cmd_AddD(fwd[i][0], ForwardToServer_f, fwd[i][1]);
	}
	Cmd_AddD("cl_status", ClStatus_f, "The connection, map, client game and game directory");
	Cmd_AddD("packet", Packet_f, "packet <destination> <contents>: send a connectionless packet (\\n for a line break)");
	Cmd_AddD("changing", Changing_f, "Sent by servers before a level change; not for typing");
	Cmd_AddD("timerefresh", Timerefresh_f, "Draw 128 frames turning around once, and report the frame rate");
	Cmd_AddD("me", Me_f, "me <action>: an emote in chat");
	Cmd_AddD("cd", Cd_f, "cd <play|loop|stop|info> [track]: CD audio, played as music/track<n>");
	Cmd_AddD("sendcvar", Sendcvar_f, "sendcvar <cvar>: tell the server a cvar's value (servers send this)");
	Cmd_AddD("mod", Mod_f, "mod <command>: a command for the menu, client game or server game");
	Cmd_AddD("cl_cmd", ClCmd_f, "cl_cmd <command>: run a command of the client game (CSQC_ConsoleCommand)");
	Cmd_AddD("menu_cmd", ClCmd_f, "menu_cmd <command>: run a command of the menu (m_consolecommand)");
	CL_InitDemos();
	cfg_write_hook = write_binds;
	cfg_reset_hook = reset_binds;

	cl_screen.showfps = Cvar_Get("cl_showfps", "0", CVAR_ARCHIVE, "Show the frame rate: 1 for fps, 2 adds frame times");
	cl_screen.showpos = Cvar_Get("cl_showpos", "0", 0, "Show the view position, angles and speed");
	cl_screen.netgraph = Cvar_Get("net_graph", "0", CVAR_ARCHIVE, "Network graph and readout: 1 numbers, 2 adds the graph");
	cl_screen.drawnotify = Cvar_Get("con_drawnotify", "0", CVAR_ARCHIVE, "Show the newest console lines over the game");
	cl_screen.notifytime = Cvar_Get("con_notifytime", "4", CVAR_ARCHIVE, "Seconds a notify line stays");
	cl_screen.notifylines = Cvar_Get("con_notifylines", "4", CVAR_ARCHIVE, "How many notify lines to show");
	cl_screen.drawhud = Cvar_Get("cl_drawhud", "1", 0, "Draw the game's 2D overlay");
	cl_screen.crosshair = Cvar_Get("crosshair", "1", CVAR_ARCHIVE, "Crosshair, for games that leave it to the engine");
	Cvar_Get("viewsize", "100", CVAR_ARCHIVE, "Screen size for games that read it (sizeup/sizedown)");
	Cvar_Get("show_fps", "0", 0, "FTE name for cl_showfps");

	Cmd_AddD("bindtoggle", BindToggle_f, "bindtoggle <key> <cvar> [values...]: the key toggles the cvar");
	Cmd_AddD("key_findbinding", FindBinding_f, "key_findbinding <command>: keys bound to a command");
	Cmd_AddD("key_listboundkeys", ListBoundKeys_f, "List every bound key");
	Cmd_AddD("in_bind", InBind_f, "in_bind <device> <key> [command]: bind for one input device");
	Cmd_AddD("centerview", CenterView_f, "Level the view pitch");
	Cmd_AddD("force_centerview", CenterView_f, "Level the view pitch");
	Cmd_AddD("setang_local", SetAng_f, "setang_local <pitch> <yaw> [roll]: set the view angles");
	Cmd_AddD("retry", Retry_f, "Connect again to the last server");
	Cmd_AddD("sizeup", SizeUp_f, "Bigger view (viewsize)");
	Cmd_AddD("sizedown", SizeDown_f, "Smaller view (viewsize)");
	Cmd_AddD("bf", BonusFlash_f, "The item-pickup screen flash");
	Cmd_AddD("v_cshift", CShift_f, "v_cshift <r> <g> <b> <amount>: tint the screen (0-255)");
	Cmd_AddD("fadein", Fade_f, "fadein [seconds] [r g b]: fade the screen in from a colour");
	Cmd_AddD("fadeout", Fade_f, "fadeout [seconds] [r g b]: fade the screen out to a colour");
	Cmd_AddD("cprint", CPrint_f, "cprint <text>: print in the middle of the screen");
	Cmd_AddD("conecho_center", CPrint_f, "conecho_center <text>: print in the middle of the screen");
	Cmd_AddD("centerprint", CPrint_f, "centerprint <text>: print in the middle of the screen");
	Cmd_AddD("conecho", ConEcho_f, "conecho <text>: print to the console");
	rcon_address = Cvar_Get("rcon_address", "", 0, "Server for rcon when not connected to it");
	rcon_password_cl = Cvar_Get("rcon_password", "", 0, "Remote console password");
	(void)rcon_password_cl;
	Cmd_AddD("pause", Pause_f, "Pause or unpause the game");
	Cmd_AddD("setpause", Pause_f, "Pause the game");
	Cmd_AddD("unpause", Pause_f, "Unpause the game");
	Cmd_AddD("rcon", Rcon_f, "rcon <command>: run a command on the server (needs rcon_password)");
	Cmd_AddD("serverinfo", Serverinfo_f, "serverinfo [key value]: the server's settings, or set one on a local server");
	Cmd_AddD("say", Say_f, "say <text>: chat to everyone");
	Cmd_AddD("say_team", Say_f, "say_team <text>: chat to your team");
}
