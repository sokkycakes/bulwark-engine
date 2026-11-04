/* Bulwark engine - poses a GoldSrc studio model (IDST 10) and writes every
 * frame of every sequence as plain vertex positions, with its triangles and
 * textures, for tools/nds/hlmdl.py to bake into a DS model. ISC licence,
 * see LICENSE.
 *
 *   hlmdldump <model.mdl> <out.dump> [body]
 *
 * The first sub-model of each body part is used (or the one "body" picks, as
 * the engine's body number does). Bone controllers are left at rest and only
 * the first blend of a sequence is taken. As the engine does, the root
 * motion a sequence's motion type names is taken out, so a walk stays put.
 *
 * Layout (little-endian): "HLD1", then u32 numverts, numtris, numtex,
 * numseqs; numtex x {char name[64], u32 w, u32 h, w*h indices, 768 palette};
 * numtris x {3 x {u32 vert, f32 s, f32 t}, u32 tex}; numseqs x {char
 * label[32], f32 fps, u32 flags, u32 activity, u32 numframes, numframes x
 * numverts x 3 f32}. s and t are in texels of their texture. */
#include "../../src/common/skel.h"
#include "../../src/common/studio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *readfile(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wf(FILE *f, float v) { fwrite(&v, 4, 1, f); }
static int rds(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p)
{
	float v;
	memcpy(&v, p, 4);
	return v;
}

typedef struct {
	int vert;
	float s, t;
} corner_t;

int main(int argc, char **argv)
{
	studio_model_t m;
	char err[256], tpath[1024];
	size_t len, tlen;
	uint8_t *buf, *tbuf = NULL;
	int body = argc > 3 ? atoi(argv[3]) : 0, numverts = 0, numtris = 0, cap = 4096;
	int subs[32], subbase[32], nsub = 0;
	corner_t (*tris)[3];
	int *tritex;
	FILE *out;
	if (argc < 3) {
		fprintf(stderr, "usage: hlmdldump <model.mdl> <out.dump> [body]\n");
		return 2;
	}
	if (!(buf = readfile(argv[1], &len)) || !Studio_Parse(&m, buf, len, err, sizeof(err))) {
		fprintf(stderr, "%s: %s\n", argv[1], buf ? err : "can't read");
		return 1;
	}
	if (!m.numtextures) {
		/* the textures live in nameT.mdl */
		snprintf(tpath, sizeof(tpath), "%.*sT.mdl", (int)(strlen(argv[1]) - 4), argv[1]);
		if (!(tbuf = readfile(tpath, &tlen)))
			tpath[strlen(tpath) - 5] = 't', tbuf = readfile(tpath, &tlen);
		if (!tbuf || !Studio_ParseTextures(&m, tbuf, tlen, err, sizeof(err)))
			fprintf(stderr, "%s: no textures (%s)\n", argv[1], tbuf ? err : tpath);
	}

	/* the sub-model the body number picks in each body part */
	for (int b = 0; b < m.numbodyparts && nsub < 32; b++) {
		const studio_bodypart_t *bp = &m.bodyparts[b];
		int which = bp->base ? (body / bp->base) % bp->nummodels : 0;
		subs[nsub] = b * 256 + which;
		subbase[nsub++] = numverts;
		numverts += bp->models[which].numverts;
	}

	/* triangles from the command lists: strips (n > 0) and fans (n < 0) */
	tris = malloc(sizeof(*tris) * (size_t)cap);
	tritex = malloc(sizeof(int) * (size_t)cap);
	for (int s = 0; s < nsub; s++) {
		const studio_submodel_t *sm = &m.bodyparts[subs[s] / 256].models[subs[s] % 256];
		for (int k = 0; k < sm->nummeshes; k++) {
			const studio_mesh_t *mesh = &sm->meshes[k];
			const uint8_t *p = mesh->cmds;
			int tex = m.skins && mesh->skinref < m.numskinref ? m.skins[mesh->skinref] : mesh->skinref;
			for (;;) {
				int n = rds(p), fan = n < 0;
				corner_t c[256];
				p += 2;
				if (!n)
					break;
				n = abs(n);
				for (int i = 0; i < n; i++, p += 8) {
					if (i < 256)
						c[i].vert = subbase[s] + rds(p), c[i].s = (float)rds(p + 4), c[i].t = (float)rds(p + 6);
				}
				for (int i = 2; i < n && i < 256; i++) {
					corner_t a, b2, d;
					if (fan)
						a = c[0], b2 = c[i - 1], d = c[i];
					else if (i & 1)
						a = c[i - 1], b2 = c[i - 2], d = c[i];
					else
						a = c[i - 2], b2 = c[i - 1], d = c[i];
					if (numtris >= cap) {
						cap *= 2;
						tris = realloc(tris, sizeof(*tris) * (size_t)cap);
						tritex = realloc(tritex, sizeof(int) * (size_t)cap);
					}
					tris[numtris][0] = a, tris[numtris][1] = b2, tris[numtris][2] = d;
					tritex[numtris++] = tex;
				}
			}
		}
	}

	if (!(out = fopen(argv[2], "wb"))) {
		fprintf(stderr, "%s: can't write\n", argv[2]);
		return 1;
	}
	fwrite("HLD1", 4, 1, out);
	w32(out, (uint32_t)numverts), w32(out, (uint32_t)numtris), w32(out, (uint32_t)m.numtextures), w32(out, (uint32_t)m.numseqs);
	for (int t = 0; t < m.numtextures; t++) {
		const studio_texture_t *tx = &m.textures[t];
		char name[64] = {0};
		snprintf(name, sizeof(name), "%.63s", tx->name);
		fwrite(name, 64, 1, out);
		w32(out, (uint32_t)tx->width), w32(out, (uint32_t)tx->height);
		fwrite(tx->pixels, 1, (size_t)tx->width * tx->height, out);
		fwrite(tx->palette, 1, 768, out);
	}
	for (int i = 0; i < numtris; i++) {
		for (int c = 0; c < 3; c++)
			w32(out, (uint32_t)tris[i][c].vert), wf(out, tris[i][c].s), wf(out, tris[i][c].t);
		w32(out, (uint32_t)tritex[i]);
	}

	/* every frame of every sequence, posed */
	{
		float *trs = malloc(sizeof(float) * 10 * (size_t)m.numbones);
		bonemat_t *abs_ = malloc(sizeof(bonemat_t) * (size_t)m.numbones);
		for (int q = 0; q < m.numseqs; q++) {
			const studio_seq_t *sq = &m.seqs[q];
			char label[32] = {0};
			snprintf(label, sizeof(label), "%.31s", sq->label);
			fwrite(label, 32, 1, out);
			wf(out, sq->fps), w32(out, (uint32_t)sq->flags), w32(out, (uint32_t)sq->activity), w32(out, (uint32_t)sq->numframes);
			for (int fr = 0; fr < sq->numframes; fr++) {
				if (!Studio_DecodeFrame(&m, q, 0, fr, trs))
					Studio_RestPose(&m, trs);
				/* the engine takes the root motion out: X 1, Y 2, Z 4 */
				if (sq->motionbone >= 0 && sq->motionbone < m.numbones)
					for (int c = 0; c < 3; c++)
						if (sq->motiontype & (1 << c))
							trs[sq->motionbone * 10 + c] = 0;
				for (int b = 0; b < m.numbones; b++) {
					bonemat_t rel;
					Bone_FromTRS(rel, trs + b * 10);
					if (m.bones[b].parent >= 0)
						Bone_Multiply(abs_[b], abs_[m.bones[b].parent], rel);
					else
						memcpy(abs_[b], rel, sizeof(rel));
				}
				for (int s = 0; s < nsub; s++) {
					const studio_submodel_t *sm = &m.bodyparts[subs[s] / 256].models[subs[s] % 256];
					for (int v = 0; v < sm->numverts; v++) {
						float in[3] = {rdf(sm->verts + v * 12), rdf(sm->verts + v * 12 + 4), rdf(sm->verts + v * 12 + 8)}, o[3];
						Bone_Transform(abs_[sm->vertbone[v]], in, o);
						wf(out, o[0]), wf(out, o[1]), wf(out, o[2]);
					}
				}
			}
		}
		free(trs);
		free(abs_);
	}
	fclose(out);
	printf("%s: %d verts, %d tris, %d textures, %d sequences\n", argv[1], numverts, numtris, m.numtextures, m.numseqs);
	return 0;
}
