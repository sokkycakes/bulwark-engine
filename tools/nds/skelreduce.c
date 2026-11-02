/* Bulwark engine - cuts a Source model's skeleton down to a few bones for
 * the DS, and writes the result as a .bsk file (docs/platforms/nds.md,
 * "Skeletons"). ISC licence, see LICENSE.
 *
 *   skelreduce <game folder> <model.mdl> <out.bsk> [options]
 *     -keep a,b,c   bones to keep, by name or by the end of a name
 *                   ("Pelvis" finds ValveBiped.Bip01_Pelvis); default: all
 *     -clips a,b    sequences to write, by exact name; default: all of the
 *                   model's own sequences (not blend cells)
 *     -fps n        resample the clips to n frames a second (0: as stored)
 *     -report file  per-bone and per-clip error figures, as text
 *     -ignore a,b   bones left out of the error figures, such as a prop
 *                   bone that leaves the hand (the Tank's thrown rock)
 *     -poses file   joint positions of chosen frames, full and cut down, for
 *                   drawing on the PC (see tools/nds/skelplot.py)
 *
 * Every kept bone keeps its exact place: its pose is taken from the full
 * skeleton relative to its nearest kept ancestor, so whatever the bones in
 * between did is folded into it. A dropped bone (and its hitboxes) rides
 * rigidly on its nearest kept ancestor, as it was in the bind pose. The
 * report says how far that puts it from where the full skeleton has it.
 *
 * The file is the model's data cut down, so it carries the game's
 * copyright like the model: keep it out of git and out of packages. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../plugins/source/mdlskel.h"

void skelstub_root(const char *dir);

#define MAXB 256

typedef struct {
	float m[12];
} mat_t;

static void mat_inv(float o[12], const float a[12])
{
	/* rotation transposed, translation turned back */
	o[0] = a[0], o[1] = a[4], o[2] = a[8];
	o[4] = a[1], o[5] = a[5], o[6] = a[9];
	o[8] = a[2], o[9] = a[6], o[10] = a[10];
	for (int r = 0; r < 3; r++)
		o[r * 4 + 3] = -(o[r * 4] * a[3] + o[r * 4 + 1] * a[7] + o[r * 4 + 2] * a[11]);
}

/* a rotation matrix (rows of 4, as mdlskel's) to a quaternion x y z w */
static void mat_quat(const float m[12], float q[4])
{
	float t = m[0] + m[5] + m[10];
	if (t > 0) {
		float s = sqrtf(t + 1) * 2;
		q[3] = s / 4, q[0] = (m[9] - m[6]) / s, q[1] = (m[2] - m[8]) / s, q[2] = (m[4] - m[1]) / s;
	} else if (m[0] > m[5] && m[0] > m[10]) {
		float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
		q[3] = (m[9] - m[6]) / s, q[0] = s / 4, q[1] = (m[1] + m[4]) / s, q[2] = (m[2] + m[8]) / s;
	} else if (m[5] > m[10]) {
		float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
		q[3] = (m[2] - m[8]) / s, q[0] = (m[1] + m[4]) / s, q[1] = s / 4, q[2] = (m[6] + m[9]) / s;
	} else {
		float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
		q[3] = (m[4] - m[1]) / s, q[0] = (m[2] + m[8]) / s, q[1] = (m[6] + m[9]) / s, q[2] = s / 4;
	}
	if (q[3] < 0)
		for (int k = 0; k < 4; k++)
			q[k] = -q[k];
}

static void nlerp(const float *a, const float *b, float t, float *o)
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1 : 1, l = 0;
	for (int k = 0; k < 4; k++)
		o[k] = a[k] + (s * b[k] - a[k]) * t, l += o[k] * o[k];
	l = sqrtf(l);
	for (int k = 0; k < 4; k++)
		o[k] /= l;
}

/* the full skeleton's model-space matrices for one stored frame */
static void pose(const sbone_t *b, int nb, const float *fr, mat_t *w)
{
	for (int i = 0; i < nb; i++) {
		float rel[12];
		mat_from_pq(rel, fr + i * 10, fr + i * 10 + 3);
		if (b[i].parent >= 0)
			mat_mul(w[i].m, w[b[i].parent].m, rel);
		else
			memcpy(w[i].m, rel, sizeof(rel));
	}
}

static int name_matches(const char *bone, const char *want)
{
	size_t bl = strlen(bone), wl = strlen(want);
	if (!strcasecmp(bone, want))
		return 1;
	return bl > wl && !strcasecmp(bone + bl - wl, want) && (bone[bl - wl - 1] == '.' || bone[bl - wl - 1] == '_');
}

static int in_list(const char *list, const char *name, int (*match)(const char *, const char *))
{
	char buf[4096], *tok;
	snprintf(buf, sizeof(buf), "%s", list);
	for (tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
		if (match(name, tok))
			return 1;
	return 0;
}

static int cmp_double(const void *x, const void *y) { return *(const double *)x < *(const double *)y ? -1 : *(const double *)x > *(const double *)y; }

static int exact(const char *a, const char *b) { return !strcasecmp(a, b); }

static void put16(FILE *f, int v)
{
	fputc(v & 255, f);
	fputc((v >> 8) & 255, f);
}
static void put32(FILE *f, unsigned v)
{
	put16(f, (int)(v & 0xffff));
	put16(f, (int)(v >> 16));
}
static int clamp16(float v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : (int)lrintf(v); }

int main(int argc, char **argv)
{
	const char *keep = NULL, *ignore = NULL, *clips = NULL, *reportpath = NULL, *posespath = NULL;
	float wantfps = 0;
	size_t len;
	blob_t m;
	sbone_t *b;
	int nb, keepmap[MAXB], kept[MAXB], nk = 0, anc[MAXB], newparent[MAXB];
	float (*rest)[12];
	boxes_t bx = {0};
	anims_t an = {0};
	FILE *out, *rep = NULL, *pz = NULL;
	if (argc < 4) {
		fprintf(stderr, "usage: skelreduce <game folder> <model.mdl> <out.bsk> [-keep a,b] [-ignore a,b] [-clips a,b] [-fps n] [-report file] [-poses file]\n");
		return 1;
	}
	for (int i = 4; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-keep"))
			keep = argv[i + 1];
		else if (!strcmp(argv[i], "-ignore"))
			ignore = argv[i + 1];
		else if (!strcmp(argv[i], "-clips"))
			clips = argv[i + 1];
		else if (!strcmp(argv[i], "-fps"))
			wantfps = (float)atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-report"))
			reportpath = argv[i + 1];
		else if (!strcmp(argv[i], "-poses"))
			posespath = argv[i + 1];
	}
	skelstub_root(argv[1]);
	m.d = api->load_file(argv[2], &len);
	m.len = len;
	if (!m.d) {
		fprintf(stderr, "can't read %s/%s\n", argv[1], argv[2]);
		return 1;
	}
	nb = mdl_bones(&m, &b);
	if (nb <= 0) {
		fprintf(stderr, "no bones\n");
		return 1;
	}
	rest = calloc((size_t)nb, sizeof(*rest));
	mdl_restpose(b, nb, rest);
	mdl_boxes(&m, b, nb, &bx);
	mdl_anims(argv[2], &m, b, nb, &an);

	/* which bones stay; the root always does */
	for (int i = 0; i < nb; i++) {
		keepmap[i] = -1;
		if (!keep || b[i].parent < 0 || in_list(keep, b[i].name, name_matches)) {
			keepmap[i] = nk;
			kept[nk++] = i;
		}
	}
	for (int i = 0; i < nb; i++) {
		int a = i;
		while (keepmap[a] < 0)
			a = b[a].parent;
		anc[i] = a; /* the bone itself if kept */
	}
	for (int k = 0; k < nk; k++) {
		int p = b[kept[k]].parent;
		newparent[k] = p < 0 ? -1 : keepmap[anc[p]];
	}

	if (reportpath && !(rep = fopen(reportpath, "w")))
		return 1;
	if (posespath && !(pz = fopen(posespath, "w")))
		return 1;

	/* clips: the model's own sequences, blend cells (named "seq:anim") left out */
	int nclips = 0, *clipidx = calloc((size_t)an.num + 1, sizeof(int)), totalframes = 0;
	for (int c = 0; c < an.num; c++) {
		if (strchr(an.a[c].name, ':'))
			continue;
		if (clips && !in_list(clips, an.a[c].name, exact))
			continue;
		clipidx[nclips++] = c;
	}
	int *outframes = calloc((size_t)nclips + 1, sizeof(int));
	float *outfps = calloc((size_t)nclips + 1, sizeof(float));
	for (int c = 0; c < nclips; c++) {
		sanim_t *s = &an.a[clipidx[c]];
		outfps[c] = wantfps > 0 ? wantfps : s->fps;
		outframes[c] = s->count <= 1 ? 1 : (int)lrintf((s->count - 1) * outfps[c] / s->fps) + 1;
		if (outframes[c] < 1)
			outframes[c] = 1;
		totalframes += outframes[c];
	}

	/* hitboxes folded onto kept bones: corners taken to the kept bone's bind space */
	int nboxes = bx.numhitboxes ? bx.numhitboxes : bx.num;
	if (!(out = fopen(argv[3], "wb")))
		return 1;
	fwrite("BSK1", 1, 4, out);
	put16(out, nk);
	put16(out, nclips);
	put32(out, (unsigned)totalframes);
	put16(out, nboxes);
	put16(out, 0);
	for (int k = 0; k < nk; k++) {
		char name[32] = {0};
		const char *n = strrchr(b[kept[k]].name, '.');
		snprintf(name, sizeof(name), "%.29s", n ? n + 1 : b[kept[k]].name);
		fputc(newparent[k] & 255, out);
		fputc(0, out);
		fwrite(name, 1, 30, out);
	}
	for (int i = 0; i < nboxes; i++) {
		const sbox_t *x = &bx.b[i];
		int a = anc[x->bone];
		float inv[12], tobone[12], lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
		mat_inv(inv, rest[a]);
		mat_mul(tobone, inv, rest[x->bone]);
		for (int c = 0; c < 8; c++) {
			float p[3] = {c & 1 ? x->maxs[0] : x->mins[0], c & 2 ? x->maxs[1] : x->mins[1], c & 4 ? x->maxs[2] : x->mins[2]}, o[3];
			mat_point(tobone, p, o);
			for (int k = 0; k < 3; k++)
				lo[k] = o[k] < lo[k] ? o[k] : lo[k], hi[k] = o[k] > hi[k] ? o[k] : hi[k];
		}
		/* a dropped bone's box turned onto its ancestor is an AABB of the turned box */
		fputc(keepmap[a], out);
		fputc(x->color, out);
		for (int k = 0; k < 3; k++)
			put16(out, clamp16(lo[k] * 8));
		for (int k = 0; k < 3; k++)
			put16(out, clamp16(hi[k] * 8));
	}
	for (int c = 0, first = 0; c < nclips; c++) {
		char name[32] = {0};
		snprintf(name, sizeof(name), "%.31s", an.a[clipidx[c]].name);
		fwrite(name, 1, 32, out);
		put32(out, (unsigned)first);
		put16(out, outframes[c]);
		put16(out, clamp16(outfps[c] * 16));
		put16(out, an.a[clipidx[c]].loop ? 1 : 0);
		put16(out, 0);
		first += outframes[c];
	}

	/* frames, and the error figures */
	mat_t *w = calloc((size_t)nb, sizeof(mat_t));
	float *fa = malloc(sizeof(float) * 10 * (size_t)nb), *fb = malloc(sizeof(float) * 10 * (size_t)nb), *fr = malloc(sizeof(float) * 10 * (size_t)nb);
	double *bonemax = calloc((size_t)nb, sizeof(double)), *bonesum = calloc((size_t)nb, sizeof(double));
	double *transdev = calloc((size_t)nk, sizeof(double));
	float (*firstt)[3] = calloc((size_t)nk, sizeof(*firstt));
	int samples = 0, firstset = 0;
	double *errs = NULL;
	size_t nerrs = 0, caperrs = 0;
	if (rep)
		fprintf(rep, "clip                              frames  fps   worst cm  (bone)\n");
	for (int c = 0; c < nclips; c++) {
		sanim_t *s = &an.a[clipidx[c]];
		double clipworst = 0;
		int clipworstbone = -1;
		for (int f = 0; f < outframes[c]; f++) {
			/* resample: the stored frames either side, blended */
			float t = outframes[c] > 1 ? (float)f * s->fps / outfps[c] : 0;
			int f0 = (int)t, f1 = f0 + 1 < s->count ? f0 + 1 : f0;
			float frac = t - (float)f0;
			if (f0 >= s->count)
				f0 = f1 = s->count - 1, frac = 0;
			memcpy(fa, an.frames + (size_t)(s->first + f0) * nb * 10, sizeof(float) * 10 * (size_t)nb);
			memcpy(fb, an.frames + (size_t)(s->first + f1) * nb * 10, sizeof(float) * 10 * (size_t)nb);
			for (int i = 0; i < nb; i++) {
				for (int k = 0; k < 3; k++)
					fr[i * 10 + k] = fa[i * 10 + k] + (fb[i * 10 + k] - fa[i * 10 + k]) * frac;
				nlerp(fa + i * 10 + 3, fb + i * 10 + 3, frac, fr + i * 10 + 3);
			}
			pose(b, nb, fr, w);
			for (int k = 0; k < nk; k++) {
				float local[12], inv[12], q[4];
				int i = kept[k];
				if (newparent[k] < 0) {
					memcpy(local, w[i].m, sizeof(local));
				} else {
					mat_inv(inv, w[kept[newparent[k]]].m);
					mat_mul(local, inv, w[i].m);
				}
				mat_quat(local, q);
				for (int j = 0; j < 4; j++)
					put16(out, clamp16(q[j] * 16384));
				for (int j = 0; j < 3; j++) {
					float v = local[j * 4 + 3];
					put16(out, clamp16(v * 8));
					if (!firstset)
						firstt[k][j] = v;
					else if (fabs(v - firstt[k][j]) > transdev[k])
						transdev[k] = fabs(v - firstt[k][j]);
				}
			}
			firstset = 1;
			double framemax = 0;
			/* where each dropped bone's hitbox corners (or its joint) end up riding its ancestor */
			for (int i = 0; i < nb; i++) {
				int a = anc[i];
				float inv[12], rideo[12], ride[12];
				double worst = 0;
				if (a == i || (ignore && in_list(ignore, b[i].name, name_matches)))
					continue;
				mat_inv(inv, rest[a]);
				mat_mul(rideo, inv, rest[i]);
				mat_mul(ride, w[a].m, rideo);
				for (int x = -1; x < bx.numhitboxes; x++) {
					float pts[8][3];
					int npts = 1;
					if (x >= 0 && bx.b[x].bone != i)
						continue;
					if (x < 0) {
						pts[0][0] = pts[0][1] = pts[0][2] = 0;
					} else {
						npts = 8;
						for (int cc = 0; cc < 8; cc++)
							for (int k = 0; k < 3; k++)
								pts[cc][k] = cc & (1 << k) ? bx.b[x].maxs[k] : bx.b[x].mins[k];
					}
					for (int p = 0; p < npts; p++) {
						float t1[3], t2[3];
						mat_point(w[i].m, pts[p], t1);
						mat_point(ride, pts[p], t2);
						double d = sqrt((t1[0] - t2[0]) * (t1[0] - t2[0]) + (t1[1] - t2[1]) * (t1[1] - t2[1]) + (t1[2] - t2[2]) * (t1[2] - t2[2]));
						if (d > worst)
							worst = d;
					}
				}
				if (worst > bonemax[i])
					bonemax[i] = worst;
				bonesum[i] += worst;
				if (worst > clipworst)
					clipworst = worst, clipworstbone = i;
				if (worst > framemax)
					framemax = worst;
			}
			/* the frame's figure: its worst-placed part of the body, whichever bone that is */
			if (nerrs == caperrs) {
				caperrs = caperrs ? caperrs * 2 : 4096;
				errs = realloc(errs, sizeof(double) * caperrs);
			}
			errs[nerrs++] = framemax;
			samples++;
			if (pz) {
				/* every 4th frame: full joints, then the cut-down skeleton's joints */
				if (f % 4 == 0) {
					fprintf(pz, "F %s %d\n", s->name, f);
					for (int i = 0; i < nb; i++) {
						float inv[12], rideo[12], ride[12];
						int a = anc[i];
						mat_inv(inv, rest[a]);
						mat_mul(rideo, inv, rest[i]);
						mat_mul(ride, w[a].m, rideo);
						fprintf(pz, "J %d %d %d %.2f %.2f %.2f %.2f %.2f %.2f\n", i, b[i].parent, keepmap[i] >= 0, w[i].m[3], w[i].m[7], w[i].m[11],
						        ride[3], ride[7], ride[11]);
					}
				}
			}
		}
		if (rep)
			fprintf(rep, "%-32s %6d %5.1f %8.1f   %s\n", s->name, outframes[c], outfps[c], clipworst * 2.54,
			        clipworstbone >= 0 ? b[clipworstbone].name : "-");
	}
	long size = ftell(out);
	fclose(out);
	if (pz)
		fclose(pz);

	if (rep) {
		/* 1 Source unit is an inch (2.54 cm), so errors are given in cm too */
		fprintf(rep, "\nbones kept %d of %d; clips %d, frames %d; file %ld bytes (%d bytes a frame)\n", nk, nb, nclips, totalframes, size, nk * 14);
		fprintf(rep, "\nkept bones (parent), most a translation moves off its first value (units):\n");
		for (int k = 0; k < nk; k++)
			fprintf(rep, "  %2d %-28s parent %2d  %.2f\n", k, b[kept[k]].name, newparent[k], transdev[k]);
		fprintf(rep, "\ndropped bones: rides on, worst and mean offset of its hitbox corners or joint (units, 1 unit = 2.54 cm)\n");
		for (int i = 0; i < nb; i++)
			if (anc[i] != i)
				fprintf(rep, "  %-28s on %-28s worst %6.2f  mean %6.2f\n", b[i].name, b[anc[i]].name, bonemax[i], samples ? bonesum[i] / samples : 0);
		if (nerrs) {
			qsort(errs, nerrs, sizeof(double), cmp_double);
			fprintf(rep, "\nthe worst-placed part of the body in each frame, over all %d frames: median %.2f, 95%% %.2f, worst %.2f units\n", (int)nerrs, errs[nerrs / 2], errs[nerrs * 95 / 100],
			        errs[nerrs - 1]);
		}
		fclose(rep);
	}
	printf("%s: %d of %d bones, %d clips, %d frames, %ld bytes\n", argv[3], nk, nb, nclips, totalframes, size);
	return 0;
}
