/* Bulwark engine - QuakeC compiling and decompiling from the console.
 *
 * compile [file.src] runs an external QuakeC compiler (fteqcc, found on the
 * PATH or in the base directory) on <pr_sourcedir>/progs.src, or the named
 * .src file, in the game directory that holds it. It runs on a worker
 * thread and its output comes back to the console when it finishes. The
 * compiler is started directly, without a shell, so names can't smuggle in
 * other commands.
 *
 * applycompile saves the running game, then loads it again, which brings
 * in freshly compiled progs with the game where it was.
 *
 * decompile [progs] writes decompiled/<progs>.qc: the progs' fields and
 * globals with their types and values, and each function as one readable
 * pseudo-QC line a statement, with labels at jump targets. It reads the
 * file itself (see docs/spec/progs-format.md), so any progs on the search
 * path can be decompiled whether or not it is running.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/worker.h"
#include "../qcvm/opcodes.h"
#include "../qcvm/progs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static cvar_t *pr_sourcedir;

/* --- compile --------------------------------------------------------------- */

typedef struct {
	char compiler[512], dir[512], srcfile[256];
	strbuf_t out;
	int status;
} compilejob_t;

static bool compiling;

#if defined(__3DS__) || defined(__NDS__)
/* consoles can't start another program */
static void compile_work(void *ctx)
{
	compilejob_t *j = ctx;
	SB_Printf(&j->out, "compile: can't run a compiler on this platform\n");
	j->status = -1;
}
#else
static void compile_work(void *ctx)
{
	compilejob_t *j = ctx;
	int fds[2];
	pid_t pid;
	if (pipe(fds)) {
		SB_Printf(&j->out, "compile: couldn't make a pipe: %s\n", strerror(errno));
		j->status = -1;
		return;
	}
	pid = fork();
	if (pid == 0) {
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		if (chdir(j->dir) == 0)
			execlp(j->compiler, j->compiler, "-srcfile", j->srcfile, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		SB_Printf(&j->out, "compile: couldn't start %s\n", j->compiler);
		j->status = -1;
		return;
	}
	for (;;) {
		char buf[4096];
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n <= 0)
			break;
		SB_Append(&j->out, buf, (size_t)n);
	}
	close(fds[0]);
	waitpid(pid, &j->status, 0);
}
#endif

static void compile_done(void *ctx)
{
	compilejob_t *j = ctx;
	if (j->out.len) {
		SB_Append(&j->out, "", 1);
		Con_Print(j->out.data);
		if (j->out.data[j->out.len - 2] != '\n')
			Con_Print("\n");
	}
	if (WIFEXITED(j->status) && WEXITSTATUS(j->status) == 127 && !j->out.len)
		Con_Printf("compile: couldn't run %s\n", j->compiler);
	else
		Con_Printf("compile: %s %s\n", j->srcfile,
		           WIFEXITED(j->status) && WEXITSTATUS(j->status) == 0 ? "finished" : "failed");
	SB_Free(&j->out);
	Mem_Free(j);
	compiling = false;
}

static bool is_file(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISREG(st.st_mode);
}

/* the compiler: fteqcc on the PATH, or a copy beside the game */
static bool find_compiler(char *out, size_t size)
{
	const char *path = getenv("PATH");
	snprintf(out, size, "%s/fteqcc", FS_BaseDir());
	if (is_file(out) && !access(out, X_OK))
		return true;
	while (path && *path) {
		const char *e = strchr(path, ':');
		size_t n = e ? (size_t)(e - path) : strlen(path);
		snprintf(out, size, "%.*s/fteqcc", (int)n, path);
		if (n && is_file(out) && !access(out, X_OK))
			return true;
		path = e ? e + 1 : NULL;
	}
	return false;
}

static void Compile_f(int argc, const char **argv)
{
	compilejob_t *j;
	const char *src = argc > 1 ? argv[1] : "progs.src";
	char rel[256];
	if (compiling) {
		Con_Printf("compile: already compiling\n");
		return;
	}
	j = Mem_Alloc(sizeof(*j));
	if (!find_compiler(j->compiler, sizeof(j->compiler))) {
		Con_Printf("compile: no QuakeC compiler found (fteqcc on the PATH or in %s)\n", FS_BaseDir());
		Mem_Free(j);
		return;
	}
	snprintf(rel, sizeof(rel), "%s/%s", pr_sourcedir->string, src);
	if (!FS_SafeName(rel)) {
		Con_Printf("compile: %s: not a name in the game directory\n", src);
		Mem_Free(j);
		return;
	}
	/* the newest game directory that has the source */
	for (int i = FS_NumGameDirs() - 1; i >= 0 && !j->dir[0]; i--) {
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s/%s", FS_BaseDir(), FS_GameDirAt(i), rel);
		if (is_file(full))
			snprintf(j->dir, sizeof(j->dir), "%s/%s/%s", FS_BaseDir(), FS_GameDirAt(i), pr_sourcedir->string);
	}
	if (!j->dir[0]) {
		Con_Printf("compile: %s not found\n", rel);
		Mem_Free(j);
		return;
	}
	Str_Copy(j->srcfile, src, sizeof(j->srcfile));
	Con_Printf("Source directory: %s/\nSource file: %s\n", pr_sourcedir->string, rel);
	compiling = true;
	Worker_Add(compile_work, compile_done, j);
}

static void ApplyCompile_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	if (!sv.active) {
		Con_Printf("applycompile: no game is running\n");
		return;
	}
	if (compiling) {
		Con_Printf("applycompile: still compiling\n");
		return;
	}
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED) {
			Con_Printf("Saving state\n");
			Cbuf_InsertText("save applycompile\nload applycompile\n");
			return;
		}
	/* nobody in the game to keep: start the map over with the new progs */
	Cbuf_InsertText("restart\n");
}

/* --- decompile --------------------------------------------------------------- */

static const char *const opnames[OP_NUMOPS] = {
	"DONE", "MUL_F", "MUL_V", "MUL_FV", "MUL_VF", "DIV_F", "ADD_F", "ADD_V", "SUB_F", "SUB_V", "EQ_F", "EQ_V", "EQ_S",
	"EQ_E", "EQ_FNC", "NE_F", "NE_V", "NE_S", "NE_E", "NE_FNC", "LE_F", "GE_F", "LT_F", "GT_F", "LOADF_F", "LOADF_V",
	"LOADF_S", "LOADF_ENT", "LOADF_FLD", "LOADF_FNC", "FLDADDRESS", "STORE_F", "STORE_V", "STORE_S", "STORE_ENT",
	"STORE_FLD", "STORE_FNC", "STOREP_F", "STOREP_V", "STOREP_S", "STOREP_ENT", "STOREP_FLD", "STOREP_FNC", "RETURN",
	"NOT_F", "NOT_V", "NOT_S", "NOT_ENT", "NOT_FNC", "IF", "IFNOT", "CALL0", "CALL1", "CALL2", "CALL3", "CALL4",
	"CALL5", "CALL6", "CALL7", "CALL8", "STATE", "GOTO", "AND_F", "OR_F", "BITAND", "BITOR", "MULSTORE_F",
	"MULSTORE_VF", "MULSTOREP_F", "MULSTOREP_VF", "DIVSTORE_F", "DIVSTOREP_F", "ADDSTORE_F", "ADDSTORE_V",
	"ADDSTOREP_F", "ADDSTOREP_V", "SUBSTORE_F", "SUBSTORE_V", "SUBSTOREP_F", "SUBSTOREP_V", "FETCH_GBL_F",
	"FETCH_GBL_V", "FETCH_GBL_S", "FETCH_GBL_E", "FETCH_GBL_FNC", "CSTATE", "CWSTATE", "THINKTIME", "BITSETSTORE_F",
	"BITSETSTOREP_F", "BITCLRSTORE_F", "BITCLRSTOREP_F", "RAND0", "RAND1", "RAND2", "RANDV0", "RANDV1", "RANDV2",
	"SWITCH_F", "SWITCH_V", "SWITCH_S", "SWITCH_E", "SWITCH_FNC", "CASE", "CASERANGE", "CALL1H", "CALL2H", "CALL3H",
	"CALL4H", "CALL5H", "CALL6H", "CALL7H", "CALL8H", "STORE_I", "STORE_IF", "STORE_FI", "ADD_I", "ADD_FI", "ADD_IF",
	"SUB_I", "SUB_FI", "SUB_IF", "CONV_IF", "CONV_FI", "CONVP_IF", "CONVP_FI", "LOADF_I", "STOREP_I", "STOREP_IF",
	"STOREP_FI", "BITAND_I", "BITOR_I", "MUL_I", "DIV_I", "EQ_I", "NE_I", "IFNOTS", "IFS", "NOT_I", "DIV_VF",
	"BITXOR_I", "RSHIFT_I", "LSHIFT_I", "GLOBALADDRESS", "ADD_PIW", "LOADA_F", "LOADA_V", "LOADA_S", "LOADA_ENT",
	"LOADA_FLD", "LOADA_FNC", "LOADA_I", "STORE_P", "LOADF_P", "LOADP_F", "LOADP_V", "LOADP_S", "LOADP_ENT",
	"LOADP_FLD", "LOADP_FNC", "LOADP_I", "LE_I", "GE_I", "LT_I", "GT_I", "LE_IF", "GE_IF", "LT_IF", "GT_IF", "LE_FI",
	"GE_FI", "LT_FI", "GT_FI", "EQ_IF", "EQ_FI", "ADD_SF", "SUB_S", "STOREP_C", "LOADP_C", "MUL_IF", "MUL_FI", "MUL_VI",
	"MUL_IV", "DIV_IF", "DIV_FI", "BITAND_IF", "BITOR_IF", "BITAND_FI", "BITOR_FI", "AND_I", "OR_I", "AND_IF", "OR_IF",
	"AND_FI", "OR_FI", "NE_IF", "NE_FI", "GSTOREP_I", "GSTOREP_F", "GSTOREP_ENT", "GSTOREP_FLD", "GSTOREP_S",
	"GSTOREP_FNC", "GSTOREP_V", "GADDRESS", "GLOAD_I", "GLOAD_F", "GLOAD_FLD", "GLOAD_ENT", "GLOAD_S", "GLOAD_FNC",
	"BOUNDCHECK", "UNUSED", "PUSH", "POP", "SWITCH_I", "GLOAD_V", "IF_F", "IFNOT_F", "STOREF_V", "STOREF_F", "STOREF_S",
	"STOREF_I", "STOREP_I8", "LOADP_U8", "LE_U", "LT_U", "DIV_U", "RSHIFT_U", "ADD_I64", "SUB_I64", "MUL_I64",
	"DIV_I64", "BITAND_I64", "BITOR_I64", "BITXOR_I64", "LSHIFT_I64I", "RSHIFT_I64I", "LE_I64", "LT_I64", "EQ_I64",
	"NE_I64", "LE_U64", "LT_U64", "DIV_U64", "RSHIFT_U64I", "STORE_I64", "STOREP_I64", "STOREF_I64", "LOADF_I64",
	"LOADA_I64", "LOADP_I64", "CONV_UI64", "CONV_II64", "CONV_I64I", "CONV_FD", "CONV_DF", "CONV_I64F", "CONV_FI64",
	"CONV_I64D", "CONV_DI64", "ADD_D", "SUB_D", "MUL_D", "DIV_D", "LE_D", "LT_D", "EQ_D", "NE_D",
};

typedef struct {
	uint32_t type, ofs;
	const char *name;
} ddef_t;

typedef struct {
	const uint8_t *d;
	size_t size;
	progs_header_t h;
	bool stmt32, def32;
	dstatement32_t *st;
	ddef_t *globals, *fields;
	dfunction_t *funcs;
	const char *strings;
	const uint32_t *gv; /* initial global values */
	const char **gname; /* a name for each global word, NULL for none */
	const ddef_t **gdef;
	uint8_t *label;    /* statements that are jump targets */
	uint8_t *isimm;    /* globals that hold an unnamed constant */
	strbuf_t *out;
} decomp_t;

static uint32_t rdu(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static const char *str_at(decomp_t *c, int32_t s)
{
	return s >= 0 && s < c->h.numstrings ? c->strings + s : "";
}

static const char *type_name(uint32_t t)
{
	static const char *const names[] = {"void", "string", "float", "vector", "entity", ".field", "function",
	                                    "pointer", "int", "uint", "__int64", "__uint64", "double", "__variant",
	                                    "struct", "union", "accessor", "enum", "bool"};
	t &= ~DEF_SAVEGLOBAL;
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static float gfloat(decomp_t *c, uint32_t ofs)
{
	float f;
	uint32_t u = ofs < (uint32_t)c->h.numglobals ? c->gv[ofs] : 0;
	memcpy(&f, &u, 4);
	return f;
}

static void quote(strbuf_t *sb, const char *s)
{
	SB_Append(sb, "\"", 1);
	for (; *s; s++) {
		if (*s == '\n')
			SB_AppendStr(sb, "\\n");
		else if (*s == '"' || *s == '\\') {
			SB_Append(sb, "\\", 1);
			SB_Append(sb, s, 1);
		} else
			SB_Append(sb, s, 1);
	}
	SB_Append(sb, "\"", 1);
}

static const char *func_name(decomp_t *c, uint32_t f)
{
	f &= 0xffffff;
	return f > 0 && f < (uint32_t)c->h.numfunctions ? str_at(c, c->funcs[f].s_name) : NULL;
}

static const char *field_name(decomp_t *c, uint32_t ofs)
{
	for (int i = 0; i < c->h.numfielddefs; i++)
		if (c->fields[i].ofs == ofs && c->fields[i].name[0])
			return c->fields[i].name;
	return NULL;
}

/* an immediate's value as QC would write it */
static void value(decomp_t *c, strbuf_t *sb, uint32_t type, uint32_t ofs)
{
	uint32_t u = ofs < (uint32_t)c->h.numglobals ? c->gv[ofs] : 0;
	const char *n;
	switch (type & ~DEF_SAVEGLOBAL) {
	case ev_string: quote(sb, str_at(c, (int32_t)u)); break;
	case ev_float: SB_Printf(sb, "%g", gfloat(c, ofs)); break;
	case ev_vector: SB_Printf(sb, "'%g %g %g'", gfloat(c, ofs), gfloat(c, ofs + 1), gfloat(c, ofs + 2)); break;
	case ev_entity: SB_AppendStr(sb, u ? va("(entity)%u", u) : "world"); break;
	case ev_field: n = field_name(c, u); SB_AppendStr(sb, n ? va(".%s", n) : va("(field)%u", u)); break;
	case ev_function: n = func_name(c, u); SB_AppendStr(sb, n ? n : (u ? va("(function)%u", u) : "__NULL__")); break;
	case ev_integer: SB_Printf(sb, "%di", (int32_t)u); break;
	default: SB_Printf(sb, "0x%x", u); break;
	}
}

/* the operand at a global offset: its name, a constant's value, or a slot */
static void operand(decomp_t *c, strbuf_t *sb, uint32_t ofs)
{
	const ddef_t *d = ofs < (uint32_t)c->h.numglobals ? c->gdef[ofs] : NULL;
	if (ofs == OFS_RETURN) {
		SB_AppendStr(sb, "RETURN");
		return;
	}
	if (ofs >= OFS_PARM0 && ofs < OFS_PARM0 + 3 * MAX_PARMS && (ofs - OFS_PARM0) % 3 == 0) {
		SB_Printf(sb, "PARM%u", (ofs - OFS_PARM0) / 3);
		return;
	}
	if (d && d->ofs == ofs && ofs < (uint32_t)c->h.numglobals && c->isimm[ofs]) {
		value(c, sb, d->type, ofs);
		return;
	}
	if (ofs < (uint32_t)c->h.numglobals && c->gname[ofs]) {
		SB_AppendStr(sb, c->gname[ofs]);
		return;
	}
	SB_Printf(sb, "_g%u", ofs);
}

static void op_symbol(const char *name, const char **sym)
{
	static const struct { const char *prefix, *sym; } table[] = {
		{"MUL_", "*"}, {"DIV_", "/"}, {"ADD_", "+"}, {"SUB_", "-"}, {"EQ_", "=="}, {"NE_", "!="}, {"LE_", "<="},
		{"GE_", ">="}, {"LT_", "<"}, {"GT_", ">"}, {"AND_", "&&"}, {"OR_", "||"}, {"BITAND", "&"}, {"BITOR", "|"},
		{"BITXOR", "^"}, {"LSHIFT", "<<"}, {"RSHIFT", ">>"}};
	*sym = NULL;
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (!strncmp(name, table[i].prefix, strlen(table[i].prefix))) {
			*sym = table[i].sym;
			return;
		}
}

static void statement(decomp_t *c, int s)
{
	const dstatement32_t *st = &c->st[s];
	const char *name = st->op < OP_NUMOPS && opnames[st->op][0] ? opnames[st->op] : NULL;
	strbuf_t *o = c->out;
	const char *sym;
	int32_t sa = c->stmt32 ? (int32_t)st->a : (int16_t)st->a, sb_ = c->stmt32 ? (int32_t)st->b : (int16_t)st->b;
	int32_t sc = c->stmt32 ? (int32_t)st->c : (int16_t)st->c;
#define A operand(c, o, st->a)
#define B operand(c, o, st->b)
#define C operand(c, o, st->c)
	SB_AppendStr(o, "\t");
	if (!name) {
		SB_Printf(o, "/* unknown opcode %u */ %u, %u, %u;\n", st->op, st->a, st->b, st->c);
		return;
	}
	if (st->op == OP_DONE || st->op == OP_RETURN) {
		if (st->op == OP_RETURN && st->a) {
			SB_AppendStr(o, "return ");
			A;
			SB_AppendStr(o, ";\n");
		} else
			SB_AppendStr(o, "return;\n");
		return;
	}
	if (st->op == OP_GOTO) {
		SB_Printf(o, "goto L%d;\n", s + sa);
		return;
	}
	if (st->op == OP_IF || st->op == OP_IFNOT || st->op == OP_IF_F || st->op == OP_IFNOT_F || st->op == OP_IFS ||
	    st->op == OP_IFNOTS) {
		bool neg = strstr(name, "NOT") != NULL;
		SB_AppendStr(o, neg ? "if (!" : "if (");
		A;
		SB_Printf(o, ") goto L%d;\n", s + sb_);
		return;
	}
	if ((st->op >= OP_CALL0 && st->op <= OP_CALL8) || (st->op >= OP_CALL1H && st->op <= OP_CALL8H)) {
		bool h = st->op >= OP_CALL1H;
		int n = h ? st->op - OP_CALL1H + 1 : st->op - OP_CALL0;
		A;
		SB_AppendStr(o, "(");
		for (int i = 0; i < n; i++) {
			if (i)
				SB_AppendStr(o, ", ");
			if (h && i == 0)
				B;
			else if (h && i == 1)
				C;
			else
				SB_Printf(o, "PARM%d", i);
		}
		SB_AppendStr(o, ");\n");
		return;
	}
	if (st->op == OP_STATE || st->op == OP_CSTATE || st->op == OP_CWSTATE) {
		SB_AppendStr(o, st->op == OP_STATE ? "state(" : st->op == OP_CSTATE ? "cstate(" : "cwstate(");
		A;
		SB_AppendStr(o, ", ");
		B;
		SB_AppendStr(o, ");\n");
		return;
	}
	if (!strncmp(name, "SWITCH_", 7)) {
		SB_AppendStr(o, "switch (");
		A;
		SB_Printf(o, ") goto L%d;\n", s + sb_);
		return;
	}
	if (st->op == OP_CASE || st->op == OP_CASERANGE) {
		SB_AppendStr(o, "case ");
		A;
		if (st->op == OP_CASERANGE) {
			SB_AppendStr(o, " .. ");
			B;
		}
		SB_Printf(o, ": goto L%d;\n", s + (st->op == OP_CASE ? sb_ : sc));
		return;
	}
	if (!strncmp(name, "NOT_", 4)) {
		C;
		SB_AppendStr(o, " = !");
		A;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (!strncmp(name, "STORE_", 6)) {
		B;
		SB_AppendStr(o, " = ");
		A;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (!strncmp(name, "STOREP_", 7) || !strncmp(name, "GSTOREP_", 8)) {
		SB_AppendStr(o, "*(");
		B;
		if (st->c && st->op != OP_STOREP_C) {
			SB_AppendStr(o, " + ");
			C;
		}
		SB_AppendStr(o, ") = ");
		A;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (!strncmp(name, "STOREF_", 7)) {
		A;
		SB_AppendStr(o, ".");
		{
			const char *f = st->b < (uint32_t)c->h.numglobals ? field_name(c, c->gv[st->b]) : NULL;
			if (f && c->gdef[st->b] && !c->gdef[st->b]->name[0])
				SB_AppendStr(o, f);
			else
				B;
		}
		SB_AppendStr(o, " = ");
		C;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (!strncmp(name, "LOADF_", 6) || st->op == OP_FLDADDRESS) {
		C;
		SB_AppendStr(o, st->op == OP_FLDADDRESS ? " = &" : " = ");
		A;
		SB_AppendStr(o, ".");
		{
			const char *f = st->b < (uint32_t)c->h.numglobals ? field_name(c, c->gv[st->b]) : NULL;
			const ddef_t *d = st->b < (uint32_t)c->h.numglobals ? c->gdef[st->b] : NULL;
			/* a field constant reads best as the field's own name */
			if (f && d && (!d->name[0] || !strcmp(d->name, f)))
				SB_AppendStr(o, f);
			else
				B;
		}
		SB_AppendStr(o, ";\n");
		return;
	}
	if (!strncmp(name, "LOADP_", 6) || !strncmp(name, "GLOAD_", 6)) {
		C;
		SB_AppendStr(o, " = *(");
		A;
		if (st->b) {
			SB_AppendStr(o, " + ");
			B;
		}
		SB_AppendStr(o, ");\n");
		return;
	}
	if (!strncmp(name, "LOADA_", 6) || st->op == OP_GLOBALADDRESS) {
		C;
		SB_AppendStr(o, st->op == OP_GLOBALADDRESS ? " = &" : " = ");
		A;
		SB_AppendStr(o, "[");
		B;
		SB_AppendStr(o, "];\n");
		return;
	}
	if (!strncmp(name, "RAND", 4)) {
		C;
		SB_AppendStr(o, name[4] == 'V' ? " = randomv(" : " = random(");
		if (name[strlen(name) - 1] >= '1')
			A;
		if (name[strlen(name) - 1] == '2') {
			SB_AppendStr(o, ", ");
			B;
		}
		SB_AppendStr(o, ");\n");
		return;
	}
	if (!strncmp(name, "CONV_", 5)) {
		C;
		SB_Printf(o, " = (%s)", !strcmp(name, "CONV_IF") ? "float" : !strcmp(name, "CONV_FI") ? "int" : name + 5);
		A;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (strstr(name, "STORE")) {
		/* compound assignment: b op= a */
		static const struct { const char *prefix, *sym; } ops[] = {
			{"MULSTORE", "*="}, {"DIVSTORE", "/="}, {"ADDSTORE", "+="}, {"SUBSTORE", "-="},
			{"BITSETSTORE", "|="}, {"BITCLRSTORE", "&~="}};
		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
			if (!strncmp(name, ops[i].prefix, strlen(ops[i].prefix))) {
				bool ptr = strstr(name, "STOREP") != NULL;
				if (ptr)
					SB_AppendStr(o, "*(");
				B;
				SB_Printf(o, "%s %s ", ptr ? ")" : "", ops[i].sym);
				A;
				SB_AppendStr(o, ";\n");
				return;
			}
	}
	if (st->op == OP_THINKTIME) {
		A;
		SB_AppendStr(o, ".nextthink = time + ");
		B;
		SB_AppendStr(o, ";\n");
		return;
	}
	if (st->op == OP_BOUNDCHECK) {
		/* b and c are numbers, not globals */
		SB_Printf(o, "boundcheck(%u <= ", st->c);
		A;
		SB_Printf(o, " < %u);\n", st->b);
		return;
	}
	op_symbol(name, &sym);
	if (sym) {
		C;
		SB_AppendStr(o, " = ");
		A;
		SB_Printf(o, " %s ", sym);
		B;
		SB_AppendStr(o, ";\n");
		return;
	}
	SB_Printf(o, "/* %s */ ", name);
	A;
	SB_AppendStr(o, ", ");
	B;
	SB_AppendStr(o, ", ");
	C;
	SB_AppendStr(o, ";\n");
#undef A
#undef B
#undef C
}

static void read_defs(decomp_t *c, int32_t ofs, int count, ddef_t **out)
{
	*out = Mem_Alloc(sizeof(ddef_t) * (count + 1));
	for (int i = 0; i < count; i++) {
		const uint8_t *p = c->d + ofs + (size_t)i * (c->def32 ? 12 : 8);
		if (c->def32) {
			(*out)[i].type = rdu(p);
			(*out)[i].ofs = rdu(p + 4);
			(*out)[i].name = str_at(c, (int32_t)rdu(p + 8));
		} else {
			(*out)[i].type = p[0] | (p[1] << 8);
			(*out)[i].ofs = p[2] | (p[3] << 8);
			(*out)[i].name = str_at(c, (int32_t)rdu(p + 4));
		}
	}
}

static int32_t section_end(const progs_header_t *h, int32_t ofs, size_t size)
{
	int32_t o[6] = {h->ofs_statements, h->ofs_globaldefs, h->ofs_fielddefs, h->ofs_functions, h->ofs_strings, h->ofs_globals};
	int32_t best = (int32_t)size;
	for (int i = 0; i < 6; i++)
		if (o[i] > ofs && o[i] < best)
			best = o[i];
	return best;
}

static bool in_range(size_t size, int32_t ofs, int32_t count, int recsize)
{
	return ofs >= 0 && count >= 0 && (size_t)ofs + (size_t)count * recsize <= size;
}

bool QC_Decompile(const char *name, const uint8_t *d, size_t size, strbuf_t *out)
{
	decomp_t c;
	char *strings;
	uint8_t *islocal;
	memset(&c, 0, sizeof(c));
	c.d = d;
	c.size = size;
	c.out = out;
	if (size < PROGS_HEADER_V6_SIZE)
		return false;
	for (int i = 0; i < 14; i++)
		((int32_t *)&c.h)[i] = (int32_t)rdu(d + i * 4);
	if (c.h.version != PROGS_VERSION_QUAKE && c.h.version != PROGS_VERSION_EXTENDED)
		return false;
	c.stmt32 = c.h.numstatements && (section_end(&c.h, c.h.ofs_statements, size) - c.h.ofs_statements) / c.h.numstatements >= 16;
	c.def32 = c.h.numglobaldefs && (section_end(&c.h, c.h.ofs_globaldefs, size) - c.h.ofs_globaldefs) / c.h.numglobaldefs >= 12;
	if (!in_range(size, c.h.ofs_statements, c.h.numstatements, c.stmt32 ? 16 : 8) ||
	    !in_range(size, c.h.ofs_globaldefs, c.h.numglobaldefs, c.def32 ? 12 : 8) ||
	    !in_range(size, c.h.ofs_fielddefs, c.h.numfielddefs, c.def32 ? 12 : 8) ||
	    !in_range(size, c.h.ofs_functions, c.h.numfunctions, 36) || !in_range(size, c.h.ofs_strings, c.h.numstrings, 1) ||
	    !in_range(size, c.h.ofs_globals, c.h.numglobals, 4))
		return false;
	strings = Mem_Alloc((size_t)c.h.numstrings + 1);
	memcpy(strings, d + c.h.ofs_strings, (size_t)c.h.numstrings);
	c.strings = strings;
	c.st = Mem_Alloc(sizeof(dstatement32_t) * (c.h.numstatements + 1));
	for (int i = 0; i < c.h.numstatements; i++) {
		const uint8_t *p = d + c.h.ofs_statements + (size_t)i * (c.stmt32 ? 16 : 8);
		if (c.stmt32) {
			c.st[i].op = rdu(p);
			c.st[i].a = rdu(p + 4);
			c.st[i].b = rdu(p + 8);
			c.st[i].c = rdu(p + 12);
		} else {
			c.st[i].op = p[0] | (p[1] << 8);
			c.st[i].a = p[2] | (p[3] << 8);
			c.st[i].b = p[4] | (p[5] << 8);
			c.st[i].c = p[6] | (p[7] << 8);
		}
	}
	read_defs(&c, c.h.ofs_globaldefs, c.h.numglobaldefs, &c.globals);
	read_defs(&c, c.h.ofs_fielddefs, c.h.numfielddefs, &c.fields);
	c.funcs = Mem_Alloc(sizeof(dfunction_t) * (c.h.numfunctions + 1));
	for (int i = 0; i < c.h.numfunctions; i++) {
		const uint8_t *p = d + c.h.ofs_functions + (size_t)i * 36;
		dfunction_t *f = &c.funcs[i];
		f->first_statement = (int32_t)rdu(p);
		f->parm_start = (int32_t)rdu(p + 4);
		f->locals = (int32_t)rdu(p + 8);
		f->profile = (int32_t)rdu(p + 12);
		f->s_name = (int32_t)rdu(p + 16);
		f->s_file = (int32_t)rdu(p + 20);
		f->numparms = (int32_t)rdu(p + 24);
		memcpy(f->parm_size, p + 28, 8);
	}
	{
		uint32_t *gv = Mem_Alloc(sizeof(uint32_t) * ((size_t)c.h.numglobals + 1));
		for (int i = 0; i < c.h.numglobals; i++)
			gv[i] = rdu(d + c.h.ofs_globals + (size_t)i * 4);
		c.gv = gv;
	}
	c.gname = Mem_Alloc(sizeof(char *) * ((size_t)c.h.numglobals + 3));
	c.gdef = Mem_Alloc(sizeof(ddef_t *) * ((size_t)c.h.numglobals + 3));
	c.isimm = Mem_Alloc((size_t)c.h.numglobals + 3);
	for (int i = 0; i < c.h.numglobaldefs; i++) {
		const ddef_t *g = &c.globals[i];
		if (g->ofs >= (uint32_t)c.h.numglobals)
			continue;
		if (!g->name[0] || !strcmp(g->name, "IMMEDIATE")) {
			/* compilers share one slot between equal constants; the
			 * unnamed one means it is a literal in the code */
			c.isimm[g->ofs] = 1;
			c.gdef[g->ofs] = g;
		} else if (!c.gdef[g->ofs]) {
			c.gdef[g->ofs] = g;
		}
		if (g->name[0] && strcmp(g->name, "IMMEDIATE") && !c.gname[g->ofs]) {
			c.gname[g->ofs] = g->name;
			if ((g->type & ~DEF_SAVEGLOBAL) == ev_vector && g->ofs + 2 < (uint32_t)c.h.numglobals) {
				if (!c.gname[g->ofs + 1])
					c.gname[g->ofs + 1] = Mem_Strdup(va("%s_y", g->name));
				if (!c.gname[g->ofs + 2])
					c.gname[g->ofs + 2] = Mem_Strdup(va("%s_z", g->name));
			}
		}
	}

	SB_Printf(out, "// %s, decompiled by Bulwark\n// version %d, crc %d: %d functions, %d statements, %d globals, %d fields\n\n",
	          name, c.h.version, c.h.crc, c.h.numfunctions, c.h.numstatements, c.h.numglobals, c.h.numfielddefs);

	SB_AppendStr(out, "// fields\n");
	for (int i = 0; i < c.h.numfielddefs; i++) {
		const ddef_t *f = &c.fields[i];
		size_t nl = strlen(f->name);
		if (!f->name[0])
			continue;
		/* a vector field's components */
		if (i && nl > 2 && f->name[nl - 2] == '_' && f->name[nl - 1] >= 'x' && f->name[nl - 1] <= 'z' &&
		    (f->type & ~DEF_SAVEGLOBAL) == ev_float && (c.fields[i - 1].type & ~DEF_SAVEGLOBAL) != ev_float)
			continue;
		if (i && nl > 2 && f->name[nl - 2] == '_' && f->name[nl - 1] >= 'y' && f->name[nl - 1] <= 'z' &&
		    (f->type & ~DEF_SAVEGLOBAL) == ev_float)
			continue;
		SB_Printf(out, ".%s %s; // %u\n", type_name(f->type), f->name, f->ofs);
	}

	/* function parameters and locals are declared with their functions */
	islocal = Mem_Alloc((size_t)c.h.numglobals + 1);
	for (int i = 1; i < c.h.numfunctions; i++)
		for (int g = c.funcs[i].parm_start; g >= 0 && g < c.funcs[i].parm_start + c.funcs[i].locals && g < c.h.numglobals; g++)
			islocal[g] = 1;
	SB_AppendStr(out, "\n// globals\n");
	for (int i = 0; i < c.h.numglobaldefs; i++) {
		const ddef_t *g = &c.globals[i];
		uint32_t t = g->type & ~DEF_SAVEGLOBAL;
		size_t nl = strlen(g->name);
		if (!g->name[0] || !strcmp(g->name, "IMMEDIATE") || t == ev_function || t == ev_void ||
		    (g->ofs < (uint32_t)c.h.numglobals && islocal[g->ofs]))
			continue;
		/* a vector's _x, _y and _z component names */
		if (t == ev_float && nl > 2 && g->name[nl - 2] == '_' && g->name[nl - 1] >= 'x' && g->name[nl - 1] <= 'z') {
			int k = g->name[nl - 1] - 'x';
			const ddef_t *v = (int)g->ofs >= k ? c.gdef[g->ofs - k] : NULL;
			if (v && (v->type & ~DEF_SAVEGLOBAL) == ev_vector)
				continue;
		}
		if (t == ev_field) {
			/* field references are declared above */
			continue;
		}
		SB_Printf(out, "%s %s", type_name(g->type), g->name);
		if (g->ofs < (uint32_t)c.h.numglobals && c.gv[g->ofs]) {
			SB_AppendStr(out, " = ");
			value(&c, out, g->type, g->ofs);
		}
		SB_Printf(out, "; // %u%s\n", g->ofs, g->type & DEF_SAVEGLOBAL ? ", saved" : "");
	}

	/* jump targets */
	c.label = Mem_Alloc((size_t)c.h.numstatements + 1);
	for (int s = 0; s < c.h.numstatements; s++) {
		const dstatement32_t *st = &c.st[s];
		int32_t a = c.stmt32 ? (int32_t)st->a : (int16_t)st->a, b = c.stmt32 ? (int32_t)st->b : (int16_t)st->b;
		int32_t cc = c.stmt32 ? (int32_t)st->c : (int16_t)st->c, t = -1;
		if (st->op == OP_GOTO)
			t = s + a;
		else if (st->op == OP_IF || st->op == OP_IFNOT || st->op == OP_IF_F || st->op == OP_IFNOT_F ||
		         st->op == OP_IFS || st->op == OP_IFNOTS || (st->op >= OP_SWITCH_F && st->op <= OP_SWITCH_FNC) ||
		         st->op == OP_SWITCH_I || st->op == OP_CASE)
			t = s + b;
		else if (st->op == OP_CASERANGE)
			t = s + cc;
		if (t >= 0 && t < c.h.numstatements)
			c.label[t] = 1;
	}

	SB_AppendStr(out, "\n// functions\n");
	for (int i = 1; i < c.h.numfunctions; i++) {
		const dfunction_t *f = &c.funcs[i];
		int ofs = f->parm_start;
		SB_AppendStr(out, "\nvoid(");
		if (f->numparms < 0)
			SB_AppendStr(out, "...");
		for (int p = 0; p < f->numparms && p < 8; p++) {
			const char *pn = ofs >= 0 && ofs < c.h.numglobals && c.gname[ofs] ? c.gname[ofs] : NULL;
			const ddef_t *pd = ofs >= 0 && ofs < c.h.numglobals ? c.gdef[ofs] : NULL;
			SB_Printf(out, "%s%s %s", p ? ", " : "", pd && pn ? type_name(pd->type) : f->parm_size[p] == 3 ? "vector" : "__variant",
			          pn ? pn : va("p%d", p));
			ofs += f->parm_size[p];
		}
		SB_Printf(out, ") %s", str_at(&c, f->s_name));
		if (f->first_statement <= 0) {
			SB_Printf(out, " = #%d; // %s\n", -f->first_statement, str_at(&c, f->s_file));
			continue;
		}
		SB_Printf(out, " = // %s\n{\n", str_at(&c, f->s_file));
		/* locals past the parameters */
		for (int g = ofs; g < f->parm_start + f->locals && g < c.h.numglobals; g++) {
			const ddef_t *ld = c.gdef[g];
			if (ld && ld->ofs == (uint32_t)g && ld->name[0] && strcmp(ld->name, "IMMEDIATE")) {
				SB_Printf(out, "\tlocal %s %s;\n", type_name(ld->type), ld->name);
				if ((ld->type & ~DEF_SAVEGLOBAL) == ev_vector)
					g += 2;
			}
		}
		for (int s = f->first_statement; s < c.h.numstatements; s++) {
			if (c.label[s])
				SB_Printf(out, "L%d:\n", s);
			statement(&c, s);
			if (c.st[s].op == OP_DONE)
				break;
			/* the next function starts here: a function's code is contiguous */
			if (i + 1 < c.h.numfunctions && c.funcs[i + 1].first_statement == s + 1)
				break;
		}
		SB_AppendStr(out, "};\n");
	}

	for (int i = 0; i < c.h.numglobals + 3; i++)
		if (c.gname[i] && (c.gname[i] < c.strings || c.gname[i] > c.strings + c.h.numstrings))
			Mem_Free((void *)c.gname[i]);
	Mem_Free(c.gname);
	Mem_Free(c.gdef);
	Mem_Free(c.label);
	Mem_Free(c.isimm);
	Mem_Free(islocal);
	Mem_Free((void *)c.gv);
	Mem_Free(c.funcs);
	Mem_Free(c.globals);
	Mem_Free(c.fields);
	Mem_Free(c.st);
	Mem_Free(strings);
	return true;
}

static void Decompile_f(int argc, const char **argv)
{
	char name[128], base[128], path[192], ospath[512];
	strbuf_t out = {0};
	size_t len;
	uint8_t *d;
	FILE *f;
	Str_Copy(name, argc > 1 ? argv[1] : "progs.dat", sizeof(name));
	if (!strchr(name, '.'))
		Str_Cat(name, ".dat", sizeof(name));
	if (!(d = (uint8_t *)FS_LoadFile(name, &len))) {
		Con_Printf("decompile: couldn't load %s\n", name);
		return;
	}
	if (!QC_Decompile(name, d, len, &out)) {
		Con_Printf("decompile: %s is not a progs file this can read\n", name);
		Mem_Free(d);
		return;
	}
	Mem_Free(d);
	{
		const char *slash = strrchr(name, '/');
		Str_Copy(base, slash ? slash + 1 : name, sizeof(base));
		if (strrchr(base, '.'))
			*strrchr(base, '.') = 0;
	}
	snprintf(path, sizeof(path), "decompiled/%s.qc", base);
	if (!FS_WritePath(path, ospath, sizeof(ospath)) || !FS_CreatePath(ospath) || !(f = fopen(ospath, "wb"))) {
		Con_Printf("decompile: couldn't write %s\n", path);
		SB_Free(&out);
		return;
	}
	fwrite(out.data, 1, out.len, f);
	fclose(f);
	Con_Printf("Wrote %s (%zu bytes)\n", path, out.len);
	SB_Free(&out);
}

void SV_InitCompile(void)
{
	pr_sourcedir = Cvar_Get("pr_sourcedir", "src", 0, "Where the compile command looks for progs.src");
	Cmd_AddD("compile", Compile_f, "compile [file.src]: compile QuakeC with an external fteqcc");
	Cmd_AddD("applycompile", ApplyCompile_f, "Save the game and load it again with freshly compiled progs");
	Cmd_AddD("decompile", Decompile_f, "decompile [progs]: write decompiled/<progs>.qc as readable pseudo-QC");
}
