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
 * client game's prediction replays as it ran), 5 a uint8 player slot the
 * records after it belong to (server demos hold every player's view; see
 * sv_mvd.c). Records for players other than the one tracked are skipped,
 * and switching player plays the demo again from the start up to the
 * same time with the new player's records (a live QTV stream reconnects).
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#define DEMO_MAGIC "BWDM"
#define DEMO_VERSION 1
enum { DR_RELIABLE = 1, DR_UNRELIABLE = 2, DR_ANGLES = 3, DR_USERCMD = 4, DR_VIEW = 5 };
#define QTV_DEFAULT_PORT 27599

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

	/* server demos: whose view is shown */
	int curview;  /* the player the records now being read belong to, -1 for all */
	int track;    /* the player shown, -1 until the first one */
	int want_track; /* to start with after a restart, -1 for the first */
	uint8_t seen[32]; /* players with a view in the demo, as bits */
	bool multiview;
	double next_autotrack;

	/* a QTV stream instead of a file */
	int qtv_sock;
	bool qtv, qtv_closed, qtv_header, qtv_magic, qtv_sent;
	strbuf_t qbuf;
	size_t qpos;
	char qtv_source[256], qtv_password[128];

	/* the startdemos loop */
	char *loop[32];
	int numloop, loopnext;
} demo;

static cvar_t *demo_timescale, *demo_autorecord, *cl_autotrack, *cl_autotrack_team;
static bool autotrack_on;

static void easy_record(int argc, const char **argv, bool ask_update);

bool CL_DemoPlaying(void) { return demo.play != NULL || demo.qtv; }
bool CL_DemoMultiview(void) { return (demo.play || demo.qtv) && demo.multiview; }
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
	snprintf(out, size, "%s%s", name, strstr(name, ".dem") || strstr(name, ".mvd") ? "" : ".dem");
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

static bool read_next_qtv(void)
{
	double t;
	uint8_t ty;
	uint32_t len;
	const char *p = demo.qbuf.data + demo.qpos;
	size_t avail = demo.qbuf.len - demo.qpos;
	if (avail < 13)
		return false;
	memcpy(&t, p, 8);
	ty = (uint8_t)p[8];
	memcpy(&len, p + 9, 4);
	if (len > (1u << 24)) {
		demo.qtv_closed = true;
		return false;
	}
	if (avail < 13 + (size_t)len)
		return false;
	if (len > demo.next_cap) {
		demo.next_data = Mem_Realloc(demo.next_data, len);
		demo.next_cap = len;
	}
	memcpy(demo.next_data, p + 13, len);
	demo.qpos += 13 + len;
	if (demo.qpos > (1u << 20)) {
		memmove(demo.qbuf.data, demo.qbuf.data + demo.qpos, demo.qbuf.len - demo.qpos);
		demo.qbuf.len -= demo.qpos;
		demo.qpos = 0;
	}
	demo.next_time = t;
	demo.next_type = ty;
	demo.next_len = len;
	demo.have_next = true;
	return true;
}

static bool read_next(void)
{
	double t;
	uint8_t ty;
	uint32_t len;
	if (demo.qtv)
		return read_next_qtv();
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
	const char *tries[5];
	char a[256], b[300], c[256], d[300];
	int n = 0;
	demo_path(name, a, sizeof(a));
	snprintf(b, sizeof(b), "demos/%s", a);
	tries[n++] = a, tries[n++] = b;
	/* server demos are .mvd */
	if (!strchr(name, '.')) {
		snprintf(c, sizeof(c), "%s.mvd", name);
		snprintf(d, sizeof(d), "demos/%s.mvd", name);
		tries[n++] = c, tries[n++] = d;
	}
	tries[n] = NULL;
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
	/* which players have views, from the record headers */
	memset(demo.seen, 0, sizeof(demo.seen));
	demo.multiview = false;
	{
		long pos = ftell(demo.play);
		double tt;
		uint8_t ty, v;
		uint32_t len;
		while (fread(&tt, sizeof(tt), 1, demo.play) == 1 && fread(&ty, 1, 1, demo.play) == 1 &&
		       fread(&len, sizeof(len), 1, demo.play) == 1) {
			if (ty == DR_VIEW && len == 1 && fread(&v, 1, 1, demo.play) == 1) {
				demo.seen[v >> 3] |= (uint8_t)(1 << (v & 7));
				demo.multiview = true;
			} else if (fseek(demo.play, (long)len, SEEK_CUR))
				break;
		}
		fseek(demo.play, pos, SEEK_SET);
	}
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
	demo.curview = -1;
	demo.track = demo.want_track;
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
	demo.want_track = -1;
	start_playback(argv[1], !Str_ICmp(argv[0], "timedemo"));
}

static void qtv_close(void)
{
	NET_TCPClose(demo.qtv_sock);
	demo.qtv_sock = -1;
	SB_Free(&demo.qbuf);
	demo.qpos = 0;
	demo.qtv = false;
}

static void end_playback(bool finished)
{
	if (!demo.play && !demo.qtv)
		return;
	if (demo.play)
		fclose(demo.play);
	demo.play = NULL;
	if (demo.qtv) {
		qtv_close();
		Con_Printf("QTV stream ended\n");
	}
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

/* --- QTV streams ------------------------------------------------------------ */

static char last_qtv_host[256];

/* "[stream@]host[:port]": the address, and the stream (empty for the game) */
static bool qtv_address(const char *arg, netadr_t *adr, char *stream, size_t ssize)
{
	const char *at = strrchr(arg, '@');
	const char *host = at ? at + 1 : arg;
	stream[0] = 0;
	if (at)
		Str_Copy(stream, arg, BW_MIN(ssize, (size_t)(at - arg) + 1));
	if (!strncmp(host, "tls://", 6))
		host += 6; /* no TLS here: plain TCP */
	if (!NET_StringToAdr(host, adr, QTV_DEFAULT_PORT) || adr->type == NA_LOOPBACK) {
		if (adr->type == NA_LOOPBACK && NET_StringToAdr("127.0.0.1", adr, QTV_DEFAULT_PORT)) {
			const char *colon = strrchr(host, ':');
			if (colon)
				adr->port = (uint16_t)atoi(colon + 1);
			Str_Copy(last_qtv_host, host, sizeof(last_qtv_host));
			return true;
		}
		return false;
	}
	Str_Copy(last_qtv_host, host, sizeof(last_qtv_host));
	return true;
}

static bool qtv_start(const char *source, const char *password)
{
	netadr_t adr;
	char stream[256], req[700];
	if (!qtv_address(source, &adr, stream, sizeof(stream))) {
		Con_Printf("bad QTV address %s\n", source);
		return false;
	}
	stop_recording();
	CL_Disconnect(NULL);
	if ((demo.qtv_sock = NET_TCPConnect(&adr)) < 0) {
		Con_Printf("Couldn't connect to proxy\n");
		return false;
	}
	if (!strncmp(stream, "demo:", 5))
		snprintf(req, sizeof(req), "BWQTV 1\nPASSWORD: %s\nDEMO: %s\n\n", password, stream + 5);
	else
		snprintf(req, sizeof(req), "BWQTV 1\nPASSWORD: %s\nSOURCE: %s\n\n", password, stream);
	/* sent once the connection is up, from qtv_pump */
	SB_Free(&demo.qbuf);
	SB_AppendStr(&demo.qbuf, req);
	demo.qpos = 0;
	demo.qtv = true;
	demo.qtv_closed = false;
	demo.qtv_header = false;
	demo.qtv_magic = false;
	demo.qtv_sent = false;
	if (source != demo.qtv_source)
		Str_Copy(demo.qtv_source, source, sizeof(demo.qtv_source));
	if (password != demo.qtv_password)
		Str_Copy(demo.qtv_password, password, sizeof(demo.qtv_password));
	memset(&cl.chan, 0, sizeof(cl.chan));
	cl.demoplayback = true;
	cl.state = ca_loading;
	snprintf(cl.servername, sizeof(cl.servername), "qtv:%s", source);
	Str_Copy(demo.play_name, source, sizeof(demo.play_name));
	demo.play_time = 0;
	demo.paused = false;
	demo.have_next = false;
	demo.seek_to = 0;
	demo.timedemo = false;
	demo.curview = -1;
	demo.track = demo.want_track;
	demo.multiview = false;
	memset(demo.seen, 0, sizeof(demo.seen));
	Con_Printf("connecting to QTV %s\n", source);
	return true;
}

/* moves the stream along; true once records can be read */
static bool qtv_pump(void)
{
	char buf[65536];
	int n;
	if (!demo.qtv_header && !demo.qtv_sent) {
		/* the request waits in qbuf until the connection takes it */
		n = NET_TCPSend(demo.qtv_sock, demo.qbuf.data, demo.qbuf.len);
		if (n < 0) {
			Con_Printf("Couldn't connect to proxy\n");
			end_playback(false);
			return false;
		}
		if (n > 0) {
			memmove(demo.qbuf.data, demo.qbuf.data + n, demo.qbuf.len - (size_t)n);
			demo.qbuf.len -= (size_t)n;
			if (!demo.qbuf.len)
				demo.qtv_sent = true;
		}
		if (!demo.qtv_sent)
			return false;
	}
	while ((n = NET_TCPRecv(demo.qtv_sock, buf, sizeof(buf))) > 0)
		SB_Append(&demo.qbuf, buf, (size_t)n);
	if (n < 0)
		demo.qtv_closed = true;
	if (!demo.qtv_header) {
		char *end;
		SB_Append(&demo.qbuf, "", 1);
		demo.qbuf.len--;
		end = strstr(demo.qbuf.data, "\n\n");
		if (!end) {
			if (demo.qtv_closed) {
				Con_Printf("Couldn't connect to proxy\n");
				end_playback(false);
			}
			return false;
		}
		*end = 0;
		if (strncmp(demo.qbuf.data, "BWQTV 1", 7)) {
			Con_Printf("that isn't a Bulwark QTV server\n");
			end_playback(false);
			return false;
		}
		for (char *l = demo.qbuf.data; l && *l;) {
			char *e = strchr(l, '\n');
			if (e)
				*e = 0;
			if (!strncmp(l, "ERROR:", 6)) {
				Con_Printf("QTV:%s\n", l + 6);
				end_playback(false);
				return false;
			}
			if (!strncmp(l, "LIVE:", 5) && atof(l + 5) > 0) {
				/* catch up from the stream's start to now */
				demo.seek_to = atof(l + 5);
				demo.play_time = demo.seek_to;
			}
			l = e ? e + 1 : NULL;
		}
		demo.qpos = (size_t)(end + 2 - demo.qbuf.data);
		demo.qtv_header = true;
		demo.qtv_sent = false;
	}
	if (!demo.qtv_magic) {
		uint32_t ver;
		if (demo.qbuf.len - demo.qpos < 8)
			return false;
		memcpy(&ver, demo.qbuf.data + demo.qpos + 4, 4);
		if (memcmp(demo.qbuf.data + demo.qpos, DEMO_MAGIC, 4) || ver != DEMO_VERSION) {
			Con_Printf("the QTV stream isn't from this version of Bulwark\n");
			end_playback(false);
			return false;
		}
		demo.qpos += 8;
		demo.qtv_magic = true;
	}
	return true;
}

static void Qtvplay_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Usage: %s [stream@][tls://]hostname[:port] [password]\n", argv[0]);
		return;
	}
	demo.numloop = 0;
	demo.want_track = -1;
	qtv_start(argv[1], argc > 2 ? argv[2] : "");
}

/* asks a QTV server something and prints the header lines that answer it */
static void qtv_query(const char *host, const char *what, const char *key, const char *heading)
{
	netadr_t adr;
	char stream[64], buf[4096];
	strbuf_t in = {0};
	char req[300];
	int s;
	double until;
	bool sent = false;
	if (!host || !host[0] || !qtv_address(host, &adr, stream, sizeof(stream)) || (s = NET_TCPConnect(&adr)) < 0) {
		Con_Printf("Couldn't connect to proxy\n");
		return;
	}
	snprintf(req, sizeof(req), "BWQTV 1\nPASSWORD: %s\n%s\n\n", Cvar_String("qtv_password_client"), what);
	until = Sys_Seconds() + 3;
	while (Sys_Seconds() < until) {
		int n;
		if (!sent) {
			n = NET_TCPSend(s, req, strlen(req));
			if (n < 0)
				break;
			sent = n > 0;
		}
		while ((n = NET_TCPRecv(s, buf, sizeof(buf))) > 0)
			SB_Append(&in, buf, (size_t)n);
		SB_Append(&in, "", 1);
		in.len--;
		if (n < 0 || strstr(in.data, "\n\n"))
			break;
		Sys_Sleep(0.01);
	}
	NET_TCPClose(s);
	if (!in.len || strncmp(in.data, "BWQTV 1", 7)) {
		Con_Printf("Couldn't connect to proxy\n");
		SB_Free(&in);
		return;
	}
	{
		int count = 0;
		size_t kl = strlen(key);
		Con_Printf("%s on %s:\n", heading, host);
		for (char *l = in.data; l && *l;) {
			char *e = strchr(l, '\n');
			if (e)
				*e = 0;
			if (!strncmp(l, "ERROR:", 6))
				Con_Printf("QTV:%s\n", l + 6);
			else if (!strncmp(l, key, kl) && l[kl] == ':') {
				Con_Printf("  %s\n", l + kl + 2);
				count++;
			}
			l = e ? e + 1 : NULL;
		}
		Con_Printf("%d found\n", count);
	}
	SB_Free(&in);
}

static void Qtvlist_f(int argc, const char **argv)
{
	qtv_query(argc > 1 ? argv[1] : last_qtv_host, "LIST", "STREAM", "streams");
}

static void Qtvdemos_f(int argc, const char **argv)
{
	qtv_query(argc > 1 ? argv[1] : last_qtv_host, "DEMOS", "DEMO", "demos");
}

/* --- tracking: whose view a server demo or stream shows ------------------------ */

static bool has_view(int slot) { return slot >= 0 && slot < 256 && (demo.seen[slot >> 3] & (1 << (slot & 7))); }

/* a player number (1-based, as the scoreboard counts) or name */
static int find_player(const char *arg)
{
	char name[64];
	bool numeric = arg[0] != 0;
	for (const char *p = arg; *p; p++)
		numeric &= *p >= '0' && *p <= '9';
	if (numeric)
		return atoi(arg) - 1;
	for (int i = 0; i < CL_MAX_PLAYERS; i++)
		if (CL_PlayerKey(i, "name", name, sizeof(name))[0] && !Str_ICmp(name, arg))
			return i;
	for (int i = 0; i < CL_MAX_PLAYERS; i++)
		if (CL_PlayerKey(i, "name", name, sizeof(name))[0] && !Str_NICmp(name, arg, strlen(arg)))
			return i;
	return -1;
}

static void switch_track(int slot)
{
	if (slot == demo.track)
		return;
	demo.want_track = slot;
	if (demo.qtv) {
		char src[256], pwd[128];
		Str_Copy(src, demo.qtv_source, sizeof(src));
		Str_Copy(pwd, demo.qtv_password, sizeof(pwd));
		qtv_start(src, pwd);
	} else if (demo.play) {
		char name[256];
		double t = demo.play_time;
		Str_Copy(name, demo.play_name, sizeof(name));
		if (!start_playback(name, false))
			return;
		demo.seek_to = t;
		demo.play_time = t;
	}
}

static float player_score(int slot)
{
	char v[32];
	const char *key = cl_autotrack->string;
	if (!key[0] || !Str_ICmp(key, "auto") || !strcmp(key, "1"))
		key = "frags";
	return (float)atof(CL_PlayerKey(slot, key, v, sizeof(v)));
}

/* follows whoever is doing best */
static void autotrack_frame(void)
{
	int best = -1;
	char team[32], want[32];
	if (!autotrack_on || !demo.multiview || demo.track < 0 || cl.realtime < demo.next_autotrack || demo.seek_to > 0)
		return;
	demo.next_autotrack = cl.realtime + 1;
	Str_Copy(want, cl_autotrack_team->string, sizeof(want));
	for (int i = 0; i < 256; i++) {
		if (!has_view(i))
			continue;
		if (want[0] && Str_ICmp(CL_PlayerKey(i, "team", team, sizeof(team)), want))
			continue;
		if (best < 0 || player_score(i) > player_score(best))
			best = i;
	}
	if (best >= 0 && best != demo.track && (!has_view(demo.track) || player_score(best) > player_score(demo.track)))
		switch_track(best);
}

static void Track_f(int argc, const char **argv)
{
	int seat = isdigit((unsigned char)argv[0][5]) ? argv[0][5] - '1' : 0, slot;
	if (argc < 2) {
		Con_Printf("Usage: %s userid|nick|off\n", argv[0]);
		return;
	}
	if (seat > 0) {
		Con_Printf("there is only one player view (seat 1)\n");
		return;
	}
	if (!demo.multiview) {
		/* in a live game, the game's spectator code can follow the request */
		if (cl.state < ca_loading || cl.demoplayback) {
			Con_Printf("Not connected.\n");
			return;
		}
		if (!Str_ICmp(argv[1], "off"))
			CL_SendStringCmd("ptrack");
		else if ((slot = find_player(argv[1])) >= 0)
			CL_SendStringCmd(va("ptrack %d", slot));
		else
			Con_Printf("no player %s\n", argv[1]);
		return;
	}
	autotrack_on = false;
	if (!Str_ICmp(argv[1], "off")) {
		Con_Printf("not tracking anyone\n");
		return;
	}
	slot = find_player(argv[1]);
	if (!has_view(slot)) {
		Con_Printf("%s has no view in this demo\n", argv[1]);
		return;
	}
	switch_track(slot);
}

static void Autotrack_f(int argc, const char **argv)
{
	autotrack_on = !autotrack_on;
	demo.next_autotrack = 0;
	Con_Printf("autotrack %s\n", autotrack_on ? "on" : "off");
}

/* called instead of reading the network while a demo plays */
void CL_DemoReadPackets(double dt)
{
	bool frame_done = false;
	if (!demo.play && !demo.qtv)
		return;
	if (demo.qtv && !qtv_pump())
		return;
	autotrack_frame();
	if (!demo.paused && !demo.timedemo)
		demo.play_time += dt * BW_MAX(0.0f, demo_timescale->value);
	if (demo.timedemo)
		demo.td_frames++;
	for (;;) {
		if (!demo.have_next && !read_next()) {
			if (demo.qtv && !demo.qtv_closed)
				return; /* more is on its way */
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
		if (demo.next_type == DR_VIEW) {
			if (demo.next_len == 1) {
				int v = demo.next_data[0];
				demo.curview = v;
				demo.multiview = true;
				demo.seen[v >> 3] |= (uint8_t)(1 << (v & 7));
				if (demo.track < 0)
					demo.track = v;
			}
			continue;
		}
		/* another player's view */
		if (demo.curview >= 0 && demo.curview != demo.track)
			continue;
		switch (demo.next_type) {
		case DR_RELIABLE:
		case DR_UNRELIABLE: {
			msg_t m;
			MSG_Init(&m, demo.next_data, demo.next_len);
			m.cursize = demo.next_len;
			CL_ParseServerMessage(&m, demo.next_type == DR_RELIABLE);
			if (!demo.play && !demo.qtv)
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
	else if (demo.play || demo.qtv)
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
		Con_Printf(demo.qtv ? "can't move around in a live stream\n" : "usage: %s <seconds> (+n or -n to move by n)\n", argv[0]);
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
		demo.want_track = demo.track;
		if (!start_playback(name, false))
			return;
	}
	demo.seek_to = target;
	demo.play_time = target;
}

static void DemoInfo_f(int argc, const char **argv)
{
	if (demo.play || demo.qtv)
		Con_Printf("%s %s at %.2f s%s, timescale %g\n", demo.qtv ? "watching QTV" : "playing", demo.play_name, demo.play_time,
		           demo.paused ? " (paused)" : "", demo_timescale->value);
	if (demo.multiview && demo.track >= 0) {
		char name[64];
		Con_Printf("tracking player %d (%s)%s\n", demo.track + 1, CL_PlayerKey(demo.track, "name", name, sizeof(name)),
		           autotrack_on ? ", autotrack on" : "");
	}
	if (demo.rec)
		Con_Printf("recording %s%s\n", demo.rec_name, demo.waiting ? " (waiting for the game)" : "");
	if (!demo.play && !demo.qtv && !demo.rec)
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

typedef struct {
	int n, want;
	char found[256];
} demolist_t;

static void list_demo(const char *name, const char *searchpath, void *ctx)
{
	demolist_t *l = ctx;
	l->n++;
	if (l->want == 0)
		Con_Printf("%3d: %-40s %s\n", l->n, name, searchpath);
	else if (l->want == l->n)
		Str_Copy(l->found, name, sizeof(l->found));
}

static void Demolist_f(int argc, const char **argv)
{
	demolist_t l = {0};
	FS_List("*.dem", true, list_demo, &l);
	FS_List("demos/*.dem", true, list_demo, &l);
	FS_List("demos/*.mvd", true, list_demo, &l);
	Con_Printf("%d demos\n", l.n);
}

/* rmdemonum <#>: delete a demo by its number in demolist */
static void Rmdemonum_f(int argc, const char **argv)
{
	demolist_t l = {0};
	char path[1024];
	if (argc < 2 || (l.want = atoi(argv[1])) <= 0) {
		Con_Printf("rmdemonum <#>\n");
		return;
	}
	FS_List("*.dem", true, list_demo, &l);
	FS_List("demos/*.dem", true, list_demo, &l);
	FS_List("demos/*.mvd", true, list_demo, &l);
	if (!l.found[0]) {
		Con_Printf("no demo number %d\n", l.want);
		return;
	}
	if (!FS_WritePath(l.found, path, sizeof(path)) || remove(path))
		Con_Printf("can't delete %s\n", l.found);
	else
		Con_Printf("deleted %s\n", l.found);
}

/* cancel: stop recording and throw the demo away */
static void Cancel_f(int argc, const char **argv)
{
	char rel[256], path[1024];
	if (!demo.rec) {
		Con_Printf("Not recording a demo.\n");
		return;
	}
	Str_Copy(rel, demo.rec_name, sizeof(rel));
	fclose(demo.rec);
	demo.rec = NULL;
	demo.waiting = false;
	if (FS_WritePath(rel, path, sizeof(path)))
		remove(path);
	Con_Printf("recording of %s cancelled\n", rel);
}

/* finds the time of the next record after the current one that matches;
 * the read position is put back, so playback carries on unchanged */
static bool scan_ahead(bool (*match)(int type, const uint8_t *data, uint32_t len), double *when)
{
	long pos = ftell(demo.play);
	bool had = demo.have_next, found = false;
	double ntime = demo.next_time;
	int ntype = demo.next_type;
	uint32_t nlen = demo.next_len;
	uint8_t *copy = NULL;
	if (had && nlen) {
		copy = Mem_Alloc(nlen);
		memcpy(copy, demo.next_data, nlen);
	}
	while (read_next())
		if (match(demo.next_type, demo.next_data, demo.next_len)) {
			*when = demo.next_time;
			found = true;
			break;
		}
	fseek(demo.play, pos, SEEK_SET);
	demo.have_next = had;
	demo.next_time = ntime;
	demo.next_type = ntype;
	demo.next_len = nlen;
	if (copy) {
		memcpy(demo.next_data, copy, nlen);
		Mem_Free(copy);
	}
	return found;
}

static bool is_mark(int type, const uint8_t *data, uint32_t len)
{
	static const char mark[] = "//demomark";
	if (type != DR_RELIABLE || len < sizeof(mark) - 1)
		return false;
	for (uint32_t i = 0; i + sizeof(mark) - 1 <= len; i++)
		if (!memcmp(data + i, mark, sizeof(mark) - 1))
			return true;
	return false;
}

static void seek_forward(double t)
{
	demo.seek_to = t;
	demo.play_time = t;
}

/* demo_jump_mark: on to the next "//demomark" a server stuffed in */
static void DemoJumpMark_f(int argc, const char **argv)
{
	double t;
	if (!demo.play) {
		Con_Printf("not playing a demo, cannot jump.\n");
		return;
	}
	if (scan_ahead(is_mark, &t))
		seek_forward(t);
	else
		Con_Printf("no more marks in the demo\n");
}

/* demo_jump_end: the end of the demo. FTE stops at the next intermission;
 * Bulwark's protocol has no intermission message, so this goes to the last
 * record. */
static void DemoJumpEnd_f(int argc, const char **argv)
{
	double last = -1;
	if (!demo.play) {
		Con_Printf("not playing a demo, cannot jump.\n");
		return;
	}
	/* the last record's time, reading only the headers */
	{
		FILE *f = demo.play;
		long pos = ftell(f);
		double tt;
		uint8_t ty;
		uint32_t len;
		while (fread(&tt, sizeof(tt), 1, f) == 1 && fread(&ty, 1, 1, f) == 1 && fread(&len, sizeof(len), 1, f) == 1 &&
		       !fseek(f, (long)len, SEEK_CUR))
			last = tt;
		fseek(f, pos, SEEK_SET);
	}
	if (last > demo.play_time)
		seek_forward(last - 0.05);
}

/* demo_nudge [frames]: step a few server frames forward or back */
static void DemoNudge_f(int argc, const char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 1;
	double step = cl.tickrate > 0 ? 1.0 / cl.tickrate : 1.0 / 72;
	char arg[64];
	const char *jargv[2] = {"demo_jump", arg};
	if (!demo.play) {
		Con_Printf("not playing a demo, cannot nudge.\n");
		return;
	}
	snprintf(arg, sizeof(arg), "%+f", frames * step);
	DemoJump_f(2, jargv);
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
	if (demo.qtv)
		qtv_close();
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
	Cmd_AddD("rmdemonum", Rmdemonum_f, "rmdemonum <#>: delete a demo by its demolist number");
	Cmd_AddD("cancel", Cancel_f, "Stop recording and delete the demo");
	Cmd_AddD("demo_jump_mark", DemoJumpMark_f, "Jump to the next //demomark the server sent");
	Cmd_AddD("demo_jump_end", DemoJumpEnd_f, "Jump to the end of the demo");
	Cmd_AddD("demo_nudge", DemoNudge_f, "demo_nudge [frames]: step server frames forward (or back, negative)");
	Cmd_AddD("rerecord", Rerecord_f, "Record again under the last name, from the start of the map");
	Cmd_AddD("rmdemo", Rmdemo_f, "rmdemo <name>: delete a demo");
	cl_autotrack = Cvar_Get("cl_autotrack", "auto", CVAR_ARCHIVE, "What autotrack follows: auto (frags) or a player key holding a score");
	cl_autotrack_team = Cvar_Get("cl_autotrack_team", "", CVAR_ARCHIVE, "Autotrack only this team's players (empty: anyone)");
	Cvar_Get("qtv_password_client", "", 0, "Password qtvlist and qtvdemos send");
	demo.qtv_sock = -1;
	demo.want_track = -1;
	Cmd_AddD("qtvplay", Qtvplay_f, "qtvplay [stream@]host[:port] [password]: watch a game (or demo:<name>@host) streamed over QTV");
	Cmd_AddD("qtvlist", Qtvlist_f, "qtvlist [host[:port]]: the streams a QTV server has");
	Cmd_AddD("qtvdemos", Qtvdemos_f, "qtvdemos [host[:port]]: the demos a QTV server has");
	Cmd_AddD("track", Track_f, "track <player|off>: whose view a server demo or QTV stream shows");
	Cmd_AddD("track1", Track_f, "track1 <player|off>: whose view seat 1 shows");
	Cmd_AddD("track2", Track_f, "track2 <player|off>: whose view seat 2 shows");
	Cmd_AddD("track3", Track_f, "track3 <player|off>: whose view seat 3 shows");
	Cmd_AddD("track4", Track_f, "track4 <player|off>: whose view seat 4 shows");
	Cmd_AddD("autotrack", Autotrack_f, "Follow the best player in a server demo or QTV stream");
	Cmd_AddD("demo_setspeed", DemoSetspeed_f, "demo_setspeed <scale>: playback speed (demo_timescale)");
}
