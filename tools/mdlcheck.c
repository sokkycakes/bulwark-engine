/* mdlcheck - loads a studio model and any appended .seq files with the
 * engine's parser, validates every sequence, decodes every frame and counts
 * triangles. With --fuzz it also feeds truncated and byte-flipped copies to
 * the parser, which must reject or accept them without reading out of
 * bounds (build with -fsanitize=address to check that).
 *
 *   mdlcheck model.mdl [model1.seq ...] [--fuzz N] [--quiet]
 *
 * Exit status: 0 all checks passed, 1 a check failed, 2 usage/IO error.
 * ISC licence, see LICENSE. */
#include "../src/common/studio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *readfile(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
	if (n < 0 || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

/* parse, append, validate, decode; returns the number of sequences that
 * validated and decoded, or -1 if the model or a .seq was rejected */
static int run(const uint8_t *mdl, size_t mlen, uint8_t **seqs, size_t *slen, int nseq, bool report,
               int *tris_out)
{
	studio_model_t m;
	char err[256];
	if (!Studio_Parse(&m, mdl, mlen, err, sizeof(err))) {
		if (report)
			printf("REJECT model: %s\n", err);
		return -1;
	}
	int base = m.numseqs;
	for (int i = 0; i < nseq; i++)
		if (!Studio_AppendSeq(&m, seqs[i], slen[i], err, sizeof(err))) {
			if (report)
				printf("REJECT seq %d: %s\n", i + 1, err);
			Studio_Free(&m);
			return -1;
		}
	int good = 0, tris = 0;
	float *pose = malloc(sizeof(float) * 10 * m.numbones);
	for (int s = 0; s < m.numseqs; s++) {
		const studio_seq_t *q = &m.seqs[s];
		bool ok = Studio_ValidateSeq(&m, s, err, sizeof(err));
		for (int b = 0; ok && b < q->numblends; b++)
			for (int f = 0; ok && f < q->numframes; f++) {
				if (!Studio_DecodeFrame(&m, s, b, f, pose)) {
					snprintf(err, sizeof(err), "decode failed blend %d frame %d", b, f);
					ok = false;
					break;
				}
				for (int k = 0; k < 10 * m.numbones; k++)
					if (!isfinite(pose[k])) {
						snprintf(err, sizeof(err), "non-finite pose value frame %d", f);
						ok = false;
						break;
					}
			}
		if (ok)
			good++;
		if (report && (!ok || s >= base))
			printf("%s seq %3d %-32s src %d frames %3d fps %5.1f blends %d%s%s\n", ok ? "ok  " : "FAIL", s,
			       q->label, q->source, q->numframes, q->fps, q->numblends, ok ? "" : ": ", ok ? "" : err);
	}
	for (int i = 0; i < m.numbodyparts; i++)
		for (int j = 0; j < m.bodyparts[i].nummodels; j++) {
			const studio_submodel_t *sm = &m.bodyparts[i].models[j];
			for (int k = 0; k < sm->nummeshes; k++)
				tris += Studio_CountTris(&m, &sm->meshes[k], sm->numverts, sm->numnorms);
		}
	if (report) {
		printf("model %s: bones %d hitboxes %d seqs %d (model %d + appended %d) seqgroups %d textures %d "
		       "skins %dx%d bodyparts %d attachments %d tris %d\n",
		       m.name, m.numbones, m.numhitboxes, m.numseqs, base, m.numseqs - base, m.numseqgroups,
		       m.numtextures, m.numskinfamilies, m.numskinref, m.numbodyparts, m.numattachments, tris);
		for (int i = 0; i < m.numhitboxes; i++)
			printf("  hitbox %2d bone %2d (%s) group %d\n", i, m.hitboxes[i].bone,
			       m.bones[m.hitboxes[i].bone].name, m.hitboxes[i].group);
		for (int i = 0; i < m.numattachments; i++)
			printf("  attachment %d bone %d (%s)\n", i, m.attachments[i].bone, m.bones[m.attachments[i].bone].name);
	}
	if (tris_out)
		*tris_out = tris;
	free(pose);
	int total = m.numseqs;
	Studio_Free(&m);
	return good == total ? good : -2 - (total - good);
}

static uint32_t rng = 2463534242u;
static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

int main(int argc, char **argv)
{
	const char *mdlpath = NULL;
	uint8_t *seqs[16];
	size_t slen[16];
	int nseq = 0, fuzz = 0;
	bool quiet = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fuzz") && i + 1 < argc)
			fuzz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--quiet"))
			quiet = true;
		else if (!mdlpath)
			mdlpath = argv[i];
		else if (nseq < 16) {
			if (!(seqs[nseq] = readfile(argv[i], &slen[nseq]))) {
				fprintf(stderr, "cannot read %s\n", argv[i]);
				return 2;
			}
			nseq++;
		}
	}
	size_t mlen;
	uint8_t *mdl = mdlpath ? readfile(mdlpath, &mlen) : NULL;
	if (!mdl) {
		fprintf(stderr, "usage: mdlcheck model.mdl [model1.seq ...] [--fuzz N] [--quiet]\n");
		return 2;
	}
	int tris = 0;
	int r = run(mdl, mlen, seqs, slen, nseq, !quiet, &tris);
	printf("RESULT %s: %s\n", mdlpath, r >= 0 ? "pass" : r == -1 ? "rejected" : "sequence failures");
	int status = r >= 0 ? 0 : 1;

	if (fuzz > 0) {
		/* truncation: every prefix length on a coarse grid plus the header */
		int rej = 0, acc = 0;
		for (size_t cut = 0; cut < mlen; cut += cut < 512 ? 1 : (mlen / fuzz + 1)) {
			uint8_t *t = malloc(cut ? cut : 1);
			memcpy(t, mdl, cut);
			if (run(t, cut, seqs, slen, nseq, false, NULL) == -1)
				rej++;
			else
				acc++;
			free(t);
		}
		printf("FUZZ truncate-model: %d rejected, %d accepted\n", rej, acc);
		if (acc) /* the header length must equal the file size, so no prefix may load */
			status = 1;
		for (int i = 0; i < nseq; i++) {
			rej = acc = 0;
			for (size_t cut = 0; cut < slen[i]; cut += cut < 512 ? 1 : (slen[i] / fuzz + 1)) {
				uint8_t *t = malloc(cut ? cut : 1), *keep = seqs[i];
				size_t keeplen = slen[i];
				memcpy(t, keep, cut);
				seqs[i] = t, slen[i] = cut;
				if (run(mdl, mlen, seqs, slen, nseq, false, NULL) == -1)
					rej++;
				else
					acc++;
				seqs[i] = keep, slen[i] = keeplen;
				free(t);
			}
			printf("FUZZ truncate-seq%d: %d rejected, %d accepted\n", i + 1, rej, acc);
			if (acc)
				status = 1;
		}
		/* random corruption: must not crash; outcomes are only counted */
		int out[3] = {0};
		for (int it = 0; it < fuzz; it++) {
			uint8_t *t = malloc(mlen);
			memcpy(t, mdl, mlen);
			int flips = 1 + xorshift() % 8;
			for (int k = 0; k < flips; k++)
				t[xorshift() % mlen] ^= (uint8_t)(1 + xorshift() % 255);
			int rr = run(t, mlen, seqs, slen, nseq, false, NULL);
			out[rr >= 0 ? 0 : rr == -1 ? 1 : 2]++;
			free(t);
		}
		printf("FUZZ flip-model x%d: %d loaded clean, %d rejected, %d loaded with sequence failures\n", fuzz,
		       out[0], out[1], out[2]);
		for (int i = 0; i < nseq; i++) {
			int o2[3] = {0};
			for (int it = 0; it < fuzz; it++) {
				uint8_t *t = malloc(slen[i]), *keep = seqs[i];
				memcpy(t, keep, slen[i]);
				int flips = 1 + xorshift() % 8;
				for (int k = 0; k < flips; k++)
					t[xorshift() % slen[i]] ^= (uint8_t)(1 + xorshift() % 255);
				seqs[i] = t;
				int rr = run(mdl, mlen, seqs, slen, nseq, false, NULL);
				o2[rr >= 0 ? 0 : rr == -1 ? 1 : 2]++;
				seqs[i] = keep;
				free(t);
			}
			printf("FUZZ flip-seq%d x%d: %d loaded clean, %d rejected, %d loaded with sequence failures\n", i + 1,
			       fuzz, o2[0], o2[1], o2[2]);
		}
	}
	free(mdl);
	for (int i = 0; i < nseq; i++)
		free(seqs[i]);
	return status;
}
