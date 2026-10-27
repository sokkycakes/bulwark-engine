/*
 * gdidump: run the gdifont module over the same grid as tools/gdiref/gdiref.c
 * and write the same JSONL files, so tools/gdiref/compare.py can score it
 * against a Windows capture.
 *
 * usage: gdidump [--ppem-offset N] [--tune key=value,...] <fonts_dir> <outdir>
 *
 * --ppem-offset renders N ems away from the size rule's choice (for
 * tools/gdiref/best_ppem.py). --tune overrides gdifont_tuning_t fields
 * (see gdifont.h) to score
 * alternatives against Windows.
 *
 * Font files are looked up as <fonts_dir>/<Face>_<weight>.ttf (or .ttc), the
 * names gdiref gives the files it captures with GetFontData. They are copied
 * to <outdir>/fonts so compare.py can show both sides used the same file.
 */
#include "../../src/client/gdifont.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#define mkdir_p(p) mkdir(p, 0777)
#endif

static const char *g_faces[] = { "Tahoma", "Verdana", "Arial", "Courier New", "Trebuchet MS", "Times New Roman", "Calibri" };
static const int g_weights[] = { 400, 700 };
static gdifont_tuning_t g_tune;
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void put_b64(FILE *f, const unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i + 2 < n; i += 3) {
		unsigned v = (p[i] << 16) | (p[i+1] << 8) | p[i+2];
		fputc(b64tab[v >> 18], f); fputc(b64tab[(v >> 12) & 63], f);
		fputc(b64tab[(v >> 6) & 63], f); fputc(b64tab[v & 63], f);
	}
	if (n - i == 1) {
		unsigned v = p[i] << 16;
		fputc(b64tab[v >> 18], f); fputc(b64tab[(v >> 12) & 63], f); fputs("==", f);
	} else if (n - i == 2) {
		unsigned v = (p[i] << 16) | (p[i+1] << 8);
		fputc(b64tab[v >> 18], f); fputc(b64tab[(v >> 12) & 63], f);
		fputc(b64tab[(v >> 6) & 63], f); fputc('=', f);
	}
}

static void put_jstr(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') fputc('\\', f);
		if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
		else fputc(*s, f);
	}
	fputc('"', f);
}

static int codepoints(unsigned *out)
{
	int n = 0;
	unsigned c;
	for (c = 0x20; c <= 0x7E; c++) out[n++] = c;
	for (c = 0xA0; c <= 0xFF; c++) out[n++] = c;
	for (c = 0x410; c <= 0x44F; c++) out[n++] = c;
	out[n++] = 0x0401; out[n++] = 0x0451; out[n++] = 0x2014; out[n++] = 0x2026;
	out[n++] = 0x20AC; out[n++] = 0x2122;
	return n;
}

static unsigned char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = n > 0 ? malloc(n) : NULL;
	if (!buf || fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static void put_glyph(FILE *f, const char *name, const gdifont_glyph_t *g, int ok, int gray)
{
	fprintf(f, ",\"%s\":{\"ret\":%d,\"bbx\":%d,\"bby\":%d,\"ox\":%d,\"oy\":%d", name,
		ok ? g->size : -1, g->bbx, g->bby, g->ox, g->oy);
	if (gray)
		fprintf(f, ",\"incx\":%d,\"incy\":%d", g->incx, g->incy);
	if (ok && g->size > 0) {
		fputs(",\"data\":\"", f);
		put_b64(f, g->data, g->size);
		fputc('"', f);
	}
	fputc('}', f);
}

static void run_config(FILE *gl, FILE *cf, const char *face, const unsigned char *file, size_t len,
	int tall, int weight, int aa, const unsigned *cps, int ncp)
{
	const char *err = NULL;
	gdifont_t *font = gdifont_create_memory_ex(file, len, tall, weight, 0,
		aa ? GDIFONT_ANTIALIASED : GDIFONT_NONANTIALIASED, &g_tune, &err);
	const gdifont_metrics_t *tm;
	const int pad = 8;
	int dibw, dibh, i;
	unsigned char *dib;
	static const char *tables[] = { "gasp", "EBLC", "EBDT", "hdmx", "LTSH", "VDMX", "fpgm" };

	if (!font) {
		fprintf(stderr, "gdidump: %s %d: %s\n", face, weight, err ? err : "failed");
		return;
	}
	tm = gdifont_metrics(font);
	fputs("{\"face\":", cf); put_jstr(cf, face);
	fprintf(cf, ",\"tall\":%d,\"weight\":%d,\"aa\":%d,\"realface\":", tall, weight, aa);
	put_jstr(cf, gdifont_family(font));
	fprintf(cf, ",\"tmHeight\":%d,\"tmAscent\":%d,\"tmDescent\":%d,\"tmInternalLeading\":%d,\"tmExternalLeading\":%d,"
		"\"tmAveCharWidth\":%d,\"tmMaxCharWidth\":%d,\"tmWeight\":%d,\"tmOverhang\":%d,\"ppem\":%d,\"tables\":{",
		tm->height, tm->ascent, tm->descent, tm->internal_leading, tm->external_leading,
		tm->ave_char_width, tm->max_char_width, tm->weight, tm->overhang, tm->ppem);
	for (i = 0; i < 7; i++)
		fprintf(cf, "%s\"%s\":%ld", i ? "," : "", tables[i], gdifont_table_size(font, tables[i]));
	fputs("}}\n", cf);

	/* the same DIB gdiref draws into */
	dibw = tm->max_char_width * 2 + pad * 2;
	dibh = tm->height + pad * 2;
	dib = malloc((size_t)dibw * dibh);

	for (i = 0; i < ncp; i++) {
		gdifont_abc_t abc = { 0, 0, 0 };
		gdifont_glyph_t g;
		int abcok = gdifont_abc(font, cps[i], &abc), ok, x, y;
		int minx = dibw, miny = dibh, maxx = -1, maxy = -1;

		fputs("{\"face\":", gl); put_jstr(gl, face);
		fprintf(gl, ",\"tall\":%d,\"weight\":%d,\"aa\":%d,\"cp\":%u,\"abcok\":%d,\"a\":%d,\"b\":%d,\"c\":%d",
			tall, weight, aa, cps[i], abcok, abc.a, abc.b, abc.c);

		ok = gdifont_glyph_gray8(font, cps[i], &g);
		put_glyph(gl, "gray8", &g, ok, 1);
		gdifont_glyph_release(&g);
		ok = gdifont_glyph_mono(font, cps[i], &g);
		put_glyph(gl, "mono", &g, ok, 0);
		gdifont_glyph_release(&g);

		/* ExtTextOut with the pen at (pad - A, pad), cropped to the ink */
		memset(dib, 0, (size_t)dibw * dibh);
		gdifont_text_out(font, pad - abc.a, pad, &cps[i], 1, dib, dibw, dibh, dibw);
		for (y = 0; y < dibh; y++)
			for (x = 0; x < dibw; x++)
				if (dib[y * dibw + x]) {
					if (x < minx) minx = x;
					if (x > maxx) maxx = x;
					if (y < miny) miny = y;
					if (y > maxy) maxy = y;
				}
		fprintf(gl, ",\"eto\":{\"pad\":%d,\"colour\":0", pad);
		if (maxx >= 0) {
			int w = maxx - minx + 1, h = maxy - miny + 1;
			unsigned char *crop = malloc((size_t)w * h * 3), *q = crop;
			for (y = miny; y <= maxy; y++)
				for (x = minx; x <= maxx; x++) {
					unsigned char v = dib[y * dibw + x];
					*q++ = v; *q++ = v; *q++ = v;
				}
			fprintf(gl, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"rgb\":\"", minx - pad, miny - pad, w, h);
			put_b64(gl, crop, (size_t)w * h * 3);
			fputc('"', gl);
			free(crop);
		}
		fputs("}}\n", gl);
	}
	free(dib);
	gdifont_free(font);
}

int main(int argc, char **argv)
{
	char path[4096];
	unsigned cps[512];
	int ncp, fi, wi, t, aa, found = 0;
	FILE *gl, *cf, *env;

	g_tune = gdifont_default_tuning;
	while (argc >= 5 && !strncmp(argv[1], "--", 2)) {
		if (!strcmp(argv[1], "--ppem-offset"))
			g_tune.ppem_delta = atoi(argv[2]);
		else if (!strcmp(argv[1], "--tune")) {
			char *opts = argv[2], *kv;
			for (kv = strtok(opts, ","); kv; kv = strtok(NULL, ",")) {
				char *eq = strchr(kv, '=');
				int v = eq ? atoi(eq + 1) : 1;
				if (eq) *eq = 0;
				if (!strcmp(kv, "gray_method")) g_tune.gray_method = v;
				else if (!strcmp(kv, "gray_hint")) g_tune.gray_hint = v;
				else if (!strcmp(kv, "sample_phase")) g_tune.sample_phase = v;
				else if (!strcmp(kv, "nogridfit")) g_tune.nogridfit = v;
				else if (!strcmp(kv, "aa_measure")) g_tune.aa_measure = v;
				else if (!strcmp(kv, "sample_nudge")) g_tune.sample_nudge = v;
				else if (!strcmp(kv, "mono_method")) g_tune.mono_method = v;
				else if (!strcmp(kv, "v40_below")) g_tune.v40_below = v;
				else { fprintf(stderr, "gdidump: unknown tuning %s\n", kv); return 2; }
			}
		} else
			break;
		argv += 2;
		argc -= 2;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: gdidump [--ppem-offset N] [--tune k=v,...] <fonts_dir> <outdir>\n");
		return 2;
	}
	mkdir_p(argv[2]);
	snprintf(path, sizeof(path), "%s/fonts", argv[2]);
	mkdir_p(path);

	snprintf(path, sizeof(path), "%s/env.json", argv[2]);
	env = fopen(path, "w");
	snprintf(path, sizeof(path), "%s/glyphs.jsonl", argv[2]);
	gl = fopen(path, "w");
	snprintf(path, sizeof(path), "%s/configs.jsonl", argv[2]);
	cf = fopen(path, "w");
	if (!env || !gl || !cf) {
		fprintf(stderr, "gdidump: cannot write to %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	fprintf(env, "{\"os\":\"gdifont\",\"wine\":\"\",\"fontsmoothing\":0,\"smoothingtype\":0,\"contrast\":0}\n");
	fclose(env);
	ncp = codepoints(cps);

	for (fi = 0; fi < (int)(sizeof(g_faces) / sizeof(g_faces[0])); fi++)
		for (wi = 0; wi < 2; wi++) {
			static const char *exts[] = { "ttf", "ttc" };
			unsigned char *file = NULL;
			size_t len = 0;
			int e;
			for (e = 0; e < 2 && !file; e++) {
				snprintf(path, sizeof(path), "%s/%s_%d.%s", argv[1], g_faces[fi], g_weights[wi], exts[e]);
				file = read_file(path, &len);
				if (file) {
					FILE *copy;
					snprintf(path, sizeof(path), "%s/fonts/%s_%d.%s", argv[2], g_faces[fi], g_weights[wi], exts[e]);
					if ((copy = fopen(path, "wb"))) {
						fwrite(file, 1, len, copy);
						fclose(copy);
					}
				}
			}
			if (!file)
				continue;
			found++;
			for (t = 8; t <= 24; t++)
				for (aa = 0; aa <= 1; aa++)
					run_config(gl, cf, g_faces[fi], file, len, t, g_weights[wi], aa, cps, ncp);
			free(file);
		}
	fclose(gl);
	fclose(cf);
	if (!found) {
		fprintf(stderr, "gdidump: no <Face>_<weight>.ttf files in %s\n", argv[1]);
		return 1;
	}
	return 0;
}
