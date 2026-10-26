/* Bulwark engine - console utility commands shared by the client and the
 * dedicated server: cvar helpers (toggle, inc, reset, find, help), the "if"
 * and "set_calc" expression evaluator, deferred commands, config files and
 * console logging, and file listings. The command names and behaviour follow
 * the public documentation of the Quake-family and Source consoles.
 * ISC licence, see LICENSE. */
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef BULWARK_VERSION
#define BULWARK_VERSION "0.1"
#endif

cfg_write_fn cfg_write_hook;
cfg_reset_fn cfg_reset_hook;

/* --- cvar helpers ------------------------------------------------------------ */

static cvar_t *need_cvar(const char *name)
{
	cvar_t *v = Cvar_Find(name);
	if (!v)
		Con_Printf("unknown cvar \"%s\"\n", name);
	return v;
}

static void set_number(cvar_t *v, double d)
{
	char buf[64];
	if (d == (long long)d && fabs(d) < 1e15)
		snprintf(buf, sizeof(buf), "%lld", (long long)d);
	else
		snprintf(buf, sizeof(buf), "%g", d);
	Cvar_Set(v->name, buf);
}

/* toggle <cvar> [value1 value2 ...]: flips 0/1, or steps through the values */
static void Toggle_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 2) {
		Con_Printf("usage: toggle <cvar> [values...]\n");
		return;
	}
	if (!(v = need_cvar(argv[1])))
		return;
	if (argc == 2) {
		Cvar_Set(v->name, v->value ? "0" : "1");
		return;
	}
	for (int i = 2; i < argc; i++)
		if (!strcmp(v->string, argv[i])) {
			Cvar_Set(v->name, argv[i + 1 < argc ? i + 1 : 2]);
			return;
		}
	Cvar_Set(v->name, argv[2]);
}

/* inc <cvar> [amount] (Quake), and the Source forms:
 * incrementvar <cvar> <min> <max> <delta>, multvar <cvar> <min> <max> <factor> */
static void Inc_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 2 || !(v = need_cvar(argv[1])))
		return;
	set_number(v, v->value + (argc > 2 ? atof(argv[2]) : 1));
}

static void Incrementvar_f(int argc, const char **argv)
{
	cvar_t *v;
	double mn, mx, d, x;
	if (argc < 5) {
		Con_Printf("usage: %s <cvar> <min> <max> <%s>\n", argv[0], Str_ICmp(argv[0], "multvar") ? "delta" : "factor");
		return;
	}
	if (!(v = need_cvar(argv[1])))
		return;
	mn = atof(argv[2]), mx = atof(argv[3]), d = atof(argv[4]);
	if (!Str_ICmp(argv[0], "multvar")) {
		x = v->value * d;
		x = x > mx ? mx : x < mn ? mn : x;
	} else {
		/* past either end it wraps to the other, as Source's does */
		x = v->value + d;
		if (x > mx + 1e-6)
			x = mn;
		else if (x < mn - 1e-6)
			x = mx;
	}
	set_number(v, x);
}

/* reset/cvarreset/revert <cvar or pattern> */
static void Reset_f(int argc, const char **argv)
{
	int n = 0;
	if (argc < 2) {
		Con_Printf("usage: %s <cvar or pattern>\n", argv[0]);
		return;
	}
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (Str_WildMatch(argv[1], v->name, true) && strcmp(v->string, v->defstring)) {
			Cvar_Reset(v);
			n++;
		}
	if (!strpbrk(argv[1], "*?") && !Cvar_Find(argv[1]))
		Con_Printf("unknown cvar \"%s\"\n", argv[1]);
	else if (strpbrk(argv[1], "*?"))
		Con_Printf("%d cvars reset\n", n);
}

/* cvar_resettodefaults_all, and the _saveonly/_nosaveonly forms */
static void ResetAll_f(int argc, const char **argv)
{
	bool saveonly = strstr(argv[0], "_saveonly") != NULL, nosaveonly = strstr(argv[0], "_nosaveonly") != NULL;
	for (cvar_t *v = Cvar_First(); v; v = v->next) {
		bool saved = (v->flags & CVAR_ARCHIVE) != 0;
		if ((saveonly && !saved) || (nosaveonly && saved))
			continue;
		Cvar_Reset(v);
	}
}

/* differences: every cvar not at its default */
static void Differences_f(int argc, const char **argv)
{
	int n = 0;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (strcmp(v->string, v->defstring) && !(v->flags & CVAR_READONLY)) {
			Con_Printf("%s \"%s\" (default \"%s\")\n", v->name, v->string, v->defstring);
			n++;
		}
	Con_Printf("%d cvars differ from their defaults\n", n);
}

/* cvarwatch <cvar>: print every change to it; again to stop */
static void Cvarwatch_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 2) {
		for (v = Cvar_First(); v; v = v->next)
			if (v->flags & CVAR_WATCHED)
				Con_Printf("watching %s\n", v->name);
		return;
	}
	if (!(v = need_cvar(argv[1])))
		return;
	v->flags ^= CVAR_WATCHED;
	Con_Printf("%s %s\n", v->flags & CVAR_WATCHED ? "watching" : "no longer watching", v->name);
}

/* vstr <cvar>: runs the cvar's text as commands */
static void Vstr_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 2 || !(v = need_cvar(argv[1])))
		return;
	Cbuf_InsertText(v->string);
}

/* --- searching ----------------------------------------------------------------- */

typedef struct {
	const char *needle;
	bool descriptions;
	int count;
} search_t;

static bool contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!hay)
		return false;
	for (; *hay; hay++)
		if (!Str_NICmp(hay, needle, n))
			return true;
	return false;
}

static void search_cmd(const char *name, const char *desc, void *ctx)
{
	search_t *s = ctx;
	if (contains(name, s->needle) || (s->descriptions && contains(desc, s->needle))) {
		Con_Printf("  %-28s %s\n", name, desc ? desc : "(command)");
		s->count++;
	}
}

static void search_alias(const char *name, const char *text, void *ctx)
{
	search_t *s = ctx;
	if (contains(name, s->needle) || (s->descriptions && contains(text, s->needle))) {
		Con_Printf("  %-28s alias: %s\n", name, text);
		s->count++;
	}
}

/* find (Source) and apropos (FTE): names and descriptions holding the text */
static void Find_f(int argc, const char **argv)
{
	search_t s = {0};
	if (argc < 2) {
		Con_Printf("usage: %s <text>\n", argv[0]);
		return;
	}
	s.needle = argv[1];
	s.descriptions = true;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (contains(v->name, s.needle) || contains(v->description, s.needle)) {
			Con_Printf("  %-28s \"%s\" %s\n", v->name, v->string, v->description ? v->description : "");
			s.count++;
		}
	Cmd_ForEach(search_cmd, &s);
	Cmd_ForEachAlias(search_alias, &s);
	Con_Printf("%d matches\n", s.count);
}

/* help <name>: what a cvar, command or alias is */
static void Help_f(int argc, const char **argv)
{
	cvar_t *v;
	const char *d;
	if (argc < 2) {
		Con_Printf("help <name> describes a command or cvar; find <text> searches them.\n"
		           "cmdlist and cvarlist list everything; both take a wildcard pattern.\n");
		return;
	}
	if ((v = Cvar_Find(argv[1]))) {
		Con_Printf("\"%s\" = \"%s\" (default \"%s\")%s%s\n", v->name, v->string, v->defstring,
		           v->flags ? " " : "", v->flags ? Cvar_FlagNames(v->flags) : "");
		if (v->description)
			Con_Printf(" - %s\n", v->description);
		return;
	}
	if (Cmd_Exists(argv[1])) {
		d = Cmd_Description(argv[1]);
		Con_Printf("%s: %s\n", argv[1], d ? d : "command");
		return;
	}
	if ((d = Cmd_AliasText(argv[1]))) {
		Con_Printf("%s is an alias for: %s\n", argv[1], d);
		return;
	}
	Con_Printf("no command or cvar named \"%s\"\n", argv[1]);
}

static void list_alias(const char *name, const char *text, void *ctx)
{
	const char *pattern = ctx;
	if (!pattern || Str_WildMatch(pattern, name, true))
		Con_Printf("%s : %s\n", name, text);
}

static void Aliaslist_f(int argc, const char **argv) { Cmd_ForEachAlias(list_alias, (void *)(argc > 1 ? argv[1] : NULL)); }

/* --- expressions ---------------------------------------------------------------- */

/* A small evaluator for "if" and "set_calc": numbers, quoted or bare
 * strings, $cvar references, ( ), unary ! - ~, * / % + - << >> & | ^,
 * comparisons (== = != <> < > <= >=, and eq ne for strings) and && ||.
 * A value is a string that reads as a number where it can. */

typedef struct {
	bool isnum;
	double n;
	char s[512];
} xval_t;

typedef struct {
	const char *p;
	bool error;
} xparse_t;

static void xnum(xval_t *v, double d)
{
	v->isnum = true;
	v->n = d;
	if (d == (long long)d && fabs(d) < 1e15)
		snprintf(v->s, sizeof(v->s), "%lld", (long long)d);
	else
		snprintf(v->s, sizeof(v->s), "%g", d);
}

static void xstr(xval_t *v, const char *s)
{
	char *end;
	Str_Copy(v->s, s, sizeof(v->s));
	v->n = strtod(v->s, &end);
	v->isnum = end != v->s && !*end;
	if (!v->isnum)
		v->n = 0;
}

static bool xtrue(const xval_t *v) { return v->isnum ? v->n != 0 : v->s[0] != 0; }

static void skipws(xparse_t *x)
{
	while (*x->p && isspace((unsigned char)*x->p))
		x->p++;
}

static bool accept(xparse_t *x, const char *op)
{
	size_t n = strlen(op);
	skipws(x);
	if (strncmp(x->p, op, n))
		return false;
	/* word operators need a break after them */
	if (isalpha((unsigned char)op[0]) && (isalnum((unsigned char)x->p[n]) || x->p[n] == '_'))
		return false;
	x->p += n;
	return true;
}

static void xexpr(xparse_t *x, xval_t *out);

static void xprimary(xparse_t *x, xval_t *out)
{
	char tok[512];
	size_t n = 0;
	skipws(x);
	if (accept(x, "(")) {
		xexpr(x, out);
		if (!accept(x, ")"))
			x->error = true;
		return;
	}
	if (*x->p == '"') {
		x->p++;
		while (*x->p && *x->p != '"' && n + 1 < sizeof(tok))
			tok[n++] = *x->p++;
		if (*x->p == '"')
			x->p++;
		tok[n] = 0;
		xstr(out, tok);
		return;
	}
	if (*x->p == '$') {
		cvar_t *v = NULL;
		bool brace = x->p[1] == '{';
		x->p += brace ? 2 : 1;
		while (*x->p && (isalnum((unsigned char)*x->p) || strchr("_.-/", *x->p)) && n + 1 < sizeof(tok)) {
			if (*x->p == '-' && !brace)
				break;
			tok[n++] = *x->p++;
		}
		tok[n] = 0;
		if (brace && *x->p == '}')
			x->p++;
		v = Cvar_Find(tok);
		xstr(out, v ? v->string : "");
		return;
	}
	if (isdigit((unsigned char)*x->p) || (*x->p == '.' && isdigit((unsigned char)x->p[1]))) {
		char *end;
		double d = strtod(x->p, &end);
		x->p = end;
		xnum(out, d);
		return;
	}
	/* a bare word: a cvar's value if one has that name, else the word */
	while (*x->p && (isalnum((unsigned char)*x->p) || strchr("_./:", *x->p)) && n + 1 < sizeof(tok))
		tok[n++] = *x->p++;
	tok[n] = 0;
	if (!n) {
		x->error = true;
		xstr(out, "");
		return;
	}
	{
		cvar_t *v = Cvar_Find(tok);
		xstr(out, v ? v->string : tok);
	}
}

static void xunary(xparse_t *x, xval_t *out)
{
	if (accept(x, "!")) {
		xunary(x, out);
		xnum(out, !xtrue(out));
	} else if (accept(x, "-")) {
		xunary(x, out);
		xnum(out, -out->n);
	} else if (accept(x, "~")) {
		xunary(x, out);
		xnum(out, (double)~(long long)out->n);
	} else {
		xprimary(x, out);
	}
}

static void xmul(xparse_t *x, xval_t *out)
{
	xval_t r;
	xunary(x, out);
	for (;;) {
		if (accept(x, "*")) {
			xunary(x, &r);
			xnum(out, out->n * r.n);
		} else if (accept(x, "/")) {
			xunary(x, &r);
			xnum(out, r.n ? out->n / r.n : 0);
		} else if (accept(x, "%")) {
			xunary(x, &r);
			xnum(out, r.n ? fmod(out->n, r.n) : 0);
		} else {
			return;
		}
	}
}

static void xadd(xparse_t *x, xval_t *out)
{
	xval_t r;
	xmul(x, out);
	for (;;) {
		if (accept(x, "+")) {
			xmul(x, &r);
			if (out->isnum && r.isnum) {
				xnum(out, out->n + r.n);
			} else { /* strings join */
				char j[1100];
				snprintf(j, sizeof(j), "%s%s", out->s, r.s);
				xstr(out, j);
			}
		} else if (accept(x, "-")) {
			xmul(x, &r);
			xnum(out, out->n - r.n);
		} else {
			return;
		}
	}
}

static void xshift(xparse_t *x, xval_t *out)
{
	xval_t r;
	xadd(x, out);
	for (;;) {
		if (accept(x, "<<")) {
			xadd(x, &r);
			xnum(out, (double)((long long)out->n << ((int)r.n & 63)));
		} else if (accept(x, ">>")) {
			xadd(x, &r);
			xnum(out, (double)((long long)out->n >> ((int)r.n & 63)));
		} else {
			return;
		}
	}
}

static int xcompare(const xval_t *a, const xval_t *b)
{
	if (a->isnum && b->isnum)
		return a->n < b->n ? -1 : a->n > b->n;
	return strcmp(a->s, b->s);
}

static void xrel(xparse_t *x, xval_t *out)
{
	xval_t r;
	xshift(x, out);
	for (;;) {
		int c;
		if (accept(x, "<=")) {
			xshift(x, &r);
			c = xcompare(out, &r) <= 0;
		} else if (accept(x, ">=")) {
			xshift(x, &r);
			c = xcompare(out, &r) >= 0;
		} else if (accept(x, "<>")) {
			xshift(x, &r);
			c = xcompare(out, &r) != 0;
		} else if (accept(x, "<")) {
			xshift(x, &r);
			c = xcompare(out, &r) < 0;
		} else if (accept(x, ">")) {
			xshift(x, &r);
			c = xcompare(out, &r) > 0;
		} else {
			return;
		}
		xnum(out, c);
	}
}

static void xeq(xparse_t *x, xval_t *out)
{
	xval_t r;
	xrel(x, out);
	for (;;) {
		int c;
		if (accept(x, "==") || accept(x, "eq")) {
			xrel(x, &r);
			c = xcompare(out, &r) == 0;
		} else if (accept(x, "!=") || accept(x, "ne")) {
			xrel(x, &r);
			c = xcompare(out, &r) != 0;
		} else if (strncmp(x->p, "=", 1) == 0 && x->p[1] != '=' && accept(x, "=")) {
			xrel(x, &r);
			c = xcompare(out, &r) == 0;
		} else if (accept(x, "isin")) { /* substring test */
			xrel(x, &r);
			c = strstr(r.s, out->s) != NULL;
		} else {
			return;
		}
		xnum(out, c);
	}
}

static void xbits(xparse_t *x, xval_t *out)
{
	xval_t r;
	xeq(x, out);
	for (;;) {
		skipws(x);
		if (x->p[0] == '&' && x->p[1] != '&' && accept(x, "&")) {
			xeq(x, &r);
			xnum(out, (double)((long long)out->n & (long long)r.n));
		} else if (x->p[0] == '|' && x->p[1] != '|' && accept(x, "|")) {
			xeq(x, &r);
			xnum(out, (double)((long long)out->n | (long long)r.n));
		} else if (accept(x, "^")) {
			xeq(x, &r);
			xnum(out, (double)((long long)out->n ^ (long long)r.n));
		} else {
			return;
		}
	}
}

static void xand(xparse_t *x, xval_t *out)
{
	xval_t r;
	xbits(x, out);
	while (accept(x, "&&") || accept(x, "and")) {
		xbits(x, &r);
		xnum(out, xtrue(out) && xtrue(&r));
	}
}

static void xexpr(xparse_t *x, xval_t *out)
{
	xval_t r;
	xand(x, out);
	while (accept(x, "||") || accept(x, "or")) {
		xand(x, &r);
		xnum(out, xtrue(out) || xtrue(&r));
	}
}

bool Cmd_Evaluate(const char *text, char *out, size_t outsize, bool *truth)
{
	xparse_t x = {text, false};
	xval_t v;
	xexpr(&x, &v);
	skipws(&x);
	if (*x.p)
		x.error = true;
	if (out)
		Str_Copy(out, v.s, outsize);
	if (truth)
		*truth = xtrue(&v);
	return !x.error;
}

/* the text of arguments first..last-1, quoting any with spaces unless
 * they are pieces of an expression */
static void join_args_q(int first, int last, int argc, const char **argv, char *out, size_t size, bool quote)
{
	out[0] = 0;
	for (int i = first; i < last && i < argc; i++) {
		bool space = quote && strchr(argv[i], ' ') != NULL;
		if (i > first)
			Str_Cat(out, " ", size);
		if (space)
			Str_Cat(out, "\"", size);
		Str_Cat(out, argv[i], size);
		if (space)
			Str_Cat(out, "\"", size);
	}
}

static void join_args(int first, int last, int argc, const char **argv, char *out, size_t size)
{
	join_args_q(first, last, argc, argv, out, size, true);
}

/* if <expression> then <commands> [else <commands>]
 * (the "then" may be left out when the expression is one word) */
static void If_f(int argc, const char **argv)
{
	char expr[2048], cmd[4096];
	int then = -1, els = -1;
	bool truth;
	if (argc < 3) {
		Con_Printf("usage: if <expression> then <commands> [else <commands>]\n");
		return;
	}
	for (int i = 1; i < argc; i++) {
		if (then < 0 && !Str_ICmp(argv[i], "then"))
			then = i;
		else if (then >= 0 && els < 0 && !Str_ICmp(argv[i], "else"))
			els = i;
	}
	if (then < 0) {
		then = 1;
		Str_Copy(expr, argv[1], sizeof(expr));
		for (int i = 2; i < argc; i++)
			if (!Str_ICmp(argv[i], "else")) {
				els = i;
				break;
			}
	} else {
		join_args_q(1, then, argc, argv, expr, sizeof(expr), false);
	}
	if (!Cmd_Evaluate(expr, NULL, 0, &truth))
		Con_DPrintf("if: couldn't fully read \"%s\"\n", expr);
	if (truth)
		join_args(then + 1, els < 0 ? argc : els, argc, argv, cmd, sizeof(cmd));
	else if (els >= 0)
		join_args(els + 1, argc, argc, argv, cmd, sizeof(cmd));
	else
		return;
	/* a single quoted block runs as written, so "a; b" stays two commands */
	if (truth ? (els < 0 ? argc : els) - then == 2 : argc - els == 2)
		Str_Copy(cmd, argv[truth ? then + 1 : els + 1], sizeof(cmd));
	Cbuf_InsertText(cmd);
}

/* set_calc <cvar> <expression>, seta_calc archives it as well */
static void SetCalc_f(int argc, const char **argv)
{
	char expr[2048], result[512];
	cvar_t *v;
	if (argc < 3) {
		Con_Printf("usage: %s <cvar> <expression>\n", argv[0]);
		return;
	}
	join_args_q(2, argc, argc, argv, expr, sizeof(expr), false);
	if (!Cmd_Evaluate(expr, result, sizeof(result), NULL))
		Con_Printf("%s: couldn't read \"%s\"\n", argv[0], expr);
	v = Cvar_Find(argv[1]);
	if (!v)
		v = Cvar_Get(argv[1], "", CVAR_USERCREATED, NULL);
	if (!Str_ICmp(argv[0], "seta_calc"))
		v->flags |= CVAR_ARCHIVE;
	Cvar_Set(v->name, result);
}

/* defer <seconds> <commands> */
static void Defer_f(int argc, const char **argv)
{
	char cmd[4096];
	if (argc < 3) {
		Con_Printf("usage: defer <seconds> <commands>\n");
		return;
	}
	if (argc == 3)
		Str_Copy(cmd, argv[2], sizeof(cmd));
	else
		join_args(2, argc, argc, argv, cmd, sizeof(cmd));
	Cbuf_AddDeferred(atof(argv[1]), cmd);
}

/* --- config files ----------------------------------------------------------------- */

static cvar_t *cfg_save_name, *cfg_save_auto;
int cfg_saved_at = -1;

static bool write_text_file(const char *name, const char *text, size_t len)
{
	char path[1024];
	FILE *f;
	if (!FS_WritePath(name, path, sizeof(path))) {
		Con_Printf("can't write \"%s\"\n", name);
		return false;
	}
	FS_CreatePath(path);
	f = fopen(path, "wb");
	if (!f) {
		Con_Printf("couldn't open %s for writing\n", path);
		return false;
	}
	fwrite(text, 1, len, f);
	fclose(f);
	return true;
}

/* the default config is config.cfg in the game directory; a named one
 * lives under configs/, as FTE keeps them */
static const char *cfg_name(int argc, const char **argv, bool reading)
{
	static char name[256];
	char base[200];
	if (argc < 2) {
		snprintf(name, sizeof(name), "%s%s", cfg_save_name->string, strchr(cfg_save_name->string, '.') ? "" : ".cfg");
		return name;
	}
	Str_Copy(base, argv[1], sizeof(base));
	if (!strchr(base, '.'))
		Str_Cat(base, ".cfg", sizeof(base));
	if (strchr(base, '/'))
		Str_Copy(name, base, sizeof(name));
	else
		snprintf(name, sizeof(name), "configs/%s", base);
	/* an existing config of that name outside configs/ is read in place */
	if (reading && !FS_FileExists(name) && FS_FileExists(base) && !strchr(base, '/'))
		Str_Copy(name, base, sizeof(name));
	return name;
}

bool Cfg_Save(const char *name)
{
	strbuf_t sb = {0};
	bool ok;
	SB_AppendStr(&sb, "// written by Bulwark; settings you change in the game are saved here\n");
	if (cfg_write_hook)
		cfg_write_hook(&sb);
	Cvar_WriteArchived(&sb);
	Cmd_WriteAliases(&sb);
	ok = write_text_file(name, sb.data ? sb.data : "", sb.len);
	SB_Free(&sb);
	if (ok) {
		Con_Printf("wrote %s/%s\n", FS_GameDir(), name);
		cfg_saved_at = cfg_modified;
	}
	return ok;
}

/* cfg_save/saveconfig/writeconfig/host_writeconfig [name] */
static void CfgSave_f(int argc, const char **argv)
{
	if (!Str_ICmp(argv[0], "cfg_save_ifmodified") && cfg_saved_at == cfg_modified)
		return;
	Cfg_Save(cfg_name(argc, argv, false));
}

void Cfg_SaveIfModified(void)
{
	if (cfg_save_auto && cfg_save_auto->ivalue && cfg_saved_at != cfg_modified)
		Cfg_Save(cfg_name(1, NULL, false));
}

/* back to the engine and game defaults: cvars, binds, then default.cfg */
static void reset_all(void)
{
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (v->flags & CVAR_ARCHIVE)
			Cvar_Reset(v);
	if (cfg_reset_hook)
		cfg_reset_hook();
}

static void exec_defaults(void)
{
	if (FS_FileExists("default.cfg"))
		Cbuf_InsertText("exec default.cfg\n");
	else if (FS_FileExists("default_controls.cfg"))
		Cbuf_InsertText("exec default_controls.cfg\n");
}

/* cfg_load [name]: the defaults, then the config */
static void CfgLoad_f(int argc, const char **argv)
{
	const char *name = cfg_name(argc, argv, true);
	if (!FS_FileExists(name)) {
		Con_Printf("no config named %s\n", name);
		return;
	}
	reset_all();
	Cbuf_InsertText(va("exec \"%s\"\n", name));
	exec_defaults();
	cfg_saved_at = -2; /* settled after the exec runs */
}

/* cfg_reset/resetcfg: everything back to defaults */
static void CfgReset_f(int argc, const char **argv)
{
	reset_all();
	exec_defaults();
}

static void list_cfg(const char *name, const char *searchpath, void *ctx)
{
	int *n = ctx;
	Con_Printf("  %-32s %s\n", name, searchpath);
	(*n)++;
}

static void Listconfigs_f(int argc, const char **argv)
{
	int n = 0;
	FS_List("*.cfg", true, list_cfg, &n);
	FS_List("cfg/*.cfg", true, list_cfg, &n);
	Con_Printf("%d configs\n", n);
}

/* --- console log ------------------------------------------------------------------- */

static cvar_t *con_logfile, *con_timestamp;
static FILE *logfp;
static int log_modified = -1;
static bool log_at_line_start = true;

static void log_hook(const char *text)
{
	if (!logfp)
		return;
	for (const char *p = text; *p; p++) {
		if (log_at_line_start && con_timestamp->ivalue) {
			time_t t = time(NULL);
			char stamp[32];
			strftime(stamp, sizeof(stamp), "%m/%d/%Y - %H:%M:%S: ", localtime(&t));
			fputs(stamp, logfp);
		}
		/* colour codes are left out of the file */
		if (*p == '^' && p[1] >= '0' && p[1] <= '9') {
			p++;
			log_at_line_start = false;
			continue;
		}
		fputc(*p, logfp);
		log_at_line_start = *p == '\n';
	}
	fflush(logfp);
}

static void open_log(const char *name, bool append)
{
	char path[1024];
	if (logfp) {
		fclose(logfp);
		logfp = NULL;
	}
	if (!name || !*name || !strcmp(name, "0"))
		return;
	if (!FS_WritePath(name, path, sizeof(path))) {
		Con_Printf("can't log to \"%s\"\n", name);
		return;
	}
	FS_CreatePath(path);
	logfp = fopen(path, append ? "ab" : "wb");
	if (logfp)
		Con_Printf("logging console to %s\n", path);
	log_at_line_start = true;
}

void Cmd_CheckLogfile(void)
{
	if (con_logfile && con_logfile->modified != log_modified) {
		log_modified = con_logfile->modified;
		open_log(con_logfile->string, true);
	}
}

/* logfile [name]: Quake's toggle, writing qconsole.log by default */
static void Logfile_f(int argc, const char **argv)
{
	if (argc > 1) {
		Cvar_Set("con_logfile", argv[1]);
	} else if (logfp) {
		Cvar_Set("con_logfile", "");
		Con_Printf("console log closed\n");
	} else {
		Cvar_Set("con_logfile", "qconsole.log");
	}
	Cmd_CheckLogfile();
}

/* --- files ------------------------------------------------------------------------------ */

static void Which_f(int argc, const char **argv)
{
	const char *p;
	if (argc < 2) {
		Con_Printf("usage: %s <file>\n", argv[0]);
		return;
	}
	p = FS_WhichPath(argv[1]);
	if (p)
		Con_Printf("%s is in %s\n", argv[1], p);
	else
		Con_Printf("%s not found\n", argv[1]);
}

typedef struct {
	int count;
	bool strip; /* print map names without maps/ and .bsp */
} listing_t;

static void list_file(const char *name, const char *searchpath, void *ctx)
{
	listing_t *l = ctx;
	if (l->strip) {
		char n[256];
		const char *b = strrchr(name, '/');
		char *dot;
		Str_Copy(n, b ? b + 1 : name, sizeof(n));
		if ((dot = strrchr(n, '.')))
			*dot = 0;
		Con_Printf("  %-32s %s\n", n, searchpath);
	} else {
		Con_Printf("  %-48s %s\n", name, searchpath);
	}
	l->count++;
}

/* dir/ls [pattern] */
static void Dir_f(int argc, const char **argv)
{
	listing_t l = {0};
	char pattern[512];
	Str_Copy(pattern, argc > 1 ? argv[1] : "*", sizeof(pattern));
	if (!strpbrk(pattern, "*?")) {
		size_t n = strlen(pattern);
		Str_Cat(pattern, n && pattern[n - 1] == '/' ? "*" : "/*", sizeof(pattern));
	}
	FS_List(pattern, true, list_file, &l);
	Con_Printf("%d files\n", l.count);
}

/* maps/listmaps [pattern] */
static void Maps_f(int argc, const char **argv)
{
	listing_t l = {0, true};
	char pattern[256];
	const char *p = argc > 1 && strcmp(argv[1], "*") ? argv[1] : "";
	snprintf(pattern, sizeof(pattern), "maps/*%s*.bsp", p);
	FS_List(pattern, true, list_file, &l);
	Con_Printf("%d maps\n", l.count);
}

static void Version_f(int argc, const char **argv)
{
	Con_Printf("Bulwark %s (protocol %d), built %s %s\n", BULWARK_VERSION, 1, __DATE__, __TIME__);
}

/* hunkprint/mem_dump: what the allocator can tell us */
static void Memstats_f(int argc, const char **argv)
{
	int cvars = 0;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		cvars++;
	Con_Printf("%d cvars; memory is allocated as needed, there is no fixed hunk\n", cvars);
}

void Cmd_InitExtra(void)
{
	cfg_save_name = Cvar_Get("cfg_save_name", "config", CVAR_ARCHIVE, "Config file that cfg_save writes and cfg_load reads");
	cfg_save_auto = Cvar_Get("cfg_save_auto", "1", CVAR_ARCHIVE, "Save the config on quit when a setting or bind changed");
	con_logfile = Cvar_Get("con_logfile", "", 0, "Copy console output to this file in the game directory");
	con_timestamp = Cvar_Get("con_timestamp", "0", CVAR_ARCHIVE, "Prefix lines in the console log with the date and time");
	Con_SetLogHook(log_hook);

	Cmd_AddD("toggle", Toggle_f, "toggle <cvar> [values...]: flip between 0 and 1, or step through the values");
	Cmd_AddD("inc", Inc_f, "inc <cvar> [amount]: add to a cvar, 1 by default");
	Cmd_AddD("incrementvar", Incrementvar_f, "incrementvar <cvar> <min> <max> <delta>: step a cvar, wrapping");
	Cmd_AddD("multvar", Incrementvar_f, "multvar <cvar> <min> <max> <factor>: scale a cvar within a range");
	Cmd_AddD("reset", Reset_f, "reset <cvar or pattern>: back to the default value");
	Cmd_AddD("cvarreset", Reset_f, "cvarreset <cvar or pattern>: back to the default value");
	Cmd_AddD("revert", Reset_f, "revert <cvar or pattern>: back to the default value");
	Cmd_AddD("cvar_resettodefaults_all", ResetAll_f, "Every cvar back to its default");
	Cmd_AddD("cvar_resettodefaults_saveonly", ResetAll_f, "Every archived cvar back to its default");
	Cmd_AddD("cvar_resettodefaults_nosaveonly", ResetAll_f, "Every cvar that isn't archived back to its default");
	Cmd_AddD("differences", Differences_f, "List the cvars that differ from their defaults");
	Cmd_AddD("cvarwatch", Cvarwatch_f, "cvarwatch <cvar>: print each change to it (again to stop)");
	Cmd_AddD("vstr", Vstr_f, "vstr <cvar>: run the cvar's value as commands");
	Cmd_AddD("find", Find_f, "find <text>: commands and cvars whose name or help holds the text");
	Cmd_AddD("apropos", Find_f, "apropos <text>: commands and cvars whose name or help holds the text");
	Cmd_AddD("help", Help_f, "help <name>: describe a command or cvar");
	Cmd_AddD("aliaslist", Aliaslist_f, "aliaslist [pattern]: list the aliases");
	Cmd_AddD("if", If_f, "if <expression> then <commands> [else <commands>]");
	Cmd_AddD("set_calc", SetCalc_f, "set_calc <cvar> <expression>: set a cvar to a computed value");
	Cmd_AddD("seta_calc", SetCalc_f, "seta_calc <cvar> <expression>: set_calc, and archive the cvar");
	Cmd_AddD("defer", Defer_f, "defer <seconds> <commands>: run commands later");
	Cmd_AddD("cfg_save", CfgSave_f, "cfg_save [name]: write settings, binds and aliases to a config");
	Cmd_AddD("cfg_save_ifmodified", CfgSave_f, "cfg_save, only if something changed since the last save");
	Cmd_AddD("saveconfig", CfgSave_f, "saveconfig [name]: write the config");
	Cmd_AddD("writeconfig", CfgSave_f, "writeconfig [name]: write the config");
	Cmd_AddD("host_writeconfig", CfgSave_f, "host_writeconfig [name]: write the config");
	Cmd_AddD("cfg_load", CfgLoad_f, "cfg_load [name]: reset to defaults, then run the config");
	Cmd_AddD("cfg_reset", CfgReset_f, "Settings and binds back to their defaults");
	Cmd_AddD("resetcfg", CfgReset_f, "Settings and binds back to their defaults");
	Cmd_AddD("listconfigs", Listconfigs_f, "List the .cfg files");
	Cmd_AddD("logfile", Logfile_f, "logfile [name]: start or stop copying the console to a file");
	Cmd_AddD("which", Which_f, "which <file>: the package or directory a file comes from");
	Cmd_AddD("flocate", Which_f, "flocate <file>: the package or directory a file comes from");
	Cmd_AddD("dir", Dir_f, "dir [pattern]: list game files");
	Cmd_AddD("ls", Dir_f, "ls [pattern]: list game files");
	Cmd_AddD("maps", Maps_f, "maps [text]: list the maps");
	Cmd_AddD("maplist", Maps_f, "maplist [text]: list the maps");
	Cmd_AddD("listmaps", Maps_f, "listmaps [text]: list the maps");
	Cmd_AddD("version", Version_f, "Engine version and build date");
	Cmd_AddD("hunkprint", Memstats_f, "Memory statistics");
}
