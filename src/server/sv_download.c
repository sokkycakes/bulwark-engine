/* Bulwark engine - files between server and clients: downloads of the
 * map, models and sounds a client lacks, and the screenshots the server
 * asks for with snap (uploads).
 *
 * A client asks with the "download <file>" string command and the file
 * comes back in svc_download messages on the reliable stream, paced by
 * how much of the stream is still in flight. Uploads go the other way in
 * clc_upload messages a window at a time; the server asks for each next
 * window with a "nextul" stufftext and can stop one with "stopul".
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/digest.h"
#include "../common/fs.h"
#include "../net/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DL_CHUNK 1024
#define DL_INFLIGHT 96  /* reliable fragments queued before more file goes in */
#define MAX_UPLOAD (8 << 20)

static cvar_t *sv_allowdownload, *sv_allowdownload_packages, *sv_allowdownload_csprogs, *sv_snapdir;

typedef struct {
	char name[128];
	uint8_t *data;
	size_t size, pos;
	/* uploads */
	char ulname[128];
	uint8_t *ul;
	size_t ulsize, ulgot;
} transfer_t;

static transfer_t *transfers; /* one per client slot */
static int numtransfers;

static transfer_t *tr(client_t *c)
{
	int slot = (int)(c - svs.clients);
	if (slot >= numtransfers) {
		transfers = Mem_Realloc(transfers, (size_t)svs.maxclients * sizeof(*transfers));
		memset(transfers + numtransfers, 0, (size_t)(svs.maxclients - numtransfers) * sizeof(*transfers));
		numtransfers = svs.maxclients;
	}
	return &transfers[slot];
}

static bool is_slot(const client_t *c)
{
	return svs.clients && c >= svs.clients && c < svs.clients + svs.maxclients;
}

void SV_FreeTransfers(client_t *c)
{
	/* the multi-view demo's shadow clients aren't slots */
	if (!is_slot(c) || (int)(c - svs.clients) >= numtransfers)
		return;
	transfer_t *t = tr(c);
	Mem_Free(t->data);
	Mem_Free(t->ul);
	memset(t, 0, sizeof(*t));
}

static void refuse(client_t *c, const char *name, const char *why)
{
	uint8_t buf[512];
	msg_t m;
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, svc_download);
	MSG_WriteByte(&m, DL_REFUSED);
	MSG_WriteString(&m, name);
	MSG_WriteString(&m, why);
	SV_ClientReliable(c, m.data, m.cursize);
}

/* why name can't be downloaded, or NULL */
static const char *forbidden(const char *name)
{
	const char *ext = strrchr(name, '.');
	const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
	if (!sv_allowdownload->ivalue)
		return "downloads are off on this server";
	if (!FS_SafeName(name) || name[0] == '.')
		return "bad file name";
	if (!ext || !Str_ICmp(ext, ".cfg") || !Str_ICmp(ext, ".rc"))
		return "not a downloadable file";
	if (!Str_ICmp(base, "progs.dat") || !Str_ICmp(base, "qwprogs.dat") || !Str_ICmp(base, "server.dat"))
		return "the server's own code isn't downloadable";
	if (!Str_ICmp(base, "csprogs.dat") && !sv_allowdownload_csprogs->ivalue)
		return "csprogs downloads are off";
	if ((!Str_ICmp(ext, ".pk3") || !Str_ICmp(ext, ".pak")) && !sv_allowdownload_packages->ivalue)
		return "package downloads are off";
	return NULL;
}

/* "download <file>" and "stopdownload" from a client; false for other commands */
bool SV_DownloadCommand(client_t *c, int argc, const char **argv)
{
	transfer_t *t;
	if (!strcmp(argv[0], "stopdownload")) {
		t = tr(c);
		Mem_Free(t->data);
		t->data = NULL;
		t->name[0] = 0;
		return true;
	}
	if (strcmp(argv[0], "download"))
		return false;
	if (argc < 2)
		return true;
	const char *name = argv[1], *why = forbidden(name);
	if (why) {
		refuse(c, name, why);
		return true;
	}
	size_t size;
	char *data = FS_LoadFile(name, &size);
	if (!data) {
		refuse(c, name, "the server doesn't have it");
		return true;
	}
	t = tr(c);
	Mem_Free(t->data);
	Str_Copy(t->name, name, sizeof(t->name));
	t->data = (uint8_t *)data;
	t->size = size;
	t->pos = 0;
	uint8_t buf[256];
	msg_t m;
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, svc_download);
	MSG_WriteByte(&m, DL_START);
	MSG_WriteString(&m, name);
	MSG_WriteLong(&m, (int32_t)size);
	SV_ClientReliable(c, m.data, m.cursize);
	Con_DPrintf("%s is downloading %s (%zu bytes)\n", c->name, name, size);
	return true;
}

/* more of the file, while the reliable stream has room */
void SV_DownloadFrame(client_t *c)
{
	if (!is_slot(c) || (int)(c - svs.clients) >= numtransfers)
		return;
	transfer_t *t = tr(c);
	if (!t->data)
		return;
	uint8_t buf[DL_CHUNK + 16];
	msg_t m;
	while (t->pos < t->size && c->chan.relnext - c->chan.relacked < DL_INFLIGHT) {
		size_t n = BW_MIN(t->size - t->pos, (size_t)DL_CHUNK);
		MSG_Init(&m, buf, sizeof(buf));
		MSG_WriteByte(&m, svc_download);
		MSG_WriteByte(&m, DL_DATA);
		MSG_WriteLong(&m, (int32_t)t->pos);
		MSG_WriteShort(&m, (int)n);
		MSG_WriteData(&m, t->data + t->pos, n);
		SV_ClientReliable(c, m.data, m.cursize);
		t->pos += n;
	}
	if (t->pos >= t->size) {
		char hex[80];
		Digest_Hex("sha256", t->data, t->size, hex, sizeof(hex));
		MSG_Init(&m, buf, sizeof(buf));
		MSG_WriteByte(&m, svc_download);
		MSG_WriteByte(&m, DL_DONE);
		MSG_WriteString(&m, hex);
		SV_ClientReliable(c, m.data, m.cursize);
		Mem_Free(t->data);
		t->data = NULL;
		t->name[0] = 0;
	}
}

/* --- uploads (snap) ------------------------------------------------------------------ */

static void save_upload(client_t *c, transfer_t *t)
{
	char rel[256], path[1024], when[32], who[64];
	time_t now = time(NULL);
	size_t n = 0;
	strftime(when, sizeof(when), "%Y%m%d-%H%M%S", localtime(&now));
	for (const char *p = c->name; *p && n + 1 < sizeof(who); p++)
		who[n++] = (char)((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ? *p : '_');
	who[n] = 0;
	snprintf(rel, sizeof(rel), "%s/%s-%s.jpg", sv_snapdir->string, who[0] ? who : "player", when);
	for (int i = 2; FS_FileExists(rel) && i < 100; i++)
		snprintf(rel, sizeof(rel), "%s/%s-%s-%d.jpg", sv_snapdir->string, who[0] ? who : "player", when, i);
	FILE *f = NULL;
	if (FS_WritePath(rel, path, sizeof(path)) && FS_CreatePath(path))
		f = fopen(path, "wb");
	if (!f || fwrite(t->ul, 1, t->ulgot, f) != t->ulgot)
		Con_Printf("snap: can't write %s\n", rel);
	else
		Con_Printf("snap: %s's screen is in %s\n", c->name, rel);
	if (f)
		fclose(f);
}

/* a clc_upload message */
void SV_ParseUpload(client_t *c, msg_t *m)
{
	transfer_t *t = tr(c);
	int op = MSG_ReadByte(m);
	switch (op) {
	case UL_START: {
		const char *name = MSG_ReadString(m);
		int32_t size = MSG_ReadLong(m);
		Mem_Free(t->ul);
		t->ul = NULL;
		if (!t->ulname[0] || size <= 0 || size > MAX_UPLOAD) {
			/* nobody asked, or too big */
			SV_StuffText(c, "stopul\n");
			return;
		}
		Con_DPrintf("Server receiving %s from %d...\n", name, (int)(c - svs.clients) + 1);
		t->ul = Mem_Alloc((size_t)size);
		t->ulsize = (size_t)size;
		t->ulgot = 0;
		SV_StuffText(c, "nextul\n");
		break;
	}
	case UL_DATA: {
		int32_t ofs = MSG_ReadLong(m);
		int len = MSG_ReadShort(m) & 0xffff;
		if (m->readpos + (size_t)len > m->cursize) {
			m->badread = true;
			return;
		}
		if (t->ul && ofs >= 0 && (size_t)ofs + (size_t)len <= t->ulsize) {
			memcpy(t->ul + ofs, m->data + m->readpos, (size_t)len);
			if ((size_t)ofs + (size_t)len > t->ulgot)
				t->ulgot = (size_t)ofs + (size_t)len;
		}
		m->readpos += (size_t)len;
		break;
	}
	case UL_WINDOW: /* the client sent a window and waits */
		if (t->ul)
			SV_StuffText(c, "nextul\n");
		break;
	case UL_DONE:
		if (t->ul && t->ulgot == t->ulsize)
			save_upload(c, t);
		Mem_Free(t->ul);
		t->ul = NULL;
		t->ulname[0] = 0;
		break;
	default:
		m->badread = true;
	}
}

static void request_snap(client_t *c)
{
	if (c->bot || c->state != CS_SPAWNED) {
		Con_Printf("snap: %s can't send a screenshot\n", c->name);
		return;
	}
	Str_Copy(tr(c)->ulname, "snap", sizeof(tr(c)->ulname));
	SV_StuffText(c, "__snap\n");
	Con_Printf("asked %s for a screenshot\n", c->name);
}

/* snap <userid>: asks a player for a screenshot, saved in sv_snapdir */
static void Snap_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Usage:  snap <userid>\n");
		return;
	}
	client_t *c = sv.active ? SV_FindClient(argv[1]) : NULL;
	if (c)
		request_snap(c);
}

/* snapall: every player */
static void Snapall_f(int argc, const char **argv)
{
	if (!sv.active)
		return;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED && !svs.clients[i].bot)
			request_snap(&svs.clients[i]);
}

void SV_InitDownloads(void)
{
	sv_allowdownload = Cvar_Get("sv_allowdownload", "1", 0, "Let clients download the files they're missing");
	sv_allowdownload_packages = Cvar_Get("sv_allowdownload_packages", "1", 0, "Let clients download .pk3 and .pak files");
	sv_allowdownload_csprogs = Cvar_Get("sv_allowdownload_csprogs", "1", 0, "Let clients download csprogs.dat");
	sv_snapdir = Cvar_Get("sv_snapdir", "snap", 0, "Where snap saves players' screenshots");
	Cmd_AddD("snap", Snap_f, "snap <userid>: get a player's screenshot");
	Cmd_AddD("snapall", Snapall_f, "Get every player's screenshot");
}
