/* Bulwark engine - client demos: recording what the server sent, and
 * playing it back through the same message parser. The commands follow
 * the Quake-family and Source consoles (record, stop, playdemo, timedemo,
 * demos/startdemos, demo_pause and friends); the file layout is Bulwark's
 * own, since the network protocol is.
 *
 * File: "BWDM", a little-endian uint32 version, then records of
 *   double time (seconds since recording began), uint8 type, uint32 length,
 *   then the bytes.
 * Types: 1 reliable server message, 2 unreliable server message, 3 the
 * view angles that frame (3 floats), 4 a usercmd the player built (so the
 * client game's prediction replays as it ran).
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DEMO_MAGIC "BWDM"
#define DEMO_VERSION 1
enum { DR_RELIABLE = 1, DR_UNRELIABLE = 2, DR_ANGLES = 3, DR_USERCMD = 4 };

static struct {
	FILE *rec;
	bool waiting;     /* record started mid-game: wait for the full update's serverdata */
	double rec_start; /* cl.realtime when the first record was written */
	char rec_name[256];

	FILE *play;
	char play_name[256];
	double play_time; /* demo clock */
	bool paused;
	bool timedemo;
	double td_start;
	int td_frames;
	/* the record read ahead but not yet due */
	bool have_next;
	double next_time;
	int next_type;
	uint32_t next_len;
	uint8_t *next_data;
	size_t next_cap;
	double seek_to; /* > 0 while fast-forwarding to a time */

	/* the startdemos loop */
	char *loop[32];
	int numloop, loopnext;
} demo;

static cvar_t *demo_timescale, *demo_autorecord;

static void easy_record(int argc, const char **argv, bool ask_update);

bool CL_DemoPlaying(void) { return demo.play != NULL; }
bool CL_DemoRecording(void) { return demo.rec != NULL && !demo.waiting; }

static void write_record(int type, const void *data, uint32_t len)
{
	double t;
	uint8_t ty = (uint8_t)type;
	if (!demo.rec || demo.waiting)
		return;
	t = cl.realtime - demo.rec_start;
	fwrite(&t, sizeof(t), 1, demo.rec);
	fwrite(&ty, 1, 1, demo.rec);
	fwrite(&len, sizeof(len), 1, demo.rec);
	if (len)
		fwrite(data, 1, len, demo.rec);
}

/* every server message the client parses goes through here first */
void CL_DemoServerMessage(const uint8_t *data, size_t len, bool reliable)
{
	/* demo_autorecord starts a recording with each game's serverdata */
	if (!demo.rec && demo_autorecord->ivalue && !cl.demoplayback && reliable && len && data[0] == svc_serverdata)
		easy_record(1, NULL, false);
	if (!demo.rec)
		return;
	if (demo.waiting) {
		/* the recording starts with the serverdata that opens a signon */
		if (!reliable || !len || data[0] != svc_serverdata)
			return;
		demo.waiting = false;
		demo.rec_start = cl.realtime;
		Con_Printf("recording to %s\n", demo.rec_name);
	}
	write_record(reliable ? DR_RELIABLE : DR_UNRELIABLE, data, (uint32_t)len);
}

void CL_DemoFrame(void)
{
	if (demo.rec && !demo.waiting && cl.state == ca_active)
		write_record(DR_ANGLES, cl.viewangles, sizeof(float) * 3);
}

void CL_DemoUsercmd(const usercmd_t *u)
{
	if (demo.rec && !demo.waiting)
		write_record(DR_USERCMD, u, sizeof(*u));
}

static void stop_recording(void)
{
	if (!demo.rec)
		return;
	fclose(demo.rec);
	demo.rec = NULL;
	Con_Printf("%s %s\n", demo.waiting ? "recording never started:" : "stopped recording", demo.rec_name);
	demo.waiting = false;
}

static void demo_path(const char *name, char *out, size_t size)
{
	snprintf(out, size, "%s%s", name, strstr(name, ".dem") ? "" : ".dem");
}

static char last_record[256]; /* for rerecord */

static bool start_recording(const char *name, bool ask_update)
{
	char rel[256], path[1024];
	uint32_t ver = DEMO_VERSION;
	if (cl.demoplayback) {
		Con_Printf("can't record while playing a demo\n");
		return false;
	}
	stop_recording();
	demo_path(name, rel, sizeof(rel));
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(demo.rec = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", rel);
		return false;
	}
	fwrite(DEMO_MAGIC, 1, 4, demo.rec);
	fwrite(&ver, sizeof(ver), 1, demo.rec);
	Str_Copy(demo.rec_name, rel, sizeof(demo.rec_name));
	Str_Copy(last_record, name, sizeof(last_record));
	demo.waiting = true;
	if (!ask_update)
		return true;
	if (cl.state >= ca_loading) {
		/* the server sends everything again, from serverdata on */
		CL_SendStringCmd("fullupdate");
		Con_Printf("asking the server for a full update to start %s\n", rel);
	} else {
		Con_Printf("%s will start recording when a game starts\n", rel);
	}
	return true;
}

/* rerecord: record again under the name last used, from the start of the map */
static void Rerecord_f(int argc, const char **argv)
{
	if (!last_record[0]) {
		Con_Printf("nothing recorded yet\n");
		return;
	}
	if (start_recording(last_record, false) && cl.mapname[0])
		Cbuf_InsertText(va("map_restart\n"));
}

/* rmdemo <name>: delete a demo */
static void Rmdemo_f(int argc, const char **argv)
{
	char rel[256], path[1024];
	if (argc < 2 || !FS_SafeName(argv[1])) {
		Con_Printf("usage: rmdemo <demoname>\n");
		return;
	}
	demo_path(argv[1], rel, sizeof(rel));
	if (!FS_WritePath(rel, path, sizeof(path)) || remove(path))
		Con_Printf("no demo %s\n", rel);
	else
		Con_Printf("deleted %s\n", rel);
}

static void DemoSetspeed_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("demo speed is %g\n", demo_timescale->value);
		return;
	}
	Cvar_Set("demo_timescale", argv[1]);
}

/* record <name> [map]: records from now (or from the given map) */
static void Record_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: record <demoname> [map]\n");
		return;
	}
	if (!start_recording(argv[1], argc < 3))
		return;
	if (argc > 2)
		Cbuf_InsertText(va("map \"%s\"\n", argv[2]));
}

/* easyrecord [name]: a dated name */
static void easy_record(int argc, const char **argv, bool ask_update)
{
	char name[256];
	time_t t = time(NULL);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(name, sizeof(name), "demos/%s-%s%s%s", stamp, cl.mapname[0] ? cl.mapname : "demo", argc > 1 ? "-" : "",
	         argc > 1 ? argv[1] : "");
	start_recording(name, ask_update);
}

static void EasyRecord_f(int argc, const char **argv) { easy_record(argc, argv, true); }

/* --- playback --------------------------------------------------------------------------- */

static bool read_next(void)
{
	double t;
	uint8_t ty;
	uint32_t len;
	if (fread(&t, sizeof(t), 1, demo.play) != 1 || fread(&ty, 1, 1, demo.play) != 1 ||
	    fread(&len, sizeof(len), 1, demo.play) != 1 || len > (1u << 24))
		return false;
	if (len > demo.next_cap) {
		demo.next_data = Mem_Realloc(demo.next_data, len);
		demo.next_cap = len;
	}
	if (len && fread(demo.next_data, 1, len, demo.play) != len)
		return false;
	demo.next_time = t;
	demo.next_type = ty;
	demo.next_len = len;
	demo.have_next = true;
	return true;
}

static void end_playback(bool finished);

static bool open_demo(const char *name)
{
	char rel[256], magic[4];
	uint32_t ver;
	const char *tries[3];
	char a[256], b[300];
	demo_path(name, a, sizeof(a));
	snprintf(b, sizeof(b), "demos/%s", a);
	tries[0] = a, tries[1] = b, tries[2] = NULL;
	for (int i = 0; tries[i]; i++) {
		char path[1024];
		const char *where = FS_WhichPath(tries[i]);
		/* demos are loose files: open them where the search path found them */
		if (!where)
			continue;
		snprintf(path, sizeof(path), "%s/%s/%s", FS_BaseDir(), where, tries[i]);
		if ((demo.play = fopen(path, "rb"))) {
			Str_Copy(rel, tries[i], sizeof(rel));
			break;
		}
	}
	if (!demo.play) {
		Con_Printf("no demo %s\n", a);
		return false;
	}
	if (fread(magic, 1, 4, demo.play) != 4 || memcmp(magic, DEMO_MAGIC, 4) || fread(&ver, sizeof(ver), 1, demo.play) != 1 ||
	    ver != DEMO_VERSION) {
		Con_Printf("%s isn't a Bulwark demo (or is from another version)\n", rel);
		fclose(demo.play);
		demo.play = NULL;
		return false;
	}
	Str_Copy(demo.play_name, rel, sizeof(demo.play_name));
	return true;
}

static bool start_playback(const char *name, bool timedemo)
{
	stop_recording();
	CL_Disconnect(NULL);
	if (!open_demo(name))
		return false;
	memset(&cl.chan, 0, sizeof(cl.chan));
	cl.demoplayback = true;
	cl.state = ca_loading;
	Str_Copy(cl.servername, demo.play_name, sizeof(cl.servername));
	demo.play_time = 0;
	demo.paused = false;
	demo.have_next = false;
	demo.seek_to = 0;
	demo.timedemo = timedemo;
	demo.td_frames = 0;
	demo.td_start = Sys_Seconds();
	Con_Printf("playing %s\n", demo.play_name);
	return true;
}

static void Playdemo_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: %s <demoname>\n", argv[0]);
		return;
	}
	demo.numloop = 0;
	start_playback(argv[1], !Str_ICmp(argv[0], "timedemo"));
}

static void end_playback(bool finished)
{
	if (!demo.play)
		return;
	fclose(demo.play);
	demo.play = NULL;
	if (demo.timedemo && finished) {
		double t = Sys_Seconds() - demo.td_start;
		Con_Printf("timedemo %s: %d frames in %.2f seconds, %.1f fps\n", demo.play_name, demo.td_frames, t,
		           t > 0 ? demo.td_frames / t : 0);
	}
	demo.timedemo = false;
	cl.demoplayback = false;
	CL_Disconnect(finished ? "demo finished" : NULL);
	/* startdemos: on to the next one */
	if (finished && demo.numloop) {
		demo.loopnext = (demo.loopnext + 1) % demo.numloop;
		Cbuf_AddText(va("demos_next\n"));
	}
}

/* called instead of reading the network while a demo plays */
void CL_DemoReadPackets(double dt)
{
	bool frame_done = false;
	if (!demo.play)
		return;
	if (!demo.paused && !demo.timedemo)
		demo.play_time += dt * BW_MAX(0.0f, demo_timescale->value);
	if (demo.timedemo)
		demo.td_frames++;
	for (;;) {
		if (!demo.have_next && !read_next()) {
			end_playback(true);
			return;
		}
		/* a timedemo plays one recorded frame per engine frame, as fast as it can */
		if (demo.timedemo) {
			if (frame_done && demo.next_type == DR_ANGLES)
				return;
			demo.play_time = demo.next_time;
		} else if (demo.next_time > demo.play_time && !(demo.seek_to > 0 && demo.next_time < demo.seek_to)) {
			return;
		}
		demo.have_next = false;
		switch (demo.next_type) {
		case DR_RELIABLE:
		case DR_UNRELIABLE: {
			msg_t m;
			MSG_Init(&m, demo.next_data, demo.next_len);
			m.cursize = demo.next_len;
			CL_ParseServerMessage(&m, demo.next_type == DR_RELIABLE);
			if (!demo.play)
				return; /* the message ended the game */
			break;
		}
		case DR_ANGLES:
			if (demo.next_len == sizeof(float) * 3)
				memcpy(cl.viewangles, demo.next_data, sizeof(float) * 3);
			frame_done = true;
			break;
		case DR_USERCMD:
			if (demo.next_len == sizeof(usercmd_t)) {
				usercmd_t u;
				memcpy(&u, demo.next_data, sizeof(u));
				cl.cmds[u.sequence % CL_CMD_BACKUP] = u;
				if (u.sequence + 1 > cl.clientcommandframe)
					cl.clientcommandframe = u.sequence + 1;
			}
			break;
		default:
			break;
		}
		if (demo.seek_to > 0 && demo.next_time >= demo.seek_to) {
			demo.play_time = demo.seek_to;
			demo.seek_to = 0;
		}
	}
}

/* stop: ends a recording, or a demo being played */
static void Stop_f(int argc, const char **argv)
{
	if (demo.rec)
		stop_recording();
	else if (demo.play)
		end_playback(false);
	else
		Con_Printf("not recording or playing a demo\n");
}

static void Stopdemo_f(int argc, const char **argv)
{
	demo.numloop = 0;
	end_playback(false);
}

static void DemoPause_f(int argc, const char **argv)
{
	if (!demo.play)
		return;
	if (!Str_ICmp(argv[0], "demo_pause"))
		demo.paused = true;
	else if (!Str_ICmp(argv[0], "demo_resume"))
		demo.paused = false;
	else
		demo.paused = !demo.paused;
	cl.paused = demo.paused;
}

/* demo_jump/demo_gototime <seconds> (+/- for relative): going back
 * restarts the demo and runs through to the time */
static void DemoJump_f(int argc, const char **argv)
{
	double target;
	if (argc < 2 || !demo.play) {
		Con_Printf("usage: %s <seconds> (+n or -n to move by n)\n", argv[0]);
		return;
	}
	target = atof(argv[1]);
	if (argv[1][0] == '+' || argv[1][0] == '-')
		target += demo.play_time;
	if (target < 0)
		target = 0;
	if (target < demo.play_time) {
		char name[256];
		Str_Copy(name, demo.play_name, sizeof(name));
		if (!start_playback(name, false))
			return;
	}
	demo.seek_to = target;
	demo.play_time = target;
}

static void DemoInfo_f(int argc, const char **argv)
{
	if (demo.play)
		Con_Printf("playing %s at %.2f s%s, timescale %g\n", demo.play_name, demo.play_time, demo.paused ? " (paused)" : "",
		           demo_timescale->value);
	if (demo.rec)
		Con_Printf("recording %s%s\n", demo.rec_name, demo.waiting ? " (waiting for the game)" : "");
	if (!demo.play && !demo.rec)
		Con_Printf("no demo playing or recording\n");
}

/* listdemo <name>: what is in a demo */
static void Listdemo_f(int argc, const char **argv)
{
	int counts[5] = {0};
	double last = 0;
	FILE *keep = demo.play;
	bool kept_next = demo.have_next;
	if (argc < 2) {
		Con_Printf("usage: listdemo <demoname>\n");
		return;
	}
	if (keep) {
		Con_Printf("stop the demo first\n");
		return;
	}
	if (!open_demo(argv[1]))
		return;
	while (read_next()) {
		if (demo.next_type >= 1 && demo.next_type <= 4)
			counts[demo.next_type]++;
		last = demo.next_time;
	}
	fclose(demo.play);
	demo.play = NULL;
	demo.have_next = kept_next;
	Con_Printf("%s: %.1f seconds, %d frames, %d reliable and %d unreliable messages, %d commands\n", demo.play_name,
	           last, counts[DR_ANGLES], counts[DR_RELIABLE], counts[DR_UNRELIABLE], counts[DR_USERCMD]);
}

static void list_demo(const char *name, const char *searchpath, void *ctx)
{
	int *n = ctx;
	Con_Printf("  %-40s %s\n", name, searchpath);
	(*n)++;
}

static void Demolist_f(int argc, const char **argv)
{
	int n = 0;
	FS_List("*.dem", true, list_demo, &n);
	FS_List("demos/*.dem", true, list_demo, &n);
	Con_Printf("%d demos\n", n);
}

/* startdemos <demo1> <demo2> ...: plays them in a loop (the attract mode) */
static void Startdemos_f(int argc, const char **argv)
{
	for (int i = 0; i < demo.numloop; i++)
		Mem_Free(demo.loop[i]);
	demo.numloop = 0;
	for (int i = 1; i < argc && demo.numloop < 32; i++)
		demo.loop[demo.numloop++] = Mem_Strdup(argv[i]);
	demo.loopnext = 0;
	if (demo.numloop && cl.state == ca_disconnected)
		Cbuf_AddText("demos_next\n");
}

/* demos: back into the loop */
static void Demos_f(int argc, const char **argv)
{
	if (!demo.numloop) {
		Con_Printf("no demo loop (startdemos)\n");
		return;
	}
	Cbuf_AddText("demos_next\n");
}

static void DemosNext_f(int argc, const char **argv)
{
	int n = demo.numloop;
	if (!n)
		return;
	start_playback(demo.loop[demo.loopnext % n], false);
}

/* a recording keeps going across maps, and stops with the game */
void CL_DemoDisconnected(void)
{
	if (demo.rec && !demo.waiting && !cl.demoplayback)
		stop_recording();
}

/* the game ended some other way (disconnect, a new map) during playback */
void CL_DemoAbort(void)
{
	if (demo.play) {
		fclose(demo.play);
		demo.play = NULL;
	}
	demo.timedemo = false;
	cl.demoplayback = false;
}

void CL_InitDemos(void)
{
	demo_timescale = Cvar_Get("demo_timescale", "1", 0, "Demo playback speed");
	demo_autorecord = Cvar_Get("demo_autorecord", "0", CVAR_ARCHIVE, "Record every game to demos/");
	Cmd_AddD("record", Record_f, "record <name> [map]: record a demo");
	Cmd_AddD("easyrecord", EasyRecord_f, "easyrecord [suffix]: record a demo under a dated name");
	Cmd_AddD("stop", Stop_f, "Stop recording (or playing) a demo");
	Cmd_AddD("playdemo", Playdemo_f, "playdemo <name>: play a demo");
	Cmd_AddD("timedemo", Playdemo_f, "timedemo <name>: play a demo as fast as possible and report the frame rate");
	Cmd_AddD("stopdemo", Stopdemo_f, "Stop the demo playing");
	Cmd_AddD("demo_pause", DemoPause_f, "Pause demo playback");
	Cmd_AddD("demo_resume", DemoPause_f, "Resume demo playback");
	Cmd_AddD("demo_togglepause", DemoPause_f, "Pause or resume demo playback");
	Cmd_AddD("demo_jump", DemoJump_f, "demo_jump <seconds>: go to a time in the demo (+n/-n relative)");
	Cmd_AddD("demo_gototime", DemoJump_f, "demo_gototime <seconds>: go to a time in the demo");
	Cmd_AddD("demo_info", DemoInfo_f, "What demo is playing or recording");
	Cmd_AddD("listdemo", Listdemo_f, "listdemo <name>: what a demo holds");
	Cmd_AddD("demolist", Demolist_f, "List the demos");
	Cmd_AddD("startdemos", Startdemos_f, "startdemos <demo1> [demo2...]: play demos in a loop");
	Cmd_AddD("demos", Demos_f, "Back to the startdemos loop");
	Cmd_AddD("demos_next", DemosNext_f, NULL);
	Cmd_AddD("rerecord", Rerecord_f, "Record again under the last name, from the start of the map");
	Cmd_AddD("rmdemo", Rmdemo_f, "rmdemo <name>: delete a demo");
	Cmd_AddD("demo_setspeed", DemoSetspeed_f, "demo_setspeed <scale>: playback speed (demo_timescale)");
}
