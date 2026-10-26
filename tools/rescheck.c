/* rescheck - parses VGUI .res layout and scheme files with the engine's
 * KeyValues reader and reports what it found and what it had to repair.
 *
 *   rescheck [options] file.res ...
 *     --strict        fail on anything that needs a recovery
 *     --span          let quoted strings continue past a newline
 *     --dump          print the tree in a canonical form, one entry per line
 *     --scheme        also load each file as a scheme and resolve its colours
 *     --fallback F    with --scheme, resolve missing colour names in scheme F
 *     --layout W H S  print panel rectangles for a W x H parent, scale S
 *     --fuzz N        feed truncated and N randomly corrupted copies of each
 *                     file to the parser (build with -fsanitize=address)
 *     --quiet         print failures and the summary only
 *
 * Canonical dump: "<depth> <key> = <value>" or "<depth> <key> {", with
 * bytes outside printable ASCII, '\\' and '=' written as \xNN. It exists so
 * an independent implementation can be compared line by line.
 *
 * Exit status: 0 every file parsed, 1 a file failed or a fuzz invariant
 * broke, 2 usage/IO error. ISC licence, see LICENSE. */
#include "../src/common/vgui_res.h"

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

static void putesc(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p < 32 || *p > 126 || *p == '\\' || *p == '=')
			printf("\\x%02x", *p);
		else
			putchar(*p);
	}
}

static void dump(const kv_t *kv, int depth)
{
	for (; kv; kv = kv->next) {
		printf("%d ", depth);
		putesc(kv->key);
		if (kv->value) {
			printf(" = ");
			putesc(kv->value);
			printf("\n");
		} else {
			printf(" {\n");
			dump(kv->child, depth + 1);
		}
	}
}

/* walks the tree and checks what the parser promised: returns false if a
 * count or limit disagrees */
static bool walk(const kv_t *kv, int depth, int *nodes, int *maxdepth)
{
	for (; kv; kv = kv->next) {
		(*nodes)++;
		if (strlen(kv->key) > KV_MAX_TOKEN || (kv->value && strlen(kv->value) > KV_MAX_TOKEN))
			return false;
		if (kv->value && kv->child)
			return false;
		if (!kv->value) {
			if (depth + 1 > *maxdepth)
				*maxdepth = depth + 1;
			if (!walk(kv->child, depth + 1, nodes, maxdepth))
				return false;
		}
	}
	return true;
}

static bool invariants(const kv_t *kv, const kv_report_t *rep)
{
	int nodes = 0, maxdepth = 0;
	if (!walk(kv, 0, &nodes, &maxdepth))
		return false;
	return nodes == rep->nodes && maxdepth == rep->maxdepth && maxdepth <= KV_MAX_DEPTH;
}

static int recoveries(const kv_report_t *r)
{
	return r->string_at_newline + r->string_at_eof + r->key_without_value + r->block_without_key + r->stray_close +
	       r->unclosed_blocks;
}

/* one parse of a buffer, strict and lenient; returns 0 clean, 1 repaired,
 * 2 rejected, -1 invariant broken */
static int probe(const uint8_t *b, size_t len, unsigned flags)
{
	kv_report_t rep;
	kv_t *kv = KV_Parse(b, len, flags, &rep);
	int r = kv ? (recoveries(&rep) ? 1 : 0) : 2;
	if (kv && !invariants(kv, &rep))
		r = -1;
	/* strict must accept exactly the inputs lenient parses with no repair */
	kv_report_t srep;
	kv_t *skv = KV_Parse(b, len, flags | KV_STRICT, &srep);
	if ((skv != NULL) != (r == 0 && kv != NULL))
		r = -1;
	KV_Free(skv);
	KV_Free(kv);
	return r;
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static int fuzzfile(const char *path, const uint8_t *b, size_t len, int n, unsigned flags, int counts[4])
{
	int broken = 0;
	size_t step = len / 256 + 1;
	for (size_t cut = 0; cut < len; cut += step) {
		int r = probe(b, cut, flags);
		if (r < 0) {
			printf("FUZZ-BROKEN %s prefix %zu\n", path, cut);
			broken++;
		} else {
			counts[r]++;
		}
	}
	static const uint8_t special[] = { '"', '{', '}', '/', '\n', 0, '\\', ' ', 0xff };
	uint8_t *m = malloc(len + 16);
	for (int it = 0; it < n; it++) {
		memcpy(m, b, len);
		size_t mlen = len;
		int edits = 1 + rnd() % 4;
		for (int e = 0; e < edits && mlen; e++) {
			size_t at = rnd() % mlen;
			switch (rnd() % 3) {
			case 0:
				m[at] = (uint8_t)rnd();
				break;
			case 1:
				m[at] = special[rnd() % sizeof(special)];
				break;
			default:
				memmove(m + at, m + at + 1, mlen - at - 1);
				mlen--;
				break;
			}
		}
		int r = probe(m, mlen, flags);
		if (r < 0) {
			printf("FUZZ-BROKEN %s mutation %d\n", path, it);
			broken++;
		} else {
			counts[r]++;
		}
	}
	free(m);
	return broken;
}

static vgui_scheme_t fallback;

static void schemecheck(const char *path, const uint8_t *b, size_t len)
{
	vgui_scheme_t s;
	kv_report_t rep;
	if (!VGUI_LoadScheme(&s, b, len, &rep)) {
		printf("SCHEME-FAIL %s: %s\n", path, rep.err);
		return;
	}
	if (fallback.kv)
		s.fallback = &fallback;
	int colors = 0, badcolors = 0, base = 0, basecolor = 0, defs = 0;
	uint8_t c[4];
	if (s.colors)
		for (const kv_t *kv = s.colors->child; kv; kv = kv->next)
			if (kv->value)
				colors++, badcolors += !VGUI_ParseColor(kv->value, c);
	if (s.base)
		for (const kv_t *kv = s.base->child; kv; kv = kv->next)
			if (kv->value)
			{
				base++;
				if (VGUI_SchemeColor(&s, kv->key, c))
					basecolor++;
				else
					printf("  not a colour: %s = %s\n", kv->key, kv->value);
			}
	for (int i = 0; i < s.numfonts; i++)
		defs += s.fonts[i].numdefs;
	printf("SCHEME %s colors=%d unparsed=%d base=%d base-as-color=%d fonts=%d defs=%d fontfiles=%d borders=%s\n", path,
	       colors, badcolors, base, basecolor, s.numfonts, defs, s.fontfiles ? (int)(s.fontfiles->child != NULL) : 0,
	       s.borders ? "yes" : "no");
	VGUI_FreeScheme(&s);
}

static void layout(const kv_t *kv, int w, int h, float scale)
{
	const kv_t *root = kv;
	while (root && !KV_IsBlock(root))
		root = root->next;
	vgui_panel_t p[512];
	int n = VGUI_LayoutPanels(root, w, h, scale, p, 512);
	for (int i = 0; i < n && i < 512; i++)
		printf("  %-32s %-20s %5d %5d %5d %5d z%d%s%s%s\n", p[i].name, p[i].control, p[i].x, p[i].y, p[i].w, p[i].h,
		       p[i].z, p[i].visible ? "" : " hidden", p[i].enabled ? "" : " disabled", p[i].badcoord ? " BADCOORD" : "");
	if (n > 512)
		printf("  ... %d more\n", n - 512);
}

int main(int argc, char **argv)
{
	unsigned flags = 0;
	bool dodump = false, quiet = false, scheme = false;
	int fuzz = 0, lw = 0, lh = 0;
	float ls = 1;
	int nfiles = 0, ok = 0, repaired = 0, failed = 0, broken = 0;
	int fc[4] = { 0 };
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--strict"))
			flags |= KV_STRICT;
		else if (!strcmp(argv[i], "--span"))
			flags |= KV_SPAN_LINES;
		else if (!strcmp(argv[i], "--dump"))
			dodump = true;
		else if (!strcmp(argv[i], "--quiet"))
			quiet = true;
		else if (!strcmp(argv[i], "--scheme"))
			scheme = true;
		else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc)
			fuzz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fallback") && i + 1 < argc) {
			size_t flen;
			uint8_t *fb = readfile(argv[++i], &flen);
			if (!fb || !VGUI_LoadScheme(&fallback, fb, flen, NULL)) {
				fprintf(stderr, "can't load fallback scheme %s\n", argv[i]);
				return 2;
			}
			free(fb);
		}
		else if (!strcmp(argv[i], "--layout") && i + 3 < argc) {
			lw = atoi(argv[++i]);
			lh = atoi(argv[++i]);
			ls = (float)atof(argv[++i]);
		} else if (argv[i][0] == '-' && argv[i][1] == '-') {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fuzz") || !strcmp(argv[i], "--fallback")) {
			i++;
			continue;
		}
		if (!strcmp(argv[i], "--layout")) {
			i += 3;
			continue;
		}
		if (argv[i][0] == '-' && argv[i][1] == '-')
			continue;
		size_t len;
		uint8_t *b = readfile(argv[i], &len);
		if (!b) {
			fprintf(stderr, "can't read %s\n", argv[i]);
			return 2;
		}
		nfiles++;
		kv_report_t rep;
		kv_t *kv = KV_Parse(b, len, flags, &rep);
		if (!kv) {
			printf("FAIL %s: %s\n", argv[i], rep.err);
			failed++;
		} else {
			int rec = recoveries(&rep);
			rec ? repaired++ : ok++;
			if (!invariants(kv, &rep)) {
				printf("BROKEN %s: tree disagrees with report\n", argv[i]);
				broken++;
			}
			if (!quiet || rec)
				printf("%s %s nodes=%d depth=%d nl=%d eof=%d nokey=%d novalue=%d stray=%d unclosed=%d nul=%d%s%s\n",
				       rec ? "REPAIRED" : "OK", argv[i], rep.nodes, rep.maxdepth, rep.string_at_newline,
				       rep.string_at_eof, rep.block_without_key, rep.key_without_value, rep.stray_close,
				       rep.unclosed_blocks, rep.nul_end, rec ? " first: " : "", rec ? rep.err : "");
			if (dodump)
				dump(kv, 0);
			if (lw > 0)
				layout(kv, lw, lh, ls);
		}
		if (scheme)
			schemecheck(argv[i], b, len);
		if (fuzz > 0)
			broken += fuzzfile(argv[i], b, len, fuzz, flags & ~KV_STRICT, fc);
		KV_Free(kv);
		free(b);
	}
	if (!nfiles) {
		fprintf(stderr, "usage: rescheck [--strict] [--span] [--dump] [--scheme] [--layout W H S] [--fuzz N] "
		                "[--quiet] [--fallback scheme.res] file.res ...\n");
		return 2;
	}
	printf("SUMMARY files=%d clean=%d repaired=%d failed=%d broken=%d\n", nfiles, ok, repaired, failed, broken);
	VGUI_FreeScheme(&fallback);
	if (fuzz > 0)
		printf("FUZZ clean=%d repaired=%d rejected=%d broken=%d\n", fc[0], fc[1], fc[2], broken);
	return failed || broken ? 1 : 0;
}
