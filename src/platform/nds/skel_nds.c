/* Bulwark engine - skeletal models on the DS: plays a .bsk file (a
 * skeleton and its clips, written on the PC by tools/nds/skelreduce) and
 * draws a box per hitbox on its bones, for measuring what a skeleton costs
 * before there is a mesh. ISC licence, see LICENSE.
 *
 * Posing is on the CPU in fixed point: each bone's key either side of the
 * time, blended (nlerp, one hardware square root and one hardware divide a
 * bone), turned into a matrix and multiplied onto its parent's. The GPU then
 * draws each bone's part with the bone's matrix. With up to SKEL_HW_BONES
 * bones the matrices go into the GPU's position matrix stack once, and each
 * part only restores its bone's slot: the way a mesh with one bone a vertex
 * would be drawn. With more bones each part loads its matrix again.
 *
 * File layout (all little-endian, 16-bit aligned):
 *   "BSK1", u16 bones, u16 clips, u32 frames, u16 boxes, u16 0
 *   bones x 32:  s8 parent (-1 none), u8 0, char name[30]
 *   boxes x 14:  u8 bone, u8 hit group, s16 mins[3], s16 maxs[3] (1/8 units, bone space)
 *   clips x 44:  char name[32], u32 first frame, u16 frames, u16 fps x 16, u16 loops, u16 0
 *   frames x bones x 14: s16 quaternion x y z w (x 16384), s16 translation (1/8 units),
 *                each bone relative to its parent */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nds.h"

#define SKEL_SLOT0 8 /* matrix stack slots from here on hold bones; the ones below are left for pushes */
#define SKEL_HW_BONES (31 - SKEL_SLOT0)

typedef struct {
	int16_t q[4], t[3];
} skkey_t;

typedef struct {
	char name[32];
	int first, count, fps16, loop;
} skclip_t;

typedef struct {
	uint8_t bone, group;
	int16_t mins[3], maxs[3];
} skbox_t;

struct skel_s {
	uint8_t *file;
	size_t size;
	int numbones, numclips, numframes, numboxes;
	int8_t parent[64];
	char names[64][31];
	skclip_t *clips;
	skbox_t *boxes;
	const skkey_t *keys;
};

static int rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24; }

skel_t *Skel_Load(const char *path, char *err, int errsize)
{
	size_t size;
	uint8_t *d = Fs_Load(path, &size), *p;
	skel_t *s;
	if (!d) {
		snprintf(err, errsize, "no %s", path);
		return NULL;
	}
	if (size < 16 || memcmp(d, "BSK1", 4)) {
		snprintf(err, errsize, "%s: not a BSK1 file", path);
		free(d);
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	s->file = d;
	s->size = size;
	s->numbones = rd16(d + 4);
	s->numclips = rd16(d + 6);
	s->numframes = rd32(d + 8);
	s->numboxes = rd16(d + 12);
	if (s->numbones < 1 || s->numbones > 64 ||
	    size < 16 + (size_t)s->numbones * 32 + (size_t)s->numboxes * 14 + (size_t)s->numclips * 44 + (size_t)s->numframes * s->numbones * 14) {
		snprintf(err, errsize, "%s: cut short or too many bones", path);
		Skel_Free(s);
		return NULL;
	}
	p = d + 16;
	for (int i = 0; i < s->numbones; i++, p += 32) {
		s->parent[i] = (int8_t)p[0];
		if (s->parent[i] >= i)
			s->parent[i] = -1;
		memcpy(s->names[i], p + 2, 30);
	}
	s->boxes = calloc((size_t)s->numboxes + 1, sizeof(skbox_t));
	for (int i = 0; i < s->numboxes; i++, p += 14) {
		skbox_t *b = &s->boxes[i];
		b->bone = p[0] < s->numbones ? p[0] : 0;
		b->group = p[1];
		for (int k = 0; k < 3; k++)
			b->mins[k] = (int16_t)rd16(p + 2 + k * 2), b->maxs[k] = (int16_t)rd16(p + 8 + k * 2);
	}
	s->clips = calloc((size_t)s->numclips + 1, sizeof(skclip_t));
	for (int i = 0; i < s->numclips; i++, p += 44) {
		skclip_t *c = &s->clips[i];
		memcpy(c->name, p, 31);
		c->first = rd32(p + 32);
		c->count = rd16(p + 36);
		c->fps16 = rd16(p + 38);
		c->loop = rd16(p + 40);
		if (c->count < 1 || c->first + c->count > s->numframes)
			c->first = 0, c->count = 1;
	}
	s->keys = (const skkey_t *)p;
	return s;
}

void Skel_Free(skel_t *s)
{
	if (!s)
		return;
	free(s->file);
	free(s->clips);
	free(s->boxes);
	free(s);
}

int Skel_NumBones(const skel_t *s) { return s->numbones; }
int Skel_NumClips(const skel_t *s) { return s->numclips; }
int Skel_FileBytes(const skel_t *s) { return (int)s->size; }
const char *Skel_ClipName(const skel_t *s, int clip) { return s->clips[clip % s->numclips].name; }
int Skel_ClipFrames(const skel_t *s, int clip) { return s->clips[clip % s->numclips].count; }

/* clip at time (seconds, 20.12): every bone's model-space matrix, 4.12
 * rotation in m[0..8] as the GPU's 4x3 wants it (rows are the bone's axes)
 * and the translation in 1/8 units in m[9..11] */
void Skel_Pose(const skel_t *s, int clip, int32_t time, skmat_t *out)
{
	const skclip_t *c = &s->clips[clip % s->numclips];
	int32_t ft = (int32_t)((int64_t)time * c->fps16 >> 4); /* frames, 20.12 */
	int f0 = ft >> 12, f1, frac = ft & 4095, nb = s->numbones;
	const skkey_t *ka, *kb;
	if (c->loop && c->count > 1) {
		f0 %= c->count - 1; /* the last frame of a loop repeats the first */
		f1 = f0 + 1;
	} else if (f0 >= c->count - 1) {
		f0 = f1 = c->count - 1, frac = 0;
	} else {
		f1 = f0 + 1;
	}
	ka = s->keys + (c->first + f0) * nb;
	kb = s->keys + (c->first + f1) * nb;
	for (int i = 0; i < nb; i++) {
		int32_t q[4], t[3], len, inv, r[9];
		int32_t x, y, z, w, xx, yy, zz, xy, xz, yz, wx, wy, wz, sign;
		sign = ka[i].q[0] * kb[i].q[0] + ka[i].q[1] * kb[i].q[1] + ka[i].q[2] * kb[i].q[2] + ka[i].q[3] * kb[i].q[3] < 0 ? -1 : 1;
		for (int k = 0; k < 4; k++)
			q[k] = ka[i].q[k] + ((sign * kb[i].q[k] - ka[i].q[k]) * frac >> 12);
		for (int k = 0; k < 3; k++)
			t[k] = ka[i].t[k] + ((kb[i].t[k] - ka[i].t[k]) * frac >> 12);
		len = sqrt32(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		inv = len ? div32(1 << 28, len) : 16384; /* 1.14 */
		x = q[0] * inv >> 14, y = q[1] * inv >> 14, z = q[2] * inv >> 14, w = q[3] * inv >> 14;
		xx = x * x >> 13, yy = y * y >> 13, zz = z * z >> 13; /* 2xx and so on, 1.14 */
		xy = x * y >> 13, xz = x * z >> 13, yz = y * z >> 13, wx = w * x >> 13, wy = w * y >> 13, wz = w * z >> 13;
		/* the rotation, column-vector style (r[row*3+col]), 4.12 */
		r[0] = (16384 - yy - zz) >> 2, r[1] = (xy - wz) >> 2, r[2] = (xz + wy) >> 2;
		r[3] = (xy + wz) >> 2, r[4] = (16384 - xx - zz) >> 2, r[5] = (yz - wx) >> 2;
		r[6] = (xz - wy) >> 2, r[7] = (yz + wx) >> 2, r[8] = (16384 - xx - yy) >> 2;
		if (s->parent[i] < 0) {
			for (int row = 0; row < 3; row++)
				for (int col = 0; col < 3; col++)
					out[i].m[col * 3 + row] = r[row * 3 + col];
			for (int k = 0; k < 3; k++)
				out[i].m[9 + k] = t[k];
		} else {
			const int32_t *p = out[s->parent[i]].m; /* p[col*3+row] */
			for (int row = 0; row < 3; row++) {
				for (int col = 0; col < 3; col++)
					out[i].m[col * 3 + row] =
					    (p[row] * r[col] + p[3 + row] * r[3 + col] + p[6 + row] * r[6 + col]) >> 12;
				out[i].m[9 + row] = ((p[row] * t[0] + p[3 + row] * t[1] + p[6 + row] * t[2]) >> 12) + p[9 + row];
			}
		}
	}
}

static const uint16_t groupcolor[9] = {
    RGB15(20, 20, 22), RGB15(31, 24, 8), RGB15(31, 12, 10), RGB15(28, 20, 12), RGB15(10, 26, 12),
    RGB15(12, 22, 31), RGB15(16, 31, 20), RGB15(20, 16, 31), RGB15(24, 24, 24),
};

static uint16_t shade(uint16_t c, int n)
{
	return RGB15((c & 31) * n >> 4, ((c >> 5) & 31) * n >> 4, ((c >> 10) & 31) * n >> 4);
}

static void draw_box(const skbox_t *b)
{
	int16_t x0 = b->mins[0], y0 = b->mins[1], z0 = b->mins[2], x1 = b->maxs[0], y1 = b->maxs[1], z1 = b->maxs[2];
	uint16_t c = groupcolor[b->group < 9 ? b->group : 8];
	glBegin(GL_QUADS);
	glColor(shade(c, 16));
	glVertex3v16(x0, y0, z1), glVertex3v16(x1, y0, z1), glVertex3v16(x1, y1, z1), glVertex3v16(x0, y1, z1);
	glColor(shade(c, 7));
	glVertex3v16(x0, y1, z0), glVertex3v16(x1, y1, z0), glVertex3v16(x1, y0, z0), glVertex3v16(x0, y0, z0);
	glColor(shade(c, 13));
	glVertex3v16(x1, y0, z0), glVertex3v16(x1, y1, z0), glVertex3v16(x1, y1, z1), glVertex3v16(x1, y0, z1);
	glColor(shade(c, 10));
	glVertex3v16(x0, y1, z0), glVertex3v16(x0, y0, z0), glVertex3v16(x0, y0, z1), glVertex3v16(x0, y1, z1);
	glColor(shade(c, 11));
	glVertex3v16(x0, y0, z0), glVertex3v16(x1, y0, z0), glVertex3v16(x1, y0, z1), glVertex3v16(x0, y0, z1);
	glColor(shade(c, 12));
	glVertex3v16(x1, y1, z0), glVertex3v16(x0, y1, z0), glVertex3v16(x0, y1, z1), glVertex3v16(x1, y1, z1);
	glEnd();
}

/* the posed boxes at org (map units) turned by yaw; the modelview holds the view */
void Skel_Draw(const skel_t *s, const skmat_t *pose, const int32_t *org, int yaw)
{
	bool hw = s->numbones <= SKEL_HW_BONES;
	glPushMatrix();
	glTranslatef32(org[0] * 8, org[1] * 8, org[2] * 8);
	glRotateZi(yaw);
	if (hw) {
		/* each bone's full matrix into its slot once, as a skinned mesh would */
		for (int i = 0; i < s->numbones; i++) {
			glPushMatrix();
			glMultMatrix4x3((const m4x3 *)pose[i].m);
			glStoreMatrix(SKEL_SLOT0 + i);
			glPopMatrix(1);
		}
		for (int i = 0; i < s->numboxes; i++) {
			glRestoreMatrix(SKEL_SLOT0 + s->boxes[i].bone);
			draw_box(&s->boxes[i]);
		}
	} else {
		for (int i = 0; i < s->numboxes; i++) {
			glPushMatrix();
			glMultMatrix4x3((const m4x3 *)pose[s->boxes[i].bone].m);
			draw_box(&s->boxes[i]);
			glPopMatrix(1);
		}
	}
	glPopMatrix(1);
}

/* --- the test screen -------------------------------------------------------------------- */

#define SKEL_MAX_SHOWN 16

static void draw_floor(void)
{
	glBegin(GL_QUADS);
	for (int gx = -2; gx < 6; gx++)
		for (int gy = -4; gy < 4; gy++) {
			int x = gx * 64 * 8, y = gy * 64 * 8;
			glColor((gx + gy) & 1 ? RGB15(7, 7, 9) : RGB15(9, 9, 11));
			glVertex3v16(x, y, 0), glVertex3v16(x + 512, y, 0), glVertex3v16(x + 512, y + 512, 0), glVertex3v16(x, y + 512, 0);
		}
	glEnd();
}

typedef struct {
	int fps, worst, posecycles, polys, verts;
} skrun_t;

/* draws count copies for frames frames (0: until START), each a clip apart,
 * timing the posing; clip -1 gives every copy its own clip */
static void run(const skel_t *s, int count, int clip, int frames, const char *label, skrun_t *r)
{
	static skmat_t pose[64];
	ndscam_t cam = {{-150 * 8, 0, 60 * 8}, 0, -1000, 0};
	int start = Nds_Vblanks(), last = start, worst = 0, n = 0, shown = 0;
	uint32_t posesum = 0;
	Mp_Stop();
	glBindTexture(GL_TEXTURE_2D, 0);
	memset(r, 0, sizeof(*r));
	for (;;) {
		int32_t t = (Nds_Vblanks() - start) * 4096 / 60;
		uint32_t ticks;
		scanKeys();
		if (!frames) {
			uint32_t down = keysDown();
			if (down & KEY_START)
				break;
			if (down & KEY_R)
				clip = (clip + 1) % s->numclips;
			if (down & KEY_L)
				clip = clip <= 0 ? s->numclips - 1 : clip - 1;
			if (down & KEY_UP)
				count = count < SKEL_MAX_SHOWN ? count + 1 : count;
			if (down & KEY_DOWN)
				count = count > 1 ? count - 1 : 1;
		} else if (n >= frames) {
			break;
		}
		World_SetView(&cam);
		draw_floor();
		ticks = 0;
		for (int i = 0; i < count; i++) {
			int row = count < 4 ? count : 4;
			int32_t org[3] = {(i / 4) * 70, ((i % 4) * 2 - (row - 1)) * 30, 0};
			int c = clip < 0 ? i : clip;
			cpuStartTiming(0);
			Skel_Pose(s, c, t + i * 1300, pose);
			ticks += cpuEndTiming();
			Skel_Draw(s, pose, org, 16384); /* facing the camera */
		}
		posesum += ticks;
		Gpu_Counts(&r->polys, &r->verts);
		glFlush(0);
		swiWaitForVBlank();
		n++;
		{
			int took = Nds_Vblanks() - last;
			last = Nds_Vblanks();
			if (took > worst && n > 2)
				worst = took;
		}
		if (!frames && n - shown >= 30) {
			int elapsed = Nds_Vblanks() - start;
			consoleClear();
			iprintf("%s: %d bones, %d boxes\n", label, s->numbones, s->numboxes);
			iprintf("clip %d/%d %s (%d frames)\n", clip + 1, s->numclips, clip < 0 ? "mixed" : s->clips[clip].name,
			        clip < 0 ? 0 : s->clips[clip].count);
			iprintf("%d shown, %d fps\n", count, elapsed ? n * 60 / elapsed : 0);
			/* the timer runs at 33.51 MHz: ticks * 1000 / 33514 is microseconds */
			iprintf("posing: %d us a copy\n", (int)(ticks * 1000 / 33514 / (count ? count : 1)));
			iprintf("GPU: %d polys, %d verts\n", r->polys, r->verts);
			iprintf("file %d KB (%d B a frame)\n", (int)(s->size >> 10), s->numbones * 14);
			iprintf("%s matrices\n", s->numbones <= SKEL_HW_BONES ? "stored once (GPU slots)" : "loaded per box");
			iprintf("L/R: clip  UP/DOWN: copies\nSTART: map list\n");
			Nds_PrintResults();
			shown = n;
		}
	}
	{
		int elapsed = Nds_Vblanks() - start;
		r->fps = elapsed ? n * 60 / elapsed : 0;
		r->worst = 60 / (worst ? worst : 1);
		r->posecycles = n && count ? (int)(posesum / (uint32_t)n / (uint32_t)count) : 0;
	}
}

/* "<file> <copies> [clip]": a timed run for the bench results, every copy on its own clip unless one is named */
void Skel_Bench(const char *args, char *result, int resultsize)
{
	char path[200] = "", clipname[40] = "", err[96];
	int count = 1, clip = -1;
	skel_t *s;
	skrun_t r;
	sscanf(args, "%199s %d %39s", path, &count, clipname);
	if (!(s = Skel_Load(path, err, sizeof(err)))) {
		snprintf(result, resultsize, "%.40s", err);
		return;
	}
	for (int i = 0; clipname[0] && i < s->numclips; i++)
		if (!strcasecmp(s->clips[i].name, clipname))
			clip = i;
	count = BW_NDS_CLAMP(count, 1, SKEL_MAX_SHOWN);
	run(s, count, clip, 240, path, &r);
	{
		/* one line of the bottom screen: "tank12 12x8 60/60fps 166us p970" */
		const char *base = strrchr(path, '/');
		char name[12];
		snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
		name[strcspn(name, ".")] = 0;
		snprintf(result, resultsize, "%.7s %dx%d %d/%dfps %dus p%d", name, s->numbones, count, r.fps, r.worst,
		         r.posecycles * 1000 / 33514, r.polys);
	}
	Skel_Free(s);
}

/* the interactive screen: L/R through the clips, UP/DOWN for copies */
void Skel_View(const char *args)
{
	char path[200] = "", err[96];
	int count = 1;
	skel_t *s;
	skrun_t r;
	sscanf(args, "%199s %d", path, &count);
	if (!(s = Skel_Load(path, err, sizeof(err)))) {
		iprintf("%s\n", err);
		return;
	}
	run(s, BW_NDS_CLAMP(count, 1, SKEL_MAX_SHOWN), 0, 0, path, &r);
	Skel_Free(s);
}
