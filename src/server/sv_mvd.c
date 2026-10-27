/* Bulwark engine - server side demos and live streams (the sv_demo/mvd and
 * QTV commands). A server recording holds every connected player's view:
 * each player gets their own copy of what they were sent, built from the
 * recording's own entity state, so the stream starts cleanly whenever the
 * recording does and nothing in it is ever lost. On playback, "track"
 * picks whose view is shown.
 *
 * The file is a Bulwark client demo ("BWDM", see cl_demo.c) with one more
 * record type: 5, a uint8 player slot that the records after it belong to.
 * A client demo has no such records, so either kind plays with playdemo.
 *
 * Live streams use the same bytes over TCP (qtv_streamport). A request is
 * text lines ending with an empty line:
 *   BWQTV 1
 *   PASSWORD: <qtv_password>
 *   SOURCE: <stream> | DEMO: <name> | LIST | DEMOS | REVERSE: <name>
 * and the answer is "BWQTV 1" and header lines (OK, ERROR: text, LIVE:
 * <seconds into the stream>, STREAM: name, DEMO: name size) up to an empty
 * line, then for OK the stream. REVERSE comes from another server's
 * qtvreverse: it sends its game, and this server passes it on to its own
 * viewers as the stream <name>.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../net/net.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEMO_MAGIC "BWDM"
#define DEMO_VERSION 1
enum { DR_RELIABLE = 1, DR_UNRELIABLE = 2, DR_ANGLES = 3, DR_USERCMD = 4, DR_VIEW = 5 };
#define QTV_DEFAULT_PORT 27599
#define QTV_MAX_BACKLOG (16u << 20) /* bytes a slow viewer may fall behind before being dropped */
#define MAX_SINKS 32
#define MAX_RELAYS 8

typedef struct {
	client_t sh; /* the recording's entity state for this player */
	double next; /* next snapshot due */
} view_t;

typedef enum { SINK_FILE, SINK_QTV } sinkkind_t;

typedef struct {
	bool used;
	sinkkind_t kind;
	FILE *f;
	char name[256]; /* demo file, relative to the game dir, or the viewer's address */
	double start;
	int curview;
	view_t *views[256];
	/* SINK_QTV */
	int sock;
	strbuf_t out;
	size_t sent;
	bool closing; /* drop once the backlog is sent */
} sink_t;

/* a stream another server pushes to this one (qtvreverse) */
typedef struct {
	bool used;
	char name[64];
	int sock;
	strbuf_t data; /* everything so far, so late viewers can be caught up */
	double start;
	int viewers[MAX_SINKS]; /* QTV connection indices + 1 */
} relay_t;

/* a TCP connection still sending its request */
typedef struct {
	int sock;
	netadr_t from;
	strbuf_t in;
	double since;
	int relay_for; /* >= 0: a QTV viewer of this relay, after its request */
	size_t relay_sent;
} qtvconn_t;

static sink_t sinks[MAX_SINKS];
static relay_t relays[MAX_RELAYS];
static qtvconn_t conns[MAX_SINKS];
static int numconns;
static int listener = -1, listen_port;
static sink_t *recording; /* the file being recorded (one at a time, like FTE) */
static uint8_t *snapbuf;
#define SNAPBUF_SIZE (1u << 20)

static cvar_t *sv_demoDir, *sv_demofps, *sv_demoAutoRecord, *sv_demoAutoPrefix, *sv_demoPrefix, *sv_demoSuffix,
	*sv_demotxt, *sv_demoMaxDirSize, *sv_demoMaxSize, *sv_demoMaxDirCount, *sv_demoClearOld, *sv_demoPings;
static cvar_t *qtv_streamport, *qtv_password, *qtv_maxstreams;
static float last_frags[256];
static bool frags_known[256];
static double next_frags;

/* --- writing records --------------------------------------------------- */

static void sink_bytes(sink_t *s, const void *data, size_t len)
{
	if (s->kind == SINK_FILE)
		fwrite(data, 1, len, s->f);
	else
		SB_Append(&s->out, data, len);
}

static void emit(sink_t *s, int type, const void *data, uint32_t len)
{
	double t = host_time - s->start;
	uint8_t ty = (uint8_t)type;
	sink_bytes(s, &t, sizeof(t));
	sink_bytes(s, &ty, 1);
	sink_bytes(s, &len, sizeof(len));
	if (len)
		sink_bytes(s, data, len);
}

static void put(sink_t *s, int type, int view, const void *data, uint32_t len)
{
	if (view >= 0 && view != s->curview) {
		uint8_t v = (uint8_t)view;
		emit(s, DR_VIEW, &v, 1);
		s->curview = view;
	}
	emit(s, type, data, len);
}

static int slot_of(const client_t *c) { return (int)(c - svs.clients); }

/* starts recording player c's view: the signon they'd get now, then (if
 * they're already in the game) the signon's end */
static void add_view(sink_t *s, client_t *c, bool spawned)
{
	strbuf_t sig = {0};
	int slot = slot_of(c);
	view_t *v;
	size_t at = 0;
	if (slot < 0 || slot > 255 || s->views[slot])
		return;
	v = s->views[slot] = Mem_Alloc(sizeof(view_t));
	v->sh.edict = c->edict;
	SV_ShadowReset(&v->sh);
	SV_CaptureSignon(c, &sig);
	while (at + 4 <= sig.len) {
		uint32_t n;
		memcpy(&n, sig.data + at, 4);
		at += 4;
		if (at + n > sig.len)
			break;
		put(s, DR_RELIABLE, slot, sig.data + at, n);
		at += n;
	}
	SB_Free(&sig);
	SV_ResetStats(c); /* so the recording gets every stat */
	if (spawned) {
		uint8_t m[2] = {svc_signon, 2};
		put(s, DR_RELIABLE, slot, m, 2);
	}
}

static void drop_view(sink_t *s, int slot)
{
	if (!s->views[slot])
		return;
	SV_ShadowFree(&s->views[slot]->sh);
	Mem_Free(s->views[slot]);
	s->views[slot] = NULL;
}

static sink_t *new_sink(sinkkind_t kind)
{
	for (int i = 0; i < MAX_SINKS; i++)
		if (!sinks[i].used) {
			sink_t *s = &sinks[i];
			memset(s, 0, sizeof(*s));
			s->used = true;
			s->kind = kind;
			s->sock = -1;
			s->curview = -1;
			s->start = host_time;
			return s;
		}
	return NULL;
}

static void begin_sink(sink_t *s)
{
	uint32_t ver = DEMO_VERSION;
	sink_bytes(s, DEMO_MAGIC, 4);
	sink_bytes(s, &ver, sizeof(ver));
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_SPAWNED && !c->bot)
			add_view(s, c, true);
	}
}

static void free_sink(sink_t *s)
{
	for (int i = 0; i < 256; i++)
		drop_view(s, i);
	if (s->f)
		fclose(s->f);
	NET_TCPClose(s->sock);
	SB_Free(&s->out);
	if (s == recording)
		recording = NULL;
	memset(s, 0, sizeof(*s));
	s->sock = -1;
}

/* --- hooks from sv_client.c --------------------------------------------- */

void SV_MVDReliable(client_t *c, const void *data, size_t len)
{
	int slot = slot_of(c);
	for (int i = 0; i < MAX_SINKS; i++)
		if (sinks[i].used && sinks[i].views[slot])
			put(&sinks[i], DR_RELIABLE, slot, data, (uint32_t)len);
}

void SV_MVDPendingFlags(int e, uint32_t flags)
{
	for (int i = 0; i < MAX_SINKS; i++) {
		if (!sinks[i].used)
			continue;
		for (int k = 0; k < svs.maxclients && k < 256; k++) {
			view_t *v = sinks[i].views[k];
			if (v && e < v->sh.estate_count)
				v->sh.pending[e] |= flags;
		}
	}
}

void SV_MVDSnapshot(client_t *c, const uint8_t *dgram, size_t len)
{
	int slot = slot_of(c);
	float fps = BW_MAX(1.0f, sv_demofps->value);
	for (int i = 0; i < MAX_SINKS; i++) {
		sink_t *s = &sinks[i];
		view_t *v = s->used ? s->views[slot] : NULL;
		msg_t m;
		if (!v)
			continue;
		if (len)
			put(s, DR_UNRELIABLE, slot, dgram, (uint32_t)len);
		if (host_time < v->next)
			continue;
		v->next = BW_MAX(v->next + 1.0 / fps, host_time - 0.5 / fps);
		if (!snapbuf)
			snapbuf = Mem_Alloc(SNAPBUF_SIZE);
		MSG_Init(&m, snapbuf, SNAPBUF_SIZE);
		m.allowoverflow = true;
		SV_ShadowSnapshot(c, &v->sh, &m);
		if (!m.overflowed)
			put(s, DR_UNRELIABLE, slot, m.data, (uint32_t)m.cursize);
		put(s, DR_ANGLES, slot, c->lastucmd.angles, sizeof(float) * 3);
	}
}

void SV_MVDUsercmd(client_t *c, const usercmd_t *u)
{
	int slot = slot_of(c);
	for (int i = 0; i < MAX_SINKS; i++)
		if (sinks[i].used && sinks[i].views[slot])
			put(&sinks[i], DR_USERCMD, slot, u, sizeof(*u));
}

void SV_MVDClientSpawning(client_t *c)
{
	int slot = slot_of(c);
	for (int i = 0; i < MAX_SINKS; i++) {
		sink_t *s = &sinks[i];
		if (!s->used)
			continue;
		if (s->views[slot])
			SV_ShadowReset(&s->views[slot]->sh); /* its stream already has the new signon */
		else
			add_view(s, c, false); /* signon 2 follows from spawn_client */
	}
}

void SV_MVDClientDropped(client_t *c)
{
	int slot = slot_of(c);
	for (int i = 0; i < MAX_SINKS; i++)
		if (sinks[i].used)
			drop_view(&sinks[i], slot);
	frags_known[slot & 255] = false;
}

/* --- recording to files -------------------------------------------------- */

static const char *demo_dir(void) { return sv_demoDir->string[0] ? sv_demoDir->string : "demos"; }

/* path of demos/<name><ext> in the write directory */
static bool demo_ospath(const char *name, const char *ext, char *out, size_t size)
{
	char rel[512];
	snprintf(rel, sizeof(rel), "%s/%s%s", demo_dir(), name, ext);
	return FS_SafeName(rel) && FS_WritePath(rel, out, size);
}

static void strip_ext(char *name)
{
	char *dot = strrchr(name, '.');
	if (dot && !Str_ICmp(dot, ".mvd"))
		*dot = 0;
}

typedef struct {
	char name[128];
	long size;
	time_t mtime;
} demofile_t;

static int cmp_demofile(const void *a, const void *b)
{
	const demofile_t *x = a, *y = b;
	return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : strcmp(x->name, y->name);
}

/* the demos on disk, oldest first; returns the count, *out to Mem_Free */
static int list_demos(demofile_t **out, const char *filter)
{
	char dir[1024];
	DIR *d;
	struct dirent *de;
	int n = 0, cap = 0;
	*out = NULL;
	if (!demo_ospath("", "", dir, sizeof(dir)) || !(d = opendir(dir)))
		return 0;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name);
		char full[1400];
		struct stat st;
		if (len < 5 || Str_ICmp(de->d_name + len - 4, ".mvd"))
			continue;
		if (filter && filter[0] && !strstr(de->d_name, filter))
			continue;
		snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (stat(full, &st))
			continue;
		if (n == cap)
			*out = Mem_Realloc(*out, sizeof(demofile_t) * (size_t)(cap = cap ? cap * 2 : 32));
		Str_Copy((*out)[n].name, de->d_name, sizeof((*out)[n].name));
		(*out)[n].size = (long)st.st_size;
		(*out)[n].mtime = st.st_mtime;
		n++;
	}
	closedir(d);
	if (n)
		qsort(*out, (size_t)n, sizeof(demofile_t), cmp_demofile);
	return n;
}

static void size_string(double bytes, char *out, size_t size)
{
	if (bytes >= 1024.0 * 1024 * 1024)
		snprintf(out, size, "%gGB", bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024.0 * 1024)
		snprintf(out, size, "%gMB", bytes / (1024.0 * 1024));
	else if (bytes >= 1024)
		snprintf(out, size, "%gKB", bytes / 1024);
	else
		snprintf(out, size, "%gB", bytes);
}

/* "100mb", "1.5g", "4096" */
static double parse_size(const char *s)
{
	char *end;
	double v = strtod(s, &end);
	while (*end == ' ')
		end++;
	switch (*end) {
	case 'k': case 'K': return v * 1024;
	case 'm': case 'M': return v * 1024 * 1024;
	case 'g': case 'G': return v * 1024 * 1024 * 1024;
	default: return v;
	}
}

static bool recording_is(const char *name)
{
	char base[256];
	const char *slash;
	if (!recording)
		return false;
	slash = strrchr(recording->name, '/');
	Str_Copy(base, slash ? slash + 1 : recording->name, sizeof(base));
	return !strcmp(base, name);
}

static void write_txt_header(const char *name)
{
	char path[1024], date[64];
	FILE *f;
	time_t t = time(NULL);
	if (!sv_demotxt->ivalue || !demo_ospath(name, ".txt", path, sizeof(path)) || !(f = fopen(path, "w")))
		return;
	strftime(date, sizeof(date), "%a %b %d, %H:%M:%S %Y", localtime(&t));
	fprintf(f, "date %s\nmap %s\nteamplay %g\ndeathmatch %g\ntimelimit %g\n\n", date, sv.mapname,
	        Cvar_Value("teamplay"), Cvar_Value("deathmatch"), Cvar_Value("timelimit"));
	fclose(f);
}

/* the scores at the end, after the header and anything sv_demoinfoadd put in */
static void write_txt_players(const char *name)
{
	char path[1024];
	FILE *f;
	qcdef_t *frags = sv.vm ? QC_FindField(sv.vm, "frags") : NULL;
	if (!sv_demotxt->ivalue || !demo_ospath(name, ".txt", path, sizeof(path)) || !(f = fopen(path, "a")))
		return;
	fprintf(f, "players\n");
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED)
			continue;
		fprintf(f, "  %4d %s\n", frags ? (int)QC_EdictFields(sv.vm, c->edict)[frags->ofs].f : 0, c->name);
	}
	fclose(f);
}

/* keeps the demo folder within sv_demoMaxDirCount and sv_demoMaxDirSize
 * by deleting the oldest (sv_demoClearOld) */
static bool make_room(void)
{
	demofile_t *list;
	int n = list_demos(&list, NULL), first = 0;
	double total = 0, maxsize = parse_size(sv_demoMaxDirSize->string);
	for (int i = 0; i < n; i++)
		total += list[i].size;
	while (n - first >= sv_demoMaxDirCount->ivalue && sv_demoMaxDirCount->ivalue > 0 && first < n) {
		char path[1024], name[128];
		if (!sv_demoClearOld->ivalue)
			break;
		Str_Copy(name, list[first].name, sizeof(name));
		strip_ext(name);
		if (demo_ospath(name, ".mvd", path, sizeof(path)))
			remove(path);
		if (demo_ospath(name, ".txt", path, sizeof(path)))
			remove(path);
		total -= list[first++].size;
	}
	while (maxsize > 0 && total >= maxsize && first < n && sv_demoClearOld->ivalue) {
		char path[1024], name[128];
		Str_Copy(name, list[first].name, sizeof(name));
		strip_ext(name);
		if (demo_ospath(name, ".mvd", path, sizeof(path)))
			remove(path);
		if (demo_ospath(name, ".txt", path, sizeof(path)))
			remove(path);
		total -= list[first++].size;
	}
	Mem_Free(list);
	if (maxsize > 0 && total >= maxsize) {
		Con_Printf("insufficient directory space, increase sv_demoMaxDirSize\n");
		return false;
	}
	if (sv_demoMaxDirCount->ivalue > 0 && n - first >= sv_demoMaxDirCount->ivalue) {
		Con_Printf("too many demos in the directory, increase sv_demoMaxDirCount\n");
		return false;
	}
	return true;
}

static bool start_recording(const char *name)
{
	char base[200], path[1024];
	sink_t *s;
	if (!sv.active) {
		Con_Printf("not running a server\n");
		return false;
	}
	snprintf(base, sizeof(base), "%s%s%s", sv_demoPrefix->string, name, sv_demoSuffix->string);
	strip_ext(base);
	if (!demo_ospath(base, ".mvd", path, sizeof(path))) {
		Con_Printf("bad demo name %s\n", name);
		return false;
	}
	if (recording)
		free_sink(recording);
	if (!make_room())
		return false;
	if (!(s = new_sink(SINK_FILE)))
		return false;
	if (!FS_CreatePath(path) || !(s->f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", path);
		free_sink(s);
		return false;
	}
	snprintf(s->name, sizeof(s->name), "%s/%s.mvd", demo_dir(), base);
	recording = s;
	begin_sink(s);
	write_txt_header(base);
	Con_Printf("Server starts recording:\n%s\n", s->name);
	return true;
}

/* name without folder and extension */
static void recording_base(char *out, size_t size)
{
	const char *slash = strrchr(recording->name, '/');
	Str_Copy(out, slash ? slash + 1 : recording->name, size);
	strip_ext(out);
}

static void stop_recording(bool cancel)
{
	char base[200], path[1024];
	if (!recording) {
		Con_Printf("Not recording a demo.\n");
		return;
	}
	recording_base(base, sizeof(base));
	free_sink(recording);
	if (cancel) {
		if (demo_ospath(base, ".mvd", path, sizeof(path)))
			remove(path);
		if (demo_ospath(base, ".txt", path, sizeof(path)))
			remove(path);
		Con_Printf("Server recording canceled, demo removed\n");
		return;
	}
	write_txt_players(base);
	SV_BroadcastPrint(2, "Server recording complete\n");
	{
		char hint[300];
		snprintf(hint, sizeof(hint), "^1/download \"%s/%s.mvd\"\n", demo_dir(), base);
		SV_BroadcastPrint(2, hint);
	}
}

static void Record_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("mvdrecord <demoname>\n");
		return;
	}
	start_recording(argv[1]);
}

/* a name from the game: ffa_<map>, team_<map> or <a>_vs_<b>_<map>, made unique with (n) */
static void easy_name(const char *prefix, char *out, size_t size)
{
	char stem[160];
	int players = 0;
	const char *a = NULL, *b = NULL;
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		players++;
		if (!a)
			a = c->name;
		else if (!b)
			b = c->name;
	}
	if (players == 2 && a && b && !Cvar_Value("teamplay"))
		snprintf(stem, sizeof(stem), "%s%s_vs_%s_%s", prefix, a, b, sv.mapname);
	else
		snprintf(stem, sizeof(stem), "%s%s_%s", prefix, Cvar_Value("teamplay") ? "team" : "ffa", sv.mapname);
	for (char *p = stem; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			*p = '_';
	for (int n = 0; n < 1000; n++) {
		char path[1024];
		snprintf(out, size, "%s(%d)", stem, n);
		if (!demo_ospath(out, ".mvd", path, sizeof(path)))
			break;
		FILE *f = fopen(path, "rb");
		if (!f)
			return;
		fclose(f);
	}
}

static void EasyRecord_f(int argc, const char **argv)
{
	char name[200];
	if (argc > 1) {
		Str_Copy(name, argv[1], sizeof(name));
		start_recording(name);
		return;
	}
	easy_name("", name, sizeof(name));
	start_recording(name);
}

static void Stop_f(int argc, const char **argv) { stop_recording(false); }
static void Cancel_f(int argc, const char **argv) { stop_recording(true); }

static void List_f(int argc, const char **argv)
{
	demofile_t *list;
	int n = list_demos(&list, argc > 1 ? argv[1] : NULL);
	double total = 0, maxsize = parse_size(sv_demoMaxDirSize->string);
	char a[64], b[64];
	Con_Printf("content of %s/*.mvd\n", demo_dir());
	if (!n)
		Con_Printf("no demos\n");
	for (int i = 0; i < n; i++) {
		bool live = recording_is(list[i].name);
		if (live)
			Con_Printf("*%d: %s %ldk\n", i + 1, list[i].name, (long)(ftell(recording->f) / 1024));
		Con_Printf("%d: %s %ldk\n", i + 1, list[i].name, list[i].size / 1024);
		total += list[i].size;
	}
	Mem_Free(list);
	size_string(total, a, sizeof(a));
	size_string(BW_MAX(0, maxsize - total), b, sizeof(b));
	Con_Printf("\ndirectory size: %s\nspace available: %s\n", a, b);
}

/* demo number (1-based, as sv_demolist shows) or "*" for the recording; the name without extension */
static bool demo_by_num(const char *arg, char *out, size_t size)
{
	demofile_t *list;
	int n, want;
	if (!strcmp(arg, "*")) {
		if (!recording)
			return false;
		recording_base(out, size);
		return true;
	}
	want = atoi(arg);
	n = list_demos(&list, NULL);
	if (want < 1 || want > n) {
		Mem_Free(list);
		return false;
	}
	Str_Copy(out, list[want - 1].name, size);
	strip_ext(out);
	Mem_Free(list);
	return true;
}

static bool remove_demo(const char *name)
{
	char path[1024], base[200];
	bool ok = false;
	Str_Copy(base, name, sizeof(base));
	strip_ext(base);
	if (recording_is(va("%s.mvd", base)))
		stop_recording(false);
	if (demo_ospath(base, ".mvd", path, sizeof(path)))
		ok = !remove(path);
	if (demo_ospath(base, ".txt", path, sizeof(path)))
		remove(path);
	return ok;
}

static void Remove_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("sv_demoremove <demoname> - removes the demo\nrmdemo *<token>   - removes demo with <token> in the name\nrmdemo *          - removes all demos\n");
		return;
	}
	if (argv[1][0] == '*') {
		demofile_t *list;
		int n = list_demos(&list, argv[1] + 1), removed = 0;
		for (int i = 0; i < n; i++)
			removed += remove_demo(list[i].name);
		Mem_Free(list);
		Con_Printf("removed %d demo%s\n", removed, removed == 1 ? "" : "s");
		return;
	}
	{
		char base[200];
		Str_Copy(base, argv[1], sizeof(base));
		strip_ext(base);
		if (remove_demo(base))
			Con_Printf("demo %s.mvd successfully removed\n", base);
		else
			Con_Printf("unable to remove demo %s.mvd\n", base);
	}
}

static void NumRemove_f(int argc, const char **argv)
{
	char base[200];
	if (argc < 2) {
		Con_Printf("sv_demonumremove <#>\n");
		return;
	}
	if (!demo_by_num(argv[1], base, sizeof(base))) {
		Con_Printf("invalid demo num\n");
		return;
	}
	if (remove_demo(base))
		Con_Printf("demo %s.mvd successfully removed\n", base);
	else
		Con_Printf("unable to remove demo %s.mvd\n", base);
}

static void info_usage(const char *cmd, const char *rest)
{
	Con_Printf("%s <demonum>%s\n<demonum> = * for currently recorded demo\n", cmd, rest);
}

static void Info_f(int argc, const char **argv)
{
	char base[200], path[1024];
	char *text;
	FILE *f;
	if (argc < 2) {
		info_usage(argv[0], "");
		return;
	}
	if (!demo_by_num(argv[1], base, sizeof(base))) {
		Con_Printf("invalid demo num\n");
		return;
	}
	if (!demo_ospath(base, ".txt", path, sizeof(path)) || !(f = fopen(path, "rb"))) {
		Con_Printf("(empty)\n");
		return;
	}
	text = Mem_Alloc(65537);
	text[fread(text, 1, 65536, f)] = 0;
	fclose(f);
	Con_Printf("%s", text);
	if (text[0] && text[strlen(text) - 1] != '\n')
		Con_Printf("\n");
	Mem_Free(text);
}

static void InfoAdd_f(int argc, const char **argv)
{
	char base[200], path[1024];
	FILE *f;
	if (argc < 3) {
		info_usage(argv[0], " <info string>");
		return;
	}
	if (!demo_by_num(argv[1], base, sizeof(base))) {
		Con_Printf("invalid demo num\n");
		return;
	}
	if (!demo_ospath(base, ".txt", path, sizeof(path)) || !(f = fopen(path, "a"))) {
		Con_Printf("can't write the demo's info\n");
		return;
	}
	for (int i = 2; i < argc; i++)
		fprintf(f, "%s%s", i > 2 ? " " : "", argv[i]);
	fprintf(f, "\n");
	fclose(f);
}

static void InfoRemove_f(int argc, const char **argv)
{
	char base[200], path[1024];
	if (argc < 2) {
		info_usage(argv[0], "");
		return;
	}
	if (!demo_by_num(argv[1], base, sizeof(base))) {
		Con_Printf("invalid demo num\n");
		return;
	}
	if (demo_ospath(base, ".txt", path, sizeof(path)) && !remove(path))
		Con_Printf("file removed\n");
	else
		Con_Printf("no info file\n");
}

/* --- QTV ------------------------------------------------------------------ */

static void conn_close(int i)
{
	NET_TCPClose(conns[i].sock);
	SB_Free(&conns[i].in);
	conns[i] = conns[--numconns];
}

static void reply(int sock, const char *text)
{
	NET_TCPSend(sock, text, strlen(text)); /* small: goes out whole on a fresh socket */
}

static const char *header(const char *req, const char *key, char *out, size_t size)
{
	size_t kl = strlen(key);
	out[0] = 0;
	for (const char *l = req; *l;) {
		const char *e = strchr(l, '\n');
		size_t n = e ? (size_t)(e - l) : strlen(l);
		if (n > kl && !Str_NICmp(l, key, kl) && l[kl] == ':') {
			const char *v = l + kl + 1;
			while (*v == ' ')
				v++;
			Str_Copy(out, v, BW_MIN(size, (size_t)(l + n - v) + 1));
			return out;
		}
		if (!e)
			break;
		l = e + 1;
	}
	return NULL;
}

static int qtv_sinks(void)
{
	int n = 0;
	for (int i = 0; i < MAX_SINKS; i++)
		n += sinks[i].used && sinks[i].kind == SINK_QTV;
	return n;
}

static relay_t *find_relay(const char *name)
{
	for (int i = 0; i < MAX_RELAYS; i++)
		if (relays[i].used && !Str_ICmp(relays[i].name, name))
			return &relays[i];
	return NULL;
}

static void stream_file_to(sink_t *s, const char *base)
{
	char path[1024];
	FILE *f;
	char buf[8192];
	size_t n;
	if (!demo_ospath(base, ".mvd", path, sizeof(path)) || !(f = fopen(path, "rb")))
		return;
	while ((n = fread(buf, 1, sizeof(buf), f)))
		SB_Append(&s->out, buf, n);
	fclose(f);
	s->closing = true;
}

/* handles a complete request; returns false when the connection is done with */
static bool handle_request(int ci)
{
	qtvconn_t *c = &conns[ci];
	const char *req = c->in.data;
	char v[256];
	strbuf_t r = {0};
	bool keep = false;
	if (strncmp(req, "BWQTV 1", 7)) {
		reply(c->sock, "BWQTV 1\nERROR: not a Bulwark QTV request\n\n");
		return false;
	}
	if (qtv_password->string[0] && (!header(req, "PASSWORD", v, sizeof(v)) || strcmp(v, qtv_password->string))) {
		reply(c->sock, "BWQTV 1\nERROR: wrong password\n\n");
		return false;
	}
	SB_AppendStr(&r, "BWQTV 1\n");
	if (strstr(req, "\nLIST\n")) {
		if (sv.active)
			SB_Printf(&r, "STREAM: %s\n", Cvar_String("hostname")[0] ? Cvar_String("hostname") : "game");
		for (int i = 0; i < MAX_RELAYS; i++)
			if (relays[i].used)
				SB_Printf(&r, "STREAM: %s\n", relays[i].name);
		SB_AppendStr(&r, "\n");
	} else if (strstr(req, "\nDEMOS\n")) {
		demofile_t *list;
		int n = list_demos(&list, NULL);
		for (int i = 0; i < n; i++)
			SB_Printf(&r, "DEMO: %s %ld\n", list[i].name, list[i].size);
		Mem_Free(list);
		SB_AppendStr(&r, "\n");
	} else if (header(req, "REVERSE", v, sizeof(v))) {
		relay_t *rl = find_relay(v);
		if (!rl)
			for (int i = 0; i < MAX_RELAYS && !rl; i++)
				if (!relays[i].used)
					rl = &relays[i];
		if (!rl || rl->used) {
			SB_AppendStr(&r, rl ? "ERROR: that stream is already being sent\n\n" : "ERROR: no room for more streams\n\n");
		} else {
			memset(rl, 0, sizeof(*rl));
			rl->used = true;
			Str_Copy(rl->name, v, sizeof(rl->name));
			rl->sock = c->sock;
			rl->start = host_time;
			c->sock = -1; /* the relay owns it now */
			{
				/* the stream follows the request straight away */
				const char *body = strstr(c->in.data, "\n\n") + 2;
				SB_Append(&rl->data, body, c->in.len - (size_t)(body - c->in.data));
			}
			SB_AppendStr(&r, "OK\n\n");
			NET_TCPSend(rl->sock, r.data, r.len);
			Con_Printf("QTV: receiving stream %s from %s\n", rl->name, NET_AdrToString(&c->from));
			SB_Free(&r);
			return false;
		}
	} else {
		sink_t *s;
		const char *demo = header(req, "DEMO", v, sizeof(v));
		char src[256];
		relay_t *rl = NULL;
		if (!demo && header(req, "SOURCE", src, sizeof(src)) && src[0] && Str_ICmp(src, "game") &&
		    Str_ICmp(src, Cvar_String("hostname")) && !(rl = find_relay(src))) {
			SB_Printf(&r, "ERROR: no stream %s\n\n", src);
		} else if (qtv_maxstreams->ivalue > 0 && qtv_sinks() >= qtv_maxstreams->ivalue) {
			SB_AppendStr(&r, "ERROR: too many viewers\n\n");
		} else if (!demo && !rl && !sv.active) {
			SB_AppendStr(&r, "ERROR: no game running\n\n");
		} else if (rl) {
			/* a relay: everything so far, then live */
			SB_Printf(&r, "OK\nLIVE: %f\n\n", host_time - rl->start);
			NET_TCPSend(c->sock, r.data, r.len);
			SB_Free(&r);
			c->relay_for = (int)(rl - relays);
			c->relay_sent = 0;
			c->in.len = 0;
			return true;
		} else if ((s = new_sink(SINK_QTV))) {
			char base[200];
			SB_AppendStr(&r, "OK\nLIVE: 0\n\n");
			SB_Append(&s->out, r.data, r.len);
			s->sock = c->sock;
			c->sock = -1;
			Str_Copy(s->name, NET_AdrToString(&c->from), sizeof(s->name));
			if (demo) {
				uint32_t ver = DEMO_VERSION;
				Str_Copy(base, demo, sizeof(base));
				strip_ext(base);
				(void)ver;
				stream_file_to(s, base);
				if (!s->closing) {
					s->out.len = 0;
					SB_Printf(&s->out, "BWQTV 1\nERROR: no demo %s\n\n", demo);
					s->closing = true;
				}
			} else {
				begin_sink(s);
				Con_Printf("QTV: %s is watching\n", s->name);
			}
			SB_Free(&r);
			return false;
		} else
			SB_AppendStr(&r, "ERROR: no room for more viewers\n\n");
	}
	NET_TCPSend(c->sock, r.data, r.len);
	SB_Free(&r);
	return keep;
}

static void run_relays(void)
{
	char buf[65536];
	for (int i = 0; i < MAX_RELAYS; i++) {
		relay_t *rl = &relays[i];
		int n;
		if (!rl->used)
			continue;
		while ((n = NET_TCPRecv(rl->sock, buf, sizeof(buf))) > 0)
			SB_Append(&rl->data, buf, (size_t)n);
		if (n < 0) {
			Con_Printf("QTV: stream %s ended\n", rl->name);
			NET_TCPClose(rl->sock);
			SB_Free(&rl->data);
			rl->used = false;
		}
	}
}

static void run_qtv(void)
{
	netadr_t from;
	int s;
	if (qtv_streamport->ivalue != listen_port) {
		NET_TCPClose(listener);
		listener = -1;
		listen_port = qtv_streamport->ivalue;
		if (listen_port > 0 && (listener = NET_TCPListen(listen_port)) >= 0)
			Con_Printf("QTV: streaming on TCP port %d\n", listen_port);
	}
	while (numconns < MAX_SINKS && (s = NET_TCPAccept(listener, &from)) >= 0) {
		qtvconn_t *c = &conns[numconns++];
		memset(c, 0, sizeof(*c));
		c->sock = s;
		c->from = from;
		c->since = host_time;
		c->relay_for = -1;
	}
	run_relays();
	for (int i = 0; i < numconns;) {
		qtvconn_t *c = &conns[i];
		char buf[2048];
		int n;
		if (c->relay_for >= 0) {
			relay_t *rl = &relays[c->relay_for];
			if (!rl->used) {
				conn_close(i);
				continue;
			}
			while (c->relay_sent < rl->data.len &&
			       (n = NET_TCPSend(c->sock, rl->data.data + c->relay_sent, rl->data.len - c->relay_sent)) > 0)
				c->relay_sent += (size_t)n;
			if (NET_TCPRecv(c->sock, buf, sizeof(buf)) < 0) {
				conn_close(i);
				continue;
			}
			i++;
			continue;
		}
		while ((n = NET_TCPRecv(c->sock, buf, sizeof(buf))) > 0)
			SB_Append(&c->in, buf, (size_t)n);
		if (n < 0 || host_time - c->since > 10 || c->in.len > 8192) {
			conn_close(i);
			continue;
		}
		SB_Append(&c->in, "", 1);
		c->in.len--;
		if (strstr(c->in.data, "\n\n")) {
			if (!handle_request(i)) {
				conn_close(i);
				continue;
			}
		}
		i++;
	}
	/* send what the viewers are owed */
	for (int i = 0; i < MAX_SINKS; i++) {
		sink_t *sk = &sinks[i];
		int n;
		if (!sk->used || sk->kind != SINK_QTV)
			continue;
		while (sk->sent < sk->out.len && (n = NET_TCPSend(sk->sock, sk->out.data + sk->sent, sk->out.len - sk->sent)) > 0)
			sk->sent += (size_t)n;
		if (sk->sent == sk->out.len)
			sk->out.len = sk->sent = 0;
		else if (sk->sent > (1u << 20)) {
			memmove(sk->out.data, sk->out.data + sk->sent, sk->out.len - sk->sent);
			sk->out.len -= sk->sent;
			sk->sent = 0;
		}
		{
			char tmp[256];
			if (NET_TCPRecv(sk->sock, tmp, sizeof(tmp)) < 0 || sk->out.len - sk->sent > QTV_MAX_BACKLOG ||
			    (sk->closing && sk->sent == sk->out.len)) {
				if (!sk->closing)
					Con_Printf("QTV: %s stopped watching\n", sk->name);
				free_sink(sk);
			}
		}
	}
}

/* qtvreverse <host[:port]> [name]: send this game to another server, which passes it on to its viewers */
static void Reverse_f(int argc, const char **argv)
{
	netadr_t to;
	sink_t *s;
	char req[512];
	if (argc < 2) {
		Con_Printf("qtvreverse <host[:port]> [stream name]\n");
		return;
	}
	if (!sv.active) {
		Con_Printf("not running a server\n");
		return;
	}
	if (!NET_StringToAdr(argv[1], &to, QTV_DEFAULT_PORT) || to.type == NA_LOOPBACK) {
		Con_Printf("bad address %s\n", argv[1]);
		return;
	}
	if (!(s = new_sink(SINK_QTV)) || (s->sock = NET_TCPConnect(&to)) < 0) {
		Con_Printf("Couldn't connect to proxy\n");
		if (s)
			free_sink(s);
		return;
	}
	snprintf(req, sizeof(req), "BWQTV 1\nPASSWORD: %s\nREVERSE: %s\n\n", qtv_password->string,
	         argc > 2 ? argv[2] : (Cvar_String("hostname")[0] ? Cvar_String("hostname") : "game"));
	SB_AppendStr(&s->out, req);
	Str_Copy(s->name, NET_AdrToString(&to), sizeof(s->name));
	begin_sink(s);
	Con_Printf("QTV: sending the game to %s\n", s->name);
}

/* --- frame, map changes ----------------------------------------------------- */

/* every player's frags as the "frags" player key, so scoreboards and
 * autotrack can read them on any client (and in demos) */
static void publish_frags(void)
{
	qcdef_t *frags;
	if (host_time < next_frags || !sv.vm || !(frags = QC_FindField(sv.vm, "frags")))
		return;
	next_frags = host_time + 0.5;
	for (int i = 0; i < svs.maxclients && i < 256; i++) {
		client_t *c = &svs.clients[i];
		float f;
		char val[32];
		if (c->state != CS_SPAWNED)
			continue;
		f = QC_EdictFields(sv.vm, c->edict)[frags->ofs].f;
		if (frags_known[i] && f == last_frags[i])
			continue;
		frags_known[i] = true;
		last_frags[i] = f;
		snprintf(val, sizeof(val), "%g", f);
		SV_SetPlayerKey(c, "frags", val, true);
	}
}

void SV_MVDFrame(void)
{
	publish_frags();
	run_qtv();
}

/* a recording to a file ends with its map (sv_demoAutoRecord starts the
 * next); live streams carry on with the players into the new one */
void SV_MVDMapChanged(void)
{
	if (recording)
		stop_recording(false);
	memset(frags_known, 0, sizeof(frags_known));
}

void SV_MVDMapSpawned(void)
{
	if (sv_demoAutoRecord->ivalue && !recording) {
		char name[200];
		easy_name(sv_demoAutoPrefix->string, name, sizeof(name));
		start_recording(name);
	}
}

void SV_MVDShutdown(void)
{
	if (recording)
		stop_recording(false);
	for (int i = 0; i < MAX_SINKS; i++)
		if (sinks[i].used)
			free_sink(&sinks[i]);
}

static void QtvStatus_f(int argc, const char **argv)
{
	int any = 0;
	if (listener >= 0)
		Con_Printf("streaming on TCP port %d%s\n", listen_port, qtv_password->string[0] ? " (password)" : "");
	for (int i = 0; i < MAX_SINKS; i++)
		if (sinks[i].used && sinks[i].kind == SINK_QTV) {
			Con_Printf("  viewer %s, %zu bytes waiting\n", sinks[i].name, sinks[i].out.len - sinks[i].sent);
			any++;
		}
	for (int i = 0; i < MAX_RELAYS; i++)
		if (relays[i].used) {
			Con_Printf("  passing on %s (%zu bytes)\n", relays[i].name, relays[i].data.len);
			any++;
		}
	if (!any && listener < 0)
		Con_Printf("not streaming (set qtv_streamport)\n");
}

void SV_InitMVD(void)
{
	sv_demoDir = Cvar_Get("sv_demoDir", "demos", 0, "Folder server demos are written to");
	sv_demofps = Cvar_Get("sv_demofps", "30", 0, "Snapshots per second in server demos");
	sv_demoAutoRecord = Cvar_Get("sv_demoAutoRecord", "0", 0, "Record a server demo of every map");
	sv_demoAutoPrefix = Cvar_Get("sv_demoAutoPrefix", "auto_", 0, "Name prefix for sv_demoAutoRecord demos");
	sv_demoPrefix = Cvar_Get("sv_demoPrefix", "", 0, "Put before every server demo's name");
	sv_demoSuffix = Cvar_Get("sv_demoSuffix", "", 0, "Put after every server demo's name");
	sv_demotxt = Cvar_Get("sv_demotxt", "1", 0, "Write a .txt of the game's details next to each server demo");
	sv_demoMaxDirSize = Cvar_Get("sv_demoMaxDirSize", "100mb", 0, "Most the demo folder may hold");
	sv_demoMaxSize = Cvar_Get("sv_demoMaxSize", "", 0, "Largest a server demo may grow (empty: no limit)");
	sv_demoMaxDirCount = Cvar_Get("sv_demoMaxDirCount", "500", 0, "Most demos the folder may hold");
	sv_demoClearOld = Cvar_Get("sv_demoClearOld", "0", 0, "Delete the oldest demos to make room");
	sv_demoPings = Cvar_Get("sv_demoPings", "10", 0, "How often pings are written (kept for FTE configs; pings are player keys)");
	qtv_streamport = Cvar_Get("qtv_streamport", "0", 0, "TCP port to stream the game on (0: off)");
	qtv_password = Cvar_Get("qtv_password", "", 0, "Password QTV viewers need");
	qtv_maxstreams = Cvar_Get("qtv_maxstreams", "0", 0, "Most QTV viewers at once (0: no limit)");
	Cmd_AddD("sv_demorecord", Record_f, "sv_demorecord <name>: record every player's view to demos/<name>.mvd");
	Cmd_AddD("mvdrecord", Record_f, "mvdrecord <name>: record every player's view to demos/<name>.mvd");
	Cmd_AddD("sv_demoeasyrecord", EasyRecord_f, "sv_demoeasyrecord [name]: record a server demo named after the game");
	Cmd_AddD("sv_demostop", Stop_f, "Stop the server demo");
	Cmd_AddD("mvdstop", Stop_f, "Stop the server demo");
	Cmd_AddD("sv_democancel", Cancel_f, "Stop the server demo and delete it");
	Cmd_AddD("mvdcancel", Cancel_f, "Stop the server demo and delete it");
	Cmd_AddD("sv_demolist", List_f, "sv_demolist [filter]: list the server demos");
	Cmd_AddD("mvdlist", List_f, "mvdlist [filter]: list the server demos");
	Cmd_AddD("sv_demoremove", Remove_f, "sv_demoremove <name|*token>: delete server demos");
	Cmd_AddD("sv_demonumremove", NumRemove_f, "sv_demonumremove <#>: delete a server demo by its sv_demolist number");
	Cmd_AddD("sv_demoinfo", Info_f, "sv_demoinfo <#|*>: show a server demo's info");
	Cmd_AddD("sv_demoinfoadd", InfoAdd_f, "sv_demoinfoadd <#|*> <text>: add a line to a server demo's info");
	Cmd_AddD("sv_demoinforemove", InfoRemove_f, "sv_demoinforemove <#|*>: delete a server demo's info");
	Cmd_AddD("qtvreverse", Reverse_f, "qtvreverse <host[:port]> [name]: send the game to another server's QTV viewers");
	Cmd_AddD("qtv_status", QtvStatus_f, "Who is watching the QTV stream");
}
