/* vguitest - checks the KeyValues reader and the VGUI scheme and layout
 * helpers against small inputs written here, each with a known answer.
 * No game content is needed. Exit status: 0 pass, 1 a check failed.
 * ISC licence, see LICENSE. */
#include "../src/common/vgui_res.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			failures++;                                                      \
		}                                                                    \
	} while (0)

static kv_t *parse(const char *text, unsigned flags, kv_report_t *rep)
{
	return KV_Parse((const uint8_t *)text, strlen(text), flags, rep);
}

static bool streq(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

static void basics(void)
{
	kv_report_t rep;
	const char *t = "\"Root.res\"\r\n{\r\n\t\"Panel\"\r\n\t{\r\n\t\t\"xpos\"\t\"12\" // note\r\n"
	                "\t\t\"Label\" \"a\\b\"//\"c\"\r\n\t\tbare token\r\n\t}\r\n}\r\n";
	kv_t *kv = parse(t, KV_STRICT, &rep);
	CHECK(kv && streq(kv->key, "Root.res") && !kv->value && !kv->next);
	CHECK(rep.nodes == 5 && rep.maxdepth == 2);
	kv_t *p = KV_Find(kv, "panel");
	CHECK(p && p->line == 3);
	CHECK(KV_GetInt(p, "XPOS", -1) == 12);
	CHECK(streq(KV_GetString(p, "label", NULL), "a\\b")); /* backslash kept, comment after value */
	CHECK(streq(KV_GetString(p, "bare", NULL), "token"));
	CHECK(KV_FindPath(kv, "Panel/xpos") && KV_FindPath(kv, "Panel/xpos")->line == 5);
	CHECK(!KV_FindPath(kv, "Panel/ypos"));
	CHECK(KV_GetInt(p, "missing", 7) == 7 && KV_GetInt(p, "label", 7) == 7);
	KV_Free(kv);

	/* duplicate keys are all kept in order and lookups take the first */
	kv = parse("r { k 1 K 2 }", KV_STRICT, &rep);
	CHECK(kv && KV_GetInt(kv, "k", 0) == 1 && kv->child->next && streq(kv->child->next->value, "2"));
	KV_Free(kv);

	/* several top-level entries, and text ending at a NUL */
	static const uint8_t nul[] = "a { } b \"1\"\n\0garbage {";
	kv = KV_Parse(nul, sizeof(nul) - 1, KV_STRICT, &rep);
	CHECK(kv && streq(kv->key, "a") && kv->next && streq(kv->next->value, "1") && !kv->next->next);
	CHECK(rep.nul_end == 1);
	KV_Free(kv);

	/* empty quoted strings are values, and the empty file has no entries */
	kv = parse("r { \"\" \"\" }", KV_STRICT, &rep);
	CHECK(kv && kv->child && streq(kv->child->key, "") && streq(kv->child->value, ""));
	KV_Free(kv);
	CHECK(!parse("  // nothing\n", 0, &rep) && streq(rep.err, "no entries"));
}

static void recoveries(void)
{
	kv_report_t rep;
	/* a quote left open at end of line closes there */
	const char *t = "r\n{\n\t\"tall\"\t\t\"45\r\n\t\"next\"\t\"1\"\n}\n";
	kv_t *kv = parse(t, 0, &rep);
	CHECK(kv && streq(KV_GetString(kv, "tall", NULL), "45") && KV_GetInt(kv, "next", 0) == 1);
	CHECK(rep.string_at_newline == 1 && rep.errline == 3);
	KV_Free(kv);
	CHECK(!parse(t, KV_STRICT, &rep) && rep.errline == 3);
	/* with KV_SPAN_LINES the string runs on to the next quote instead */
	kv = parse(t, KV_SPAN_LINES, &rep);
	CHECK(kv && KV_GetString(kv, "tall", NULL) && strchr(KV_GetString(kv, "tall", ""), '\n'));
	CHECK(rep.string_at_newline == 0);
	KV_Free(kv);

	/* key with no value before '}' is dropped; the block still closes */
	kv = parse("r { a 1 b } s 2", 0, &rep);
	CHECK(kv && !KV_Find(kv, "b") && KV_GetInt(kv, "a", 0) == 1 && kv->next && streq(kv->next->key, "s"));
	CHECK(rep.key_without_value == 1);
	KV_Free(kv);

	/* stray close, unclosed blocks, a block without a key, a string open at EOF */
	kv = parse("} r { { x 1 } q { z \"open", 0, &rep);
	CHECK(kv && rep.stray_close == 1 && rep.unclosed_blocks == 2 && rep.block_without_key == 1 &&
	      rep.string_at_eof == 1);
	CHECK(kv && kv->child && streq(kv->child->key, "") && KV_GetInt(kv->child, "x", 0) == 1);
	CHECK(streq(KV_GetString(KV_Find(kv, "q"), "z", NULL), "open"));
	KV_Free(kv);
	CHECK(!parse("r { a 1", KV_STRICT, &rep));
}

static void limits(void)
{
	kv_report_t rep;
	char deep[KV_MAX_DEPTH * 4 + 16];
	size_t n = 0;
	for (int i = 0; i <= KV_MAX_DEPTH; i++)
		deep[n++] = 'k', deep[n++] = ' ', deep[n++] = '{';
	deep[n] = 0;
	CHECK(!parse(deep, 0, &rep) && strstr(rep.err, "deeply"));
	n -= 3; /* exactly KV_MAX_DEPTH levels is fine */
	deep[n] = 0;
	kv_t *kv = parse(deep, 0, &rep);
	CHECK(kv && rep.maxdepth == KV_MAX_DEPTH && rep.unclosed_blocks == KV_MAX_DEPTH);
	KV_Free(kv);

	char *big = malloc(KV_MAX_TOKEN + 8);
	big[0] = '"';
	memset(big + 1, 'x', KV_MAX_TOKEN + 1);
	big[KV_MAX_TOKEN + 2] = '"';
	big[KV_MAX_TOKEN + 3] = 0;
	CHECK(!parse(big, 0, &rep) && strstr(rep.err, "too long"));
	big[KV_MAX_TOKEN + 1] = '"'; /* exactly KV_MAX_TOKEN characters */
	big[KV_MAX_TOKEN + 2] = ' ';
	big[KV_MAX_TOKEN + 3] = '1';
	big[KV_MAX_TOKEN + 4] = 0;
	kv = parse(big, KV_STRICT, &rep);
	CHECK(kv && strlen(kv->key) == KV_MAX_TOKEN);
	KV_Free(kv);
	free(big);
}

static void colors(void)
{
	uint8_t c[4];
	CHECK(VGUI_ParseColor("1 2 3 4", c) && c[0] == 1 && c[1] == 2 && c[2] == 3 && c[3] == 4);
	CHECK(VGUI_ParseColor(" 10\t20 30 ", c) && c[2] == 30 && c[3] == 255);
	CHECK(VGUI_ParseColor("300 -5 0 0", c) && c[0] == 255 && c[1] == 0);
	CHECK(!VGUI_ParseColor("1 2", c) && !VGUI_ParseColor("1 2 3 4 5", c) && !VGUI_ParseColor("BaseText", c));
	CHECK(!VGUI_ParseColor("1 2 3x", c) && !VGUI_ParseColor("", c));

	const char *t = "\"Scheme\"\n{\n\"Colors\"\n{\n\"White\" \"255 255 255 255\"\n\"Menu\" { \"BgColor\" \"1 2 3 4\" }\n}\n"
	                "\"BaseSettings\"\n{\n\"FgColor\" \"White\"\n\"Alias\" \"FgColor\"\n\"Loop\" \"Loop\"\n"
	                "\"Lit\" \"9 9 9\"\n\"Path\" \"resource/x\"\n}\n"
	                "\"Fonts\"\n{\n\"Default\"\n{\n\"1\" { \"name\" \"Verdana\" \"tall\" \"12\" \"yres\" \"480 599\" }\n"
	                "\"2\" { \"name\" \"Verdana\" \"tall\" \"14\" \"yres\" \"600 767\" \"antialias\" \"1\" }\n}\n"
	                "\"Plain\" { \"1\" { \"name\" \"Arial\" \"tall\" \"10\" \"dropshadow\" \"1\" } }\n}\n"
	                "\"CustomFontFiles\" { \"1\" \"resource/font/a.ttf\" }\n}\n";
	vgui_scheme_t s;
	kv_report_t rep;
	CHECK(VGUI_LoadScheme(&s, (const uint8_t *)t, strlen(t), &rep));
	CHECK(VGUI_SchemeColor(&s, "White", c) && c[0] == 255);
	CHECK(VGUI_SchemeColor(&s, "Menu/BgColor", c) && c[3] == 4);
	CHECK(VGUI_SchemeColor(&s, "Alias", c) && c[1] == 255); /* BaseSettings -> BaseSettings -> Colors */
	CHECK(VGUI_SchemeColor(&s, "Lit", c) && c[0] == 9 && c[3] == 255);
	CHECK(!VGUI_SchemeColor(&s, "Loop", c) && !VGUI_SchemeColor(&s, "Path", c) && !VGUI_SchemeColor(&s, "Nope", c));
	CHECK(s.numfonts == 2 && s.fontfiles);
	const vgui_font_t *f = VGUI_SchemeFont(&s, "default");
	CHECK(f && f->numdefs == 2 && f->defs[0].yres_min == 480 && f->defs[0].yres_max == 599);
	CHECK(VGUI_PickFontDef(f, 480)->tall == 12 && VGUI_PickFontDef(f, 700)->tall == 14);
	CHECK(VGUI_PickFontDef(f, 700)->flags == VGUI_FONT_ANTIALIAS);
	CHECK(VGUI_PickFontDef(f, 1080)->tall == 14 && VGUI_PickFontDef(f, 300)->tall == 12); /* nearest range */
	const vgui_font_t *pf = VGUI_SchemeFont(&s, "Plain");
	CHECK(pf && VGUI_PickFontDef(pf, 1080)->flags == VGUI_FONT_DROPSHADOW);

	/* a scheme that names colours it does not define finds them in its fallback */
	const char *t2 = "Scheme { Colors { Own \"1 1 1 1\" } BaseSettings { Fg White Mine Own } }";
	vgui_scheme_t s2;
	CHECK(VGUI_LoadScheme(&s2, (const uint8_t *)t2, strlen(t2), &rep));
	CHECK(!VGUI_SchemeColor(&s2, "Fg", c));
	s2.fallback = &s;
	CHECK(VGUI_SchemeColor(&s2, "Fg", c) && c[0] == 255 && VGUI_SchemeColor(&s2, "Mine", c) && c[0] == 1);
	s.fallback = &s2; /* a cycle ends after a few schemes */
	CHECK(!VGUI_SchemeColor(&s2, "Nope", c));
	VGUI_FreeScheme(&s2);
	VGUI_FreeScheme(&s);
	CHECK(!VGUI_LoadScheme(&s, (const uint8_t *)"a 1", 3, &rep));
}

static void layout(void)
{
	int v;
	CHECK(VGUI_ResolveCoord("12", 640, 1, &v) && v == 12);
	CHECK(VGUI_ResolveCoord("-4", 640, 1, &v) && v == -4);
	CHECK(VGUI_ResolveCoord("r50", 640, 1, &v) && v == 590);
	CHECK(VGUI_ResolveCoord("c-100", 640, 1, &v) && v == 220);
	CHECK(VGUI_ResolveCoord("c20", 640, 2, &v) && v == 360);
	CHECK(VGUI_ResolveCoord("10 ", 640, 1.5f, &v) && v == 15);
	CHECK(!VGUI_ResolveCoord("", 640, 1, &v) && !VGUI_ResolveCoord("12px", 640, 1, &v) &&
	      !VGUI_ResolveCoord("x5", 640, 1, &v) && v == 0);

	const char *t = "\"L.res\" { \"L\" { \"ControlName\" \"Frame\" \"fieldName\" \"L\" \"xpos\" \"0\" \"ypos\" \"0\" "
	                "\"wide\" \"640\" \"tall\" \"480\" } \"Btn\" { \"ControlName\" \"Button\" \"xpos\" \"r110\" "
	                "\"ypos\" \"c-10\" \"wide\" \"100\" \"tall\" \"20\" \"visible\" \"0\" \"zpos\" \"3\" } "
	                "\"Bad\" { \"xpos\" \"oops\" } \"stray\" \"1\" }";
	kv_report_t rep;
	kv_t *kv = parse(t, KV_STRICT, &rep);
	vgui_panel_t p[4];
	int n = VGUI_LayoutPanels(kv, 640, 480, 1, p, 4);
	CHECK(n == 3);
	CHECK(streq(p[0].control, "Frame") && p[0].w == 640 && p[0].visible);
	CHECK(streq(p[1].name, "Btn") && p[1].x == 530 && p[1].y == 230 && p[1].z == 3 && !p[1].visible);
	CHECK(p[2].badcoord && p[2].x == 0 && !p[1].badcoord);
	CHECK(VGUI_LayoutPanels(kv, 640, 480, 1, p, 1) == 3); /* counts past max */
	KV_Free(kv);
}

int main(void)
{
	basics();
	recoveries();
	limits();
	colors();
	layout();
	if (failures)
		printf("%d failure(s)\n", failures);
	else
		printf("vguitest: all checks passed\n");
	return failures ? 1 : 0;
}
