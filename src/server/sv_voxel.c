/* Bulwark engine - the server's voxel world: QC builtins, file save/load
 * and replication to clients (svc_voxel). The server is the only writer;
 * every change goes to every client on the reliable stream, and a client
 * that connects later gets the whole world at signon. See
 * docs/spec/voxel.md. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/fs.h"
#include "../common/voxel.h"

#include <errno.h>
#include <stdio.h>

static char vx_error[512];
static char *vx_meta;
static int vx_unknown;

/* --- replication ---------------------------------------------------------- */

static void write_init(msg_t *m, const vxworld_t *w)
{
	MSG_WriteByte(m, svc_voxel);
	MSG_WriteByte(m, VXN_INIT);
	for (int i = 0; i < 3; i++)
		MSG_WriteVarUInt(m, (uint32_t)w->size[i]);
	MSG_WriteFloat(m, w->blocksize);
	for (int i = 0; i < 3; i++)
		MSG_WriteFloat(m, w->origin[i]);
}

static void write_def(msg_t *m, const vxworld_t *w, int type)
{
	const vxdef_t *d = &w->defs[type];
	MSG_WriteByte(m, svc_voxel);
	MSG_WriteByte(m, VXN_DEF);
	MSG_WriteVarUInt(m, (uint32_t)type);
	MSG_WriteByte(m, d->shape);
	MSG_WriteLong(m, (int32_t)d->contents);
	for (int i = 0; i < 4; i++)
		MSG_WriteByte(m, d->color[i]);
	MSG_WriteByte(m, d->flags);
	MSG_WriteString(m, d->name);
	MSG_WriteString(m, d->texture);
}

/* returns false for an empty chunk (nothing written) */
static bool write_chunk(msg_t *m, const vxworld_t *w, int cx, int cy, int cz, bool evenempty)
{
	static uint8_t buf[VX_CHUNKCELLS * 8];
	size_t n;
	if (!evenempty && !w->chunks[(cz * w->nchunks[1] + cy) * w->nchunks[0] + cx])
		return false;
	n = Vox_EncodeChunk(w, cx, cy, cz, buf, sizeof(buf));
	if (!n)
		return false;
	MSG_WriteByte(m, svc_voxel);
	MSG_WriteByte(m, VXN_CHUNK);
	MSG_WriteVarUInt(m, (uint32_t)cx);
	MSG_WriteVarUInt(m, (uint32_t)cy);
	MSG_WriteVarUInt(m, (uint32_t)cz);
	MSG_WriteVarUInt(m, (uint32_t)n);
	MSG_WriteData(m, buf, n);
	return true;
}

/* sends the whole world through 'send' (one client or everyone) */
static void send_world(client_t *c)
{
	const vxworld_t *w = sv.voxel;
	uint8_t *big = Mem_Alloc(VX_CHUNKCELLS * 8 + 64);
	msg_t m;
	MSG_Init(&m, big, VX_CHUNKCELLS * 8 + 64);
	if (!w) {
		MSG_WriteByte(&m, svc_voxel);
		MSG_WriteByte(&m, VXN_FREE);
		c ? SV_ClientReliable(c, &m) : SV_BroadcastReliable(&m);
		Mem_Free(big);
		return;
	}
	write_init(&m, w);
	c ? SV_ClientReliable(c, &m) : SV_BroadcastReliable(&m);
	for (int t = 1; t < VX_MAX_TYPES; t++) {
		if (!w->defs[t].defined)
			continue;
		MSG_Clear(&m);
		write_def(&m, w, t);
		c ? SV_ClientReliable(c, &m) : SV_BroadcastReliable(&m);
	}
	for (int cz = 0; cz < w->nchunks[2]; cz++)
		for (int cy = 0; cy < w->nchunks[1]; cy++)
			for (int cx = 0; cx < w->nchunks[0]; cx++) {
				MSG_Clear(&m);
				if (write_chunk(&m, w, cx, cy, cz, false))
					c ? SV_ClientReliable(c, &m) : SV_BroadcastReliable(&m);
			}
	Mem_Free(big);
}

void SV_VoxelSignon(client_t *c)
{
	if (sv.voxel)
		send_world(c);
}

void SV_VoxelShutdown(void)
{
	Vox_Free(sv.voxel);
	sv.voxel = NULL;
	Mem_Free(vx_meta);
	vx_meta = NULL;
}

/* --- builtins ---------------------------------------------------------- */

static bool cell_arg(qcvm_t *vm, int n, int c[3])
{
	const float *v = QC_PVEC(vm, n);
	for (int i = 0; i < 3; i++)
		c[i] = (int)floorf(v[i] + 0.5f);
	return sv.voxel && Vox_InBounds(sv.voxel, c[0], c[1], c[2]);
}

/* float voxel_create(vector cells, float blocksize, vector origin) */
static void bi_voxel_create(qcvm_t *vm)
{
	const float *s = QC_PVEC(vm, 0), *o = QC_PVEC(vm, 2);
	int size[3] = {(int)s[0], (int)s[1], (int)s[2]};
	vxworld_t *w = Vox_Create(size, QC_PFLOAT(vm, 1), o);
	if (!w) {
		Con_Printf("voxel_create: size %d %d %d with %g-unit blocks is out of range\n", size[0], size[1], size[2],
		           QC_PFLOAT(vm, 1));
		QC_ReturnFloat(vm, 0);
		return;
	}
	if (sv.voxel)
		memcpy(w->defs, sv.voxel->defs, sizeof(w->defs));
	Vox_Free(sv.voxel);
	sv.voxel = w;
	send_world(NULL);
	QC_ReturnFloat(vm, 1);
}

static void bi_voxel_free(qcvm_t *vm)
{
	SV_VoxelShutdown();
	send_world(NULL);
}

/* float voxel_define(float type, string name, float shape, float contents,
 *                    vector rgb, float alpha, string texture, float flags) */
static void bi_voxel_define(qcvm_t *vm)
{
	vxdef_t d = {0};
	int type = (int)QC_PFLOAT(vm, 0);
	const float *rgb = QC_PVEC(vm, 4);
	uint8_t buf[512];
	msg_t m;
	if (!sv.voxel) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	Str_Copy(d.name, QC_PSTR(vm, 1), sizeof(d.name));
	d.shape = (uint8_t)QC_PFLOAT(vm, 2);
	d.contents = (uint32_t)QC_PINT(vm, 3);
	if (d.contents == 0 && QC_PFLOAT(vm, 3) != 0)
		d.contents = (uint32_t)QC_PFLOAT(vm, 3); /* passed as a float */
	for (int i = 0; i < 3; i++)
		d.color[i] = (uint8_t)BW_MAX(0, BW_MIN(255, (int)(rgb[i] * 255 + 0.5f)));
	d.color[3] = (uint8_t)BW_MAX(0, BW_MIN(255, (int)(QC_PFLOAT(vm, 5) * 255 + 0.5f)));
	Str_Copy(d.texture, vm->argc > 6 ? QC_PSTR(vm, 6) : "", sizeof(d.texture));
	d.flags = vm->argc > 7 ? (uint8_t)QC_PFLOAT(vm, 7) : 0;
	if (!Vox_Define(sv.voxel, type, &d)) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	MSG_Init(&m, buf, sizeof(buf));
	write_def(&m, sv.voxel, type);
	SV_BroadcastReliable(&m);
	QC_ReturnFloat(vm, 1);
}

/* float voxel_set(vector cell, float type, float rot) */
static void bi_voxel_set(qcvm_t *vm)
{
	int c[3], type = (int)QC_PFLOAT(vm, 1), rot = (int)QC_PFLOAT(vm, 2) & 3, oldrot, old;
	uint8_t buf[64];
	msg_t m;
	if (!cell_arg(vm, 0, c)) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	old = Vox_Get(sv.voxel, c[0], c[1], c[2], &oldrot);
	if (!Vox_Set(sv.voxel, c[0], c[1], c[2], type, rot)) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	if (old != type || (type && oldrot != rot)) {
		MSG_Init(&m, buf, sizeof(buf));
		MSG_WriteByte(&m, svc_voxel);
		MSG_WriteByte(&m, VXN_SET);
		for (int i = 0; i < 3; i++)
			MSG_WriteVarUInt(&m, (uint32_t)c[i]);
		MSG_WriteShort(&m, type);
		MSG_WriteByte(&m, type ? rot : 0);
		SV_BroadcastReliable(&m);
	}
	QC_ReturnFloat(vm, 1);
}

/* float voxel_fill(vector mincell, vector maxcell, float type, float rot):
 * sets a box of cells and returns how many changed; sent as whole chunks */
static void bi_voxel_fill(qcvm_t *vm)
{
	const float *a = QC_PVEC(vm, 0), *b = QC_PVEC(vm, 1);
	int lo[3], hi[3], type = (int)QC_PFLOAT(vm, 2), rot = (int)QC_PFLOAT(vm, 3) & 3, changed = 0;
	vxworld_t *w = sv.voxel;
	if (!w || type < 0 || type >= VX_MAX_TYPES || !w->defs[type].defined) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	for (int i = 0; i < 3; i++) {
		lo[i] = BW_MAX(0, (int)floorf(BW_MIN(a[i], b[i]) + 0.5f));
		hi[i] = BW_MIN(w->size[i] - 1, (int)floorf(BW_MAX(a[i], b[i]) + 0.5f));
		if (lo[i] > hi[i]) {
			QC_ReturnFloat(vm, 0);
			return;
		}
	}
	for (int z = lo[2]; z <= hi[2]; z++)
		for (int y = lo[1]; y <= hi[1]; y++)
			for (int x = lo[0]; x <= hi[0]; x++) {
				int r, t = Vox_Get(w, x, y, z, &r);
				if (t == type && (!t || r == rot))
					continue;
				Vox_Set(w, x, y, z, type, rot);
				changed++;
			}
	if (changed) {
		uint8_t *big = Mem_Alloc(VX_CHUNKCELLS * 8 + 64);
		msg_t m;
		MSG_Init(&m, big, VX_CHUNKCELLS * 8 + 64);
		for (int cz = lo[2] / VX_CHUNK; cz <= hi[2] / VX_CHUNK; cz++)
			for (int cy = lo[1] / VX_CHUNK; cy <= hi[1] / VX_CHUNK; cy++)
				for (int cx = lo[0] / VX_CHUNK; cx <= hi[0] / VX_CHUNK; cx++) {
					MSG_Clear(&m);
					if (write_chunk(&m, w, cx, cy, cz, true))
						SV_BroadcastReliable(&m);
				}
		Mem_Free(big);
	}
	QC_ReturnFloat(vm, (float)changed);
}

static void bi_voxel_get(qcvm_t *vm)
{
	int c[3];
	QC_ReturnFloat(vm, cell_arg(vm, 0, c) ? (float)Vox_Get(sv.voxel, c[0], c[1], c[2], NULL) : 0);
}

static void bi_voxel_getrot(qcvm_t *vm)
{
	int c[3], rot = 0;
	if (cell_arg(vm, 0, c))
		Vox_Get(sv.voxel, c[0], c[1], c[2], &rot);
	QC_ReturnFloat(vm, (float)rot);
}

/* --- shared read-only builtins (also used by the client, see cl_csqc.c) */

vxworld_t *vx_clientworld; /* the client's mirror, set by cl_main.c */

static vxworld_t *cur_voxel(qcvm_t *vm) { return vm->kind == QCVM_SERVER ? sv.voxel : vx_clientworld; }

void VX_bi_voxel_cellforpoint(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	int c[3] = {0, 0, 0};
	float out[3];
	if (w)
		Vox_CellForPoint(w, QC_PVEC(vm, 0), c);
	out[0] = (float)c[0], out[1] = (float)c[1], out[2] = (float)c[2];
	QC_ReturnVector(vm, out);
}

void VX_bi_voxel_cellorigin(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	const float *v = QC_PVEC(vm, 0);
	float out[3] = {0, 0, 0};
	if (w)
		Vox_CellMins(w, (int)floorf(v[0] + 0.5f), (int)floorf(v[1] + 0.5f), (int)floorf(v[2] + 0.5f), out);
	QC_ReturnVector(vm, out);
}

void VX_bi_voxel_size(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	float out[3] = {0, 0, 0};
	if (w)
		out[0] = (float)w->size[0], out[1] = (float)w->size[1], out[2] = (float)w->size[2];
	QC_ReturnVector(vm, out);
}

void VX_bi_voxel_blocksize(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	QC_ReturnFloat(vm, w ? w->blocksize : 0);
}

void VX_bi_voxel_count(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	QC_ReturnFloat(vm, w ? (float)w->count : 0);
}

void VX_bi_voxel_typeforname(qcvm_t *vm) { QC_ReturnFloat(vm, (float)Vox_TypeForName(cur_voxel(vm), QC_PSTR(vm, 0))); }

void VX_bi_voxel_typename(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	int t = (int)QC_PFLOAT(vm, 0);
	QC_ReturnString(vm, w && t >= 0 && t < VX_MAX_TYPES && w->defs[t].defined ? w->defs[t].name : "");
}

void VX_bi_voxel_tracecell(qcvm_t *vm)
{
	const int *c = SV_LastVoxelCell();
	float out[3] = {-1, -1, -1};
	if (c[3] >= 0)
		out[0] = (float)c[0], out[1] = (float)c[1], out[2] = (float)c[2];
	QC_ReturnVector(vm, out);
}

void VX_bi_voxel_tracetype(qcvm_t *vm) { QC_ReturnFloat(vm, (float)SV_LastVoxelCell()[3]); }

void VX_bi_voxel_client_get(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	const float *v = QC_PVEC(vm, 0);
	QC_ReturnFloat(vm, w ? (float)Vox_Get(w, (int)floorf(v[0] + 0.5f), (int)floorf(v[1] + 0.5f), (int)floorf(v[2] + 0.5f), NULL) : 0);
}

void VX_bi_voxel_client_getrot(qcvm_t *vm)
{
	vxworld_t *w = cur_voxel(vm);
	const float *v = QC_PVEC(vm, 0);
	int rot = 0;
	if (w)
		Vox_Get(w, (int)floorf(v[0] + 0.5f), (int)floorf(v[1] + 0.5f), (int)floorf(v[2] + 0.5f), &rot);
	QC_ReturnFloat(vm, (float)rot);
}

/* --- files ---------------------------------------------------------------- */

/* game files live under data/, the same place QC fopen uses */
static bool data_path(const char *name, char *out, size_t outsize)
{
	char rel[256];
	if (!name || !*name || strstr(name, "..") || name[0] == '/' || strchr(name, '\\') || strchr(name, ':'))
		return false;
	snprintf(rel, sizeof(rel), "data/%s", name);
	return FS_WritePath(rel, out, outsize);
}

/* float voxel_save(string name, string meta): writes data/<name>, keeping
 * the previous file as <name>.bak; the new file is written to a temporary
 * name first so a crash never leaves a half-written save in place */
static void bi_voxel_save(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0), *meta = vm->argc > 1 ? QC_PSTR(vm, 1) : "";
	char path[1024], tmp[1100], bak[1100];
	uint8_t *data;
	size_t len;
	FILE *f;
	bool ok;
	vx_error[0] = 0;
	if (!sv.voxel || !data_path(name, path, sizeof(path))) {
		Str_Copy(vx_error, sv.voxel ? "bad file name" : "no voxel world", sizeof(vx_error));
		QC_ReturnFloat(vm, 0);
		return;
	}
	if (!Vox_SaveToMemory(sv.voxel, (const uint8_t *)meta, strlen(meta), &data, &len)) {
		Str_Copy(vx_error, "encode failed", sizeof(vx_error));
		QC_ReturnFloat(vm, 0);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	snprintf(bak, sizeof(bak), "%s.bak", path);
	FS_CreatePath(path);
	f = fopen(tmp, "wb");
	ok = f && fwrite(data, 1, len, f) == len;
	if (f)
		ok = (fclose(f) == 0) && ok;
	Mem_Free(data);
	if (ok) {
		rename(path, bak); /* there may be no previous file */
		ok = rename(tmp, path) == 0;
	}
	if (!ok) {
		snprintf(vx_error, sizeof(vx_error), "write failed: %s", strerror(errno));
		remove(tmp);
	}
	QC_ReturnFloat(vm, ok ? 1 : 0);
}

/* float voxel_load(string name): replaces the world with data/<name>; on
 * failure the current world stays and voxel_error() says why */
static void bi_voxel_load(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	char rel[256], err[200];
	size_t len, mlen;
	uint8_t *data, *meta;
	vxworld_t *w;
	vx_error[0] = 0;
	if (!name || !*name || strstr(name, "..")) {
		Str_Copy(vx_error, "bad file name", sizeof(vx_error));
		QC_ReturnFloat(vm, 0);
		return;
	}
	snprintf(rel, sizeof(rel), "data/%s", name);
	data = (uint8_t *)FS_LoadFile(rel, &len);
	if (!data) {
		snprintf(vx_error, sizeof(vx_error), "%s not found", rel);
		QC_ReturnFloat(vm, 0);
		return;
	}
	w = Vox_LoadFromMemory(data, len, sv.voxel, &meta, &mlen, &vx_unknown, err, sizeof(err));
	Mem_Free(data);
	if (!w) {
		snprintf(vx_error, sizeof(vx_error), "%s: %s", rel, err);
		QC_ReturnFloat(vm, 0);
		return;
	}
	Vox_Free(sv.voxel);
	sv.voxel = w;
	Mem_Free(vx_meta);
	vx_meta = meta ? (char *)meta : NULL;
	send_world(NULL);
	QC_ReturnFloat(vm, 1);
}

static void bi_voxel_meta(qcvm_t *vm) { QC_ReturnString(vm, vx_meta ? vx_meta : ""); }
static void bi_voxel_error(qcvm_t *vm) { QC_ReturnString(vm, vx_error); }
static void bi_voxel_unknown(qcvm_t *vm) { QC_ReturnFloat(vm, (float)vx_unknown); }

const qc_builtin_t sv_voxel_builtins[] = {
	{"voxel_create", bi_voxel_create},
	{"voxel_free", bi_voxel_free},
	{"voxel_define", bi_voxel_define},
	{"voxel_set", bi_voxel_set},
	{"voxel_fill", bi_voxel_fill},
	{"voxel_get", bi_voxel_get},
	{"voxel_getrot", bi_voxel_getrot},
	{"voxel_cellforpoint", VX_bi_voxel_cellforpoint},
	{"voxel_cellorigin", VX_bi_voxel_cellorigin},
	{"voxel_size", VX_bi_voxel_size},
	{"voxel_blocksize", VX_bi_voxel_blocksize},
	{"voxel_count", VX_bi_voxel_count},
	{"voxel_typeforname", VX_bi_voxel_typeforname},
	{"voxel_typename", VX_bi_voxel_typename},
	{"voxel_tracecell", VX_bi_voxel_tracecell},
	{"voxel_tracetype", VX_bi_voxel_tracetype},
	{"voxel_save", bi_voxel_save},
	{"voxel_load", bi_voxel_load},
	{"voxel_meta", bi_voxel_meta},
	{"voxel_error", bi_voxel_error},
	{"voxel_unknown", bi_voxel_unknown},
	{NULL, NULL}};
