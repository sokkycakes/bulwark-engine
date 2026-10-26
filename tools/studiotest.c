/* studiotest - builds small studio models and .seq files in memory, from
 * the layouts in docs/spec/studio-mdl.md, and checks that the parser
 * decodes them as written and rejects broken ones for the right reason.
 * Needs no game content. ISC licence, see LICENSE. */
#include "../src/common/studio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
	uint8_t b[8192];
	size_t n;
} buf_t;

static size_t put(buf_t *w, const void *p, size_t n)
{
	size_t at = w->n;
	memcpy(w->b + w->n, p, n);
	w->n += n;
	return at;
}
static size_t zeros(buf_t *w, size_t n)
{
	size_t at = w->n;
	memset(w->b + w->n, 0, n);
	w->n += n;
	return at;
}
static void wi(buf_t *w, size_t at, int32_t v) { memcpy(w->b + at, &v, 4); } /* little-endian host assumed */
static void wf(buf_t *w, size_t at, float v) { memcpy(w->b + at, &v, 4); }
static void ws(buf_t *w, size_t at, int16_t v) { memcpy(w->b + at, &v, 2); }

/* offsets of interesting fields, for the malformed cases */
typedef struct {
	size_t bone1, seq0, anim0, rle0, mesh0, tris0;
} layout_t;

/* two bones, one sequence of 4 frames whose bone 1 x channel is
 * run-length coded as values 10, 20 then 20 held, scale 0.5; one strip */
static void build_model(buf_t *w, layout_t *L)
{
	memset(w, 0, sizeof(*w));
	zeros(w, 244);
	memcpy(w->b, "IDST", 4);
	wi(w, 4, 10);
	memcpy(w->b + 8, "synthetic.mdl", 13);

	size_t bones = zeros(w, 2 * 112);
	memcpy(w->b + bones, "root", 4);
	wi(w, bones + 32, -1);
	memcpy(w->b + bones + 112, "child", 5);
	wi(w, bones + 112 + 32, 0);
	for (int j = 0; j < 2; j++)
		for (int k = 0; k < 6; k++) {
			wi(w, bones + 112 * j + 40 + 4 * k, -1);
			wf(w, bones + 112 * j + 64 + 4 * k, j == 1 && k == 0 ? 1.0f : 0.0f);
			wf(w, bones + 112 * j + 88 + 4 * k, 0.5f);
		}
	L->bone1 = bones + 112;
	wi(w, 140, 2), wi(w, 144, (int)bones);

	size_t hb = zeros(w, 32);
	wi(w, hb, 1), wi(w, hb + 4, 3);
	wi(w, 156, 1), wi(w, 160, (int)hb);

	size_t sg = zeros(w, 104);
	memcpy(w->b + sg, "default", 7);
	wi(w, 172, 1), wi(w, 176, (int)sg);

	/* animation: 2 bones x 6 channel offsets, then bone 1 channel 0 runs */
	size_t anim = zeros(w, 24);
	size_t rle = zeros(w, 8); /* run: valid 2, total 4, values 10, 20; then padding */
	w->b[rle] = 2, w->b[rle + 1] = 4;
	ws(w, rle + 2, 10), ws(w, rle + 4, 20);
	ws(w, anim + 12, (int16_t)(rle - (anim + 12)));
	L->anim0 = anim, L->rle0 = rle;

	size_t seq = zeros(w, 176);
	memcpy(w->b + seq, "idle", 4);
	wf(w, seq + 32, 30.0f);
	wi(w, seq + 56, 4);
	wi(w, seq + 120, 1);
	wi(w, seq + 124, (int)anim);
	L->seq0 = seq;
	wi(w, 164, 1), wi(w, 168, (int)seq);

	/* one 4x2 texture and its palette */
	size_t tex = zeros(w, 80);
	memcpy(w->b + tex, "skin.bmp", 8);
	wi(w, tex + 68, 4), wi(w, tex + 72, 2);
	size_t px = zeros(w, 8 + 768);
	wi(w, tex + 76, (int)px);
	wi(w, 180, 1), wi(w, 184, (int)tex), wi(w, 188, (int)px);
	size_t skins = zeros(w, 4);
	wi(w, 192, 1), wi(w, 196, 1), wi(w, 200, (int)skins);

	/* geometry: 3 vertices on bone 1, one strip of 3 */
	size_t vinfo = zeros(w, 4);
	w->b[vinfo] = 1, w->b[vinfo + 1] = 1, w->b[vinfo + 2] = 1;
	size_t verts = zeros(w, 36);
	wf(w, verts + 12, 1.0f), wf(w, verts + 28, 1.0f);
	size_t tris = zeros(w, 2 + 3 * 8 + 2);
	ws(w, tris, 3);
	for (int i = 0; i < 3; i++)
		ws(w, tris + 2 + 8 * i, (int16_t)i);
	L->tris0 = tris;
	size_t mesh = zeros(w, 20);
	wi(w, mesh, 1), wi(w, mesh + 4, (int)tris);
	L->mesh0 = mesh;
	size_t sub = zeros(w, 112);
	memcpy(w->b + sub, "body", 4);
	wi(w, sub + 72, 1), wi(w, sub + 76, (int)mesh);
	wi(w, sub + 80, 3), wi(w, sub + 84, (int)vinfo), wi(w, sub + 88, (int)verts);
	wi(w, sub + 92, 3), wi(w, sub + 96, (int)vinfo), wi(w, sub + 100, (int)verts);
	size_t bp = zeros(w, 76);
	memcpy(w->b + bp, "studio", 6);
	wi(w, bp + 64, 1), wi(w, bp + 68, 1), wi(w, bp + 72, (int)sub);
	wi(w, 204, 1), wi(w, 208, (int)bp);

	size_t att = zeros(w, 88);
	wi(w, att + 36, 1);
	wi(w, 212, 1), wi(w, 216, (int)att);
	wi(w, 72, (int)w->n);
}

/* a .seq with one 2-frame sequence: bone 0 channel 2 values 4, 8; animation
 * offsets in the descriptor use an arbitrary origin (5000 maps to byte 8) */
static void build_seq(buf_t *w)
{
	memset(w, 0, sizeof(*w));
	zeros(w, 8);
	size_t anim = zeros(w, 24);
	size_t rle = zeros(w, 8);
	w->b[rle] = 2, w->b[rle + 1] = 2;
	ws(w, rle + 2, 4), ws(w, rle + 4, 8);
	ws(w, anim + 4, (int16_t)(rle - anim));
	size_t seq = zeros(w, 176);
	memcpy(w->b + seq, "extra", 5);
	wf(w, seq + 32, 15.0f);
	wi(w, seq + 56, 2);
	wi(w, seq + 120, 1);
	wi(w, seq + 124, 5000);
	wi(w, 0, 1), wi(w, 4, (int)seq);
}

static bool loads(const buf_t *w, char *err)
{
	studio_model_t m;
	bool ok = Studio_Parse(&m, w->b, w->n, err, 256);
	if (ok)
		Studio_Free(&m);
	return ok;
}

static void expect_reject(const buf_t *w, const char *what, const char *needle)
{
	char err[256] = "";
	bool ok = loads(w, err);
	CHECK(!ok, "%s: expected rejection", what);
	CHECK(ok || strstr(err, needle), "%s: reason \"%s\" lacks \"%s\"", what, err, needle);
}

int main(void)
{
	buf_t w, s;
	layout_t L;
	char err[256];
	studio_model_t m;
	float pose[20];

	build_model(&w, &L);
	CHECK(Studio_Parse(&m, w.b, w.n, err, sizeof(err)), "valid model rejected: %s", err);
	CHECK(m.numbones == 2 && m.numseqs == 1 && m.numtextures == 1 && m.numhitboxes == 1, "counts");
	CHECK(m.hitboxes[0].group == 3 && m.attachments[0].bone == 1, "hitbox/attachment fields");
	CHECK(Studio_ValidateSeq(&m, 0, err, sizeof(err)), "seq validate: %s", err);
	CHECK(Studio_CountTris(&m, &m.bodyparts[0].models[0].meshes[0], 3, 3) == 1, "one triangle");
	const float want[4] = {1 + 10 * 0.5f, 1 + 20 * 0.5f, 1 + 20 * 0.5f, 1 + 20 * 0.5f};
	for (int f = 0; f < 4; f++) {
		CHECK(Studio_DecodeFrame(&m, 0, 0, f, pose), "decode frame %d", f);
		CHECK(fabsf(pose[10] - want[f]) < 1e-6f, "frame %d bone 1 x = %g, want %g", f, pose[10], want[f]);
		CHECK(pose[0] == 0 && pose[6] == 1, "frame %d bone 0 is at rest", f);
	}

	build_seq(&s);
	CHECK(Studio_AppendSeq(&m, s.b, s.n, err, sizeof(err)), "valid .seq rejected: %s", err);
	CHECK(m.numseqs == 2 && m.seqs[1].source == 1 && !strcmp(m.seqs[1].label, "extra"), "appended seq");
	CHECK(Studio_ValidateSeq(&m, 1, err, sizeof(err)), "appended validate: %s", err);
	CHECK(Studio_DecodeFrame(&m, 1, 0, 1, pose) && fabsf(pose[2] - 4.0f) < 1e-6f, "appended frame 1 z = %g", pose[2]);
	CHECK(!strcmp(m.seqs[0].label, "idle"), "model sequences keep their order");

	/* malformed .seq files */
	buf_t t = s;
	t.n -= 1;
	CHECK(!Studio_AppendSeq(&m, t.b, t.n, err, sizeof(err)) && strstr(err, "EOF"), "short .seq: %s", err);
	{
		/* a second sequence pointing before the data region */
		buf_t u;
		memset(&u, 0, sizeof(u));
		put(&u, s.b, s.n);
		size_t d2 = zeros(&u, 176);
		memcpy(u.b + d2, u.b + d2 - 176, 176);
		wi(&u, d2 + 124, 4000);
		wi(&u, 0, 2);
		CHECK(!Studio_AppendSeq(&m, u.b, u.n, err, sizeof(err)) && strstr(err, "outside"), "seq anim before region: %s", err);
	}
	t = s;
	t.b[8 + 24 + 1] = 0; /* run total 0 */
	CHECK(Studio_AppendSeq(&m, t.b, t.n, err, sizeof(err)), "zero run still loads (checked later)");
	CHECK(!Studio_ValidateSeq(&m, m.numseqs - 1, err, sizeof(err)) && strstr(err, "bad run"), "zero run: %s", err);
	CHECK(!Studio_DecodeFrame(&m, m.numseqs - 1, 0, 0, pose), "zero run must not decode");
	Studio_Free(&m);

	/* malformed models */
	t = w, wi(&t, 72, 1);
	expect_reject(&t, "length mismatch", "length");
	t = w, wi(&t, 4, 11);
	expect_reject(&t, "version 11", "version");
	t = w, wi(&t, 140, 0);
	expect_reject(&t, "no bones", "bones");
	t = w, wi(&t, L.bone1 + 32, 1);
	expect_reject(&t, "self parent", "parent");
	t = w, wi(&t, 144, (int)t.n - 100);
	expect_reject(&t, "bone table past end", "past the end");
	t = w, wi(&t, L.seq0 + 56, 0);
	expect_reject(&t, "zero frames", "frames");
	t = w, wi(&t, L.seq0 + 124, (int)t.n - 4);
	expect_reject(&t, "anim past end", "animation");
	t = w, wi(&t, L.seq0 + 156, 1);
	expect_reject(&t, "seqgroup out of range", "group");
	t = w, ws(&t, L.tris0 + 2, 7);
	expect_reject(&t, "tri vertex out of range", "triangle");
	t = w, ws(&t, L.tris0 + 2 + 3 * 8, 1);
	expect_reject(&t, "unterminated tri list", "triangle");
	t = w, wi(&t, 160, -5);
	expect_reject(&t, "negative hitbox index", "hitbox");
	for (size_t cut = 0; cut < w.n; cut++) {
		t = w, t.n = cut;
		CHECK(!loads(&t, err), "prefix of %zu bytes loaded", cut);
	}

	printf("studiotest: %s (%d failures)\n", failures ? "FAIL" : "pass", failures);
	return failures ? 1 : 0;
}
