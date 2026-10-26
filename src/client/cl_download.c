/* Bulwark engine - client downloads: files the server has and we don't
 * (asked for while loading a map, or with "download"), screenshots the
 * server asks for (snap uploads), and "curl" for files from the web.
 * See sv_download.c for the server's half. ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/digest.h"
#include "../common/fs.h"
#include "../net/http.h"
#include "../net/netchan.h"
#include "../net/protocol.h"
#include "../render/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UL_CHUNK 1024
#define UL_WINDOW_CHUNKS 16

static cvar_t *cl_downloads;

static struct {
	char queue[64][128];
	int numqueued;
	char name[128]; /* "" when none is running */
	char tmp[1100];
	FILE *f;
	size_t size, got;
	bool for_signon; /* the map waits for these */
	int done, failed;
} dl;

static struct {
	uint8_t *data;
	size_t size, sent;
} ul;

/* --- downloads from the server ----------------------------------------------------- */

static void send_cmd(const char *s) { CL_SendStringCmd(s); }

static void close_current(bool keep)
{
	if (dl.f)
		fclose(dl.f);
	dl.f = NULL;
	if (!keep && dl.tmp[0])
		unlink(dl.tmp);
	dl.name[0] = dl.tmp[0] = 0;
	dl.size = dl.got = 0;
}

static void finish_all(void)
{
	bool signon = dl.for_signon;
	dl.for_signon = false;
	if (dl.done)
		FS_Restart();
	if (signon) {
		if (dl.done || dl.failed)
			Con_Printf("%d file%s downloaded%s\n", dl.done, dl.done == 1 ? "" : "s",
			           dl.failed ? va(", %d failed", dl.failed) : "");
		CL_FinishSignon();
	}
	dl.done = dl.failed = 0;
}

static void start_next(void)
{
	while (!dl.name[0] && dl.numqueued) {
		Str_Copy(dl.name, dl.queue[0], sizeof(dl.name));
		memmove(dl.queue, dl.queue + 1, (size_t)(--dl.numqueued) * sizeof(dl.queue[0]));
		if (FS_FileExists(dl.name)) {
			dl.name[0] = 0;
			continue;
		}
		send_cmd(va("download \"%s\"", dl.name));
		return;
	}
	if (!dl.name[0])
		finish_all();
}

/* files a server must not plant: configs and the menu run with the player's
 * full rights at the next start, libraries and programs run as code, and a
 * package in the game directory would carry any of those */
static bool dangerous_name(const char *name)
{
	static const char *const exts[] = {".cfg", ".rc", ".fmf", ".so", ".dll", ".dylib", ".exe", ".com", ".bat",
	                                   ".cmd", ".sh", ".scr", ".pk3", ".pak", ".pk4", ".zip", ".bwp"};
	const char *base = strrchr(name, '/'), *ext = strrchr(name, '.');
	base = base ? base + 1 : name;
	if (!Str_ICmp(base, "menu.dat") || !Str_ICmp(base, "progs.dat") || !Str_ICmp(base, "qwprogs.dat"))
		return true;
	if (ext)
		for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++)
			if (!Str_ICmp(ext, exts[i]))
				return true;
	return false;
}

static void queue(const char *name)
{
	if (!FS_SafeName(name) || !strcmp(name, dl.name))
		return;
	if (dangerous_name(name)) {
		Con_Printf("^3won't download %s: that kind of file could run as code\n", name);
		return;
	}
	for (int i = 0; i < dl.numqueued; i++)
		if (!strcmp(dl.queue[i], name))
			return;
	if (dl.numqueued < (int)BW_ARRAY_COUNT(dl.queue))
		Str_Copy(dl.queue[dl.numqueued++], name, sizeof(dl.queue[0]));
}

/* before the map loads: what it needs that we haven't got. True when the
 * signon waits for downloads; CL_FinishSignon runs once they're done. */
bool CL_StartMissingDownloads(void)
{
	if (!cl_downloads->ivalue || cl.demoplayback || cl.server.type == NA_LOOPBACK)
		return false;
	if (!FS_FileExists(cl.worldpath))
		queue(cl.worldpath);
	if (cl.csprogsname[0] && !FS_FileExists(cl.csprogsname))
		queue(cl.csprogsname);
	for (int i = 1; i < cl.nummodels; i++) {
		const char *m = cl.models[i].name;
		if (m && m[0] && m[0] != '*' && !FS_FileExists(m))
			queue(m);
	}
	for (int i = 1; i < cl.numsounds; i++) {
		const char *s = cl.sounds[i];
		if (s && s[0] && !FS_FileExists(s) && !FS_FileExists(va("sound/%s", s)))
			queue(va("sound/%s", s));
	}
	if (!dl.numqueued && !dl.name[0])
		return false;
	Con_Printf("downloading %d file%s the map needs\n", dl.numqueued, dl.numqueued == 1 ? "" : "s");
	dl.for_signon = true;
	start_next();
	return dl.for_signon;
}

void CL_ParseDownload(msg_t *m)
{
	int op = MSG_ReadByte(m);
	switch (op) {
	case DL_START: {
		char name[128], path[1024];
		Str_Copy(name, MSG_ReadString(m), sizeof(name));
		int32_t size = MSG_ReadLong(m);
		if (strcmp(name, dl.name) || size < 0) {
			send_cmd("stopdownload"); /* not something we asked for */
			return;
		}
		if (!FS_WritePath(name, path, sizeof(path)) || !FS_CreatePath(path)) {
			send_cmd("stopdownload");
			close_current(false);
			dl.failed++;
			start_next();
			return;
		}
		snprintf(dl.tmp, sizeof(dl.tmp), "%s.tmp", path);
		dl.f = fopen(dl.tmp, "wb");
		dl.size = (size_t)size;
		dl.got = 0;
		Con_Printf("downloading %s (%d KiB)\n", name, (int)((size + 1023) / 1024));
		break;
	}
	case DL_DATA: {
		int32_t ofs = MSG_ReadLong(m);
		int len = MSG_ReadShort(m) & 0xffff;
		if (m->readpos + (size_t)len > m->cursize) {
			m->badread = true;
			return;
		}
		if (dl.f && ofs >= 0) {
			fseek(dl.f, ofs, SEEK_SET);
			fwrite(m->data + m->readpos, 1, (size_t)len, dl.f);
			if ((size_t)ofs + (size_t)len > dl.got)
				dl.got = (size_t)ofs + (size_t)len;
		}
		m->readpos += (size_t)len;
		break;
	}
	case DL_DONE: {
		char want[80];
		Str_Copy(want, MSG_ReadString(m), sizeof(want));
		if (!dl.f)
			return;
		fclose(dl.f);
		dl.f = NULL;
		/* check what arrived */
		FILE *f = fopen(dl.tmp, "rb");
		size_t n = 0;
		char *data = NULL, hex[80] = "";
		if (f) {
			data = Mem_Alloc(dl.got + 1);
			n = fread(data, 1, dl.got, f);
			fclose(f);
			Digest_Hex("sha256", data, n, hex, sizeof(hex));
			Mem_Free(data);
		}
		char final[sizeof(dl.tmp)];
		Str_Copy(final, dl.tmp, sizeof(final));
		final[strlen(final) - 4] = 0;
		if (n != dl.size || Str_ICmp(hex, want) || rename(dl.tmp, final)) {
			Con_Printf("^1download of %s failed: it arrived damaged\n", dl.name);
			close_current(false);
			dl.failed++;
		} else {
			Con_Printf("downloaded %s\n", dl.name);
			dl.tmp[0] = 0;
			close_current(true);
			dl.done++;
		}
		start_next();
		break;
	}
	case DL_REFUSED: {
		char name[128];
		Str_Copy(name, MSG_ReadString(m), sizeof(name));
		const char *why = MSG_ReadString(m);
		Con_Printf("^3can't download %s: %s\n", name, why);
		if (!strcmp(name, dl.name)) {
			close_current(false);
			dl.failed++;
			start_next();
		}
		break;
	}
	default:
		m->badread = true;
	}
}

void CL_DownloadsReset(void)
{
	close_current(false);
	dl.numqueued = 0;
	dl.for_signon = false;
	dl.done = dl.failed = 0;
	Mem_Free(ul.data);
	memset(&ul, 0, sizeof(ul));
}

/* download <file>: fetches a file from the server */
static void Download_f(int argc, const char **argv)
{
	if (cl.state < ca_loading || cl.demoplayback) {
		Con_Printf("Must be connected.\n");
		return;
	}
	if (argc < 2) {
		Con_Printf("download <file>\n");
		return;
	}
	if (FS_FileExists(argv[1]) && !(argc > 2 && !strcmp(argv[2], "force")))
		Con_Printf("%s is already here (download %s force to get it anyway)\n", argv[1], argv[1]);
	else {
		queue(argv[1]);
		if (!dl.name[0])
			start_next();
	}
}

/* dlsize: how far the current download is */
static void Dlsize_f(int argc, const char **argv)
{
	if (!dl.name[0]) {
		Con_Printf("no download running\n");
		return;
	}
	Con_Printf("%s: %zu of %zu bytes (%d%%), %d more queued\n", dl.name, dl.got, dl.size,
	           dl.size ? (int)(dl.got * 100 / dl.size) : 0, dl.numqueued);
}

/* skipdl: gives up on the current download and goes on to the next */
static void Skipdl_f(int argc, const char **argv)
{
	if (!dl.name[0]) {
		Con_Printf("no download running\n");
		return;
	}
	Con_Printf("skipped %s\n", dl.name);
	send_cmd("stopdownload");
	close_current(false);
	dl.failed++;
	start_next();
}

/* finishdl: stops downloading and carries on with what's here */
static void Finishdl_f(int argc, const char **argv)
{
	if (dl.name[0])
		send_cmd("stopdownload");
	close_current(false);
	dl.numqueued = 0;
	finish_all();
}

/* --- screenshot uploads --------------------------------------------------------- */

static void send_upload(const msg_t *m)
{
	if (cl.state >= ca_loading && !cl.demoplayback)
		Netchan_WriteReliable(&cl.chan, m->data, m->cursize);
}

static void captured(const void *jpeg, size_t len)
{
	uint8_t buf[256];
	msg_t m;
	Mem_Free(ul.data);
	ul.data = Mem_Alloc(len ? len : 1);
	memcpy(ul.data, jpeg, len);
	ul.size = len;
	ul.sent = 0;
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, clc_upload);
	MSG_WriteByte(&m, UL_START);
	MSG_WriteString(&m, "snap.jpg");
	MSG_WriteLong(&m, (int32_t)len);
	send_upload(&m);
}

/* __snap: the server wants our screen */
static void Snap_f(int argc, const char **argv)
{
	if (!R_CaptureScreen(captured)) {
		/* nothing drawn: an empty upload, which the server turns down */
		captured(NULL, 0);
	}
}

/* nextul: the server is ready for the next window of the upload */
static void Nextul_f(int argc, const char **argv)
{
	uint8_t buf[UL_CHUNK + 32];
	msg_t m;
	if (!ul.data)
		return;
	for (int i = 0; i < UL_WINDOW_CHUNKS && ul.sent < ul.size; i++) {
		size_t n = BW_MIN(ul.size - ul.sent, (size_t)UL_CHUNK);
		MSG_Init(&m, buf, sizeof(buf));
		MSG_WriteByte(&m, clc_upload);
		MSG_WriteByte(&m, UL_DATA);
		MSG_WriteLong(&m, (int32_t)ul.sent);
		MSG_WriteShort(&m, (int)n);
		MSG_WriteData(&m, ul.data + ul.sent, n);
		send_upload(&m);
		ul.sent += n;
	}
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, clc_upload);
	MSG_WriteByte(&m, ul.sent < ul.size ? UL_WINDOW : UL_DONE);
	send_upload(&m);
	if (ul.sent >= ul.size) {
		Mem_Free(ul.data);
		ul.data = NULL;
	}
}

/* stopul: the server doesn't want the upload */
static void Stopul_f(int argc, const char **argv)
{
	Mem_Free(ul.data);
	ul.data = NULL;
}

/* --- curl ------------------------------------------------------------------------- */

typedef struct curljob_s {
	http_t *h;
	char url[512], dest[1024], name[256];
	bool pak;
	struct curljob_s *next;
} curljob_t;

static curljob_t *curljobs;

/* curl [--info] [--cancel [url]] [--pak] [--as <file>] [--for <file>...] [--forthismap] <url>
 * the DarkPlaces command games use to fetch files from the web */
static void Curl_f(int argc, const char **argv)
{
	const char *url = NULL, *as = NULL;
	bool pak = false, needed = true, forany = false;
	if (argc < 2) {
		Con_Printf("curl: No args\n");
		return;
	}
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--info")) {
			int n = 0;
			for (curljob_t *j = curljobs; j; j = j->next, n++) {
				size_t total, got = HTTP_Progress(j->h, &total);
				Con_Printf("%s: %zu/%zu bytes -> %s\n", j->url, got, total, j->name);
			}
			Con_Printf("%d download%s running\n", n, n == 1 ? "" : "s");
			return;
		}
		if (!strcmp(a, "--cancel")) {
			for (curljob_t *j = curljobs; j; j = j->next)
				if (i + 1 >= argc || !strcmp(j->url, argv[i + 1]))
					HTTP_Cancel(j->h);
			return;
		}
		if (!strcmp(a, "--pak"))
			pak = true;
		else if (!strcmp(a, "--as") && i + 1 < argc)
			as = argv[++i];
		else if (!strcmp(a, "--for") && i + 1 < argc) {
			/* only wanted while one of these is missing */
			if (!forany)
				needed = false;
			forany = true;
			while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) && i + 2 < argc)
				needed |= !FS_FileExists(argv[++i]);
		} else if (!strcmp(a, "--forthismap")) {
			/* fine either way: the file is wanted now */
		} else if (!strncmp(a, "--", 2)) {
			Con_Printf("curl: unknown option %s\n", a);
		} else
			url = a;
	}
	if (!url) {
		Con_Printf("curl: no url\n");
		return;
	}
	if (!needed)
		return;
	const char *base = strrchr(url, '/') ? strrchr(url, '/') + 1 : url;
	char name[256];
	char *q;
	Str_Copy(name, as ? as : base, sizeof(name));
	if ((q = strchr(name, '?')))
		*q = 0;
	if (!as && !pak) {
		char tmp[300];
		snprintf(tmp, sizeof(tmp), "dlcache/%s", name);
		Str_Copy(name, tmp, sizeof(name));
	}
	curljob_t *j = Mem_Alloc(sizeof(*j));
	if (!name[0] || !FS_WritePath(name, j->dest, sizeof(j->dest)) || !FS_CreatePath(j->dest)) {
		Con_Printf("curl: can't save as %s\n", name);
		Mem_Free(j);
		return;
	}
	Str_Copy(j->url, url, sizeof(j->url));
	Str_Copy(j->name, name, sizeof(j->name));
	j->pak = pak;
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", j->dest);
	j->h = HTTP_Get(url, tmp);
	if (!j->h) {
		Con_Printf("curl: can't fetch %s\n", url);
		Mem_Free(j);
		return;
	}
	j->next = curljobs;
	curljobs = j;
	Con_Printf("curl: downloading %s\n", url);
}

void CL_DownloadFrame(void)
{
	for (curljob_t **pj = &curljobs; *pj;) {
		curljob_t *j = *pj;
		int st = HTTP_State(j->h);
		if (st == HTTP_RUNNING) {
			pj = &j->next;
			continue;
		}
		char tmp[1100];
		snprintf(tmp, sizeof(tmp), "%s.tmp", j->dest);
		if (st == HTTP_DONE && HTTP_Code(j->h) / 100 == 2 && !rename(tmp, j->dest)) {
			Con_Printf("curl: downloaded %s\n", j->name);
			if (j->pak)
				FS_Restart();
		} else {
			unlink(tmp);
			Con_Printf("^3curl: %s failed: %s\n", j->url,
			           st == HTTP_DONE ? va("HTTP %d", HTTP_Code(j->h)) : HTTP_Error(j->h));
		}
		HTTP_Free(j->h);
		*pj = j->next;
		Mem_Free(j);
	}
}

void CL_InitDownloads(void)
{
	cl_downloads = Cvar_Get("cl_downloads", "1", CVAR_ARCHIVE, "Download the files a server's map needs that are missing");
	Cmd_AddD("download", Download_f, "download <file>: get a file from the server");
	Cmd_AddD("dlsize", Dlsize_f, "Show the current download's size and progress");
	Cmd_AddD("skipdl", Skipdl_f, "Give up on the current download");
	Cmd_AddD("finishdl", Finishdl_f, "Stop downloading and carry on with the files that are here");
	Cmd_AddD("nextul", Nextul_f, "For internal use");
	Cmd_AddD("stopul", Stopul_f, "For internal use");
	Cmd_AddD("__snap", Snap_f, "For internal use: send the server a screenshot");
	Cmd_AddD("curl", Curl_f, "For use by xonotic: curl [--pak] [--as <file>] [--for <file>] <url>, --info, --cancel");
}
