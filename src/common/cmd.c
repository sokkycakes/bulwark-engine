/* Bulwark engine - console commands and the command buffer. ISC licence, see LICENSE. */
#include "cmd.h"
#include "manifest.h"
#include "worker.h"
#include "cvar.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct cmd_s {
	char *name;
	char *description;
	cmd_fn fn;
	int level; /* the lowest execution level allowed to run it ("restrict") */
	struct cmd_s *next;
} cmd_t;

typedef struct alias_s {
	char *name, *text;
	int level; /* the level its text runs at, -1 for its caller's ("aliaslevel") */
	struct alias_s *next;
} alias_t;

static cmd_t *cmd_list;
static alias_t *alias_list;
static cmd_fallback_fn cmd_fallback;
static cmd_fallback_fn cmd_prehook;
static strbuf_t cbuf;
static const char *cmd_args = "";
static int cbuf_wait;

/* Execution levels: the local console runs at RESTRICT_MAX, rcon and ranked
 * players lower. Text queued at a level other than the buffer's is fenced
 * with "__lvl <key> <level>" lines, so it runs at the level that queued it;
 * the key is secret so other text can't raise its own level. cbuf_level is
 * the level the buffer is at as it runs, tail_level the one at its end. */
static int cmd_execlevel = RESTRICT_MAX;
static int cbuf_level = RESTRICT_MAX, tail_level = RESTRICT_MAX;
static char lvl_key[24];

int Cmd_ExecLevel(void) { return cmd_execlevel; }

static const char *lvl_fence(int level)
{
	if (!lvl_key[0])
		snprintf(lvl_key, sizeof(lvl_key), "%08x%08x", (unsigned)rand() ^ (unsigned)(Sys_Seconds() * 1000003),
		         (unsigned)(size_t)&cmd_execlevel ^ (unsigned)rand());
	return va("__lvl %s %d\n", lvl_key, level);
}

/* a buffer line that sets the level; true if the line was one */
static bool lvl_line(const char *line)
{
	size_t n = strlen(lvl_key);
	if (strncmp(line, "__lvl ", 6))
		return false;
	if (n && !strncmp(line + 6, lvl_key, n) && line[6 + n] == ' ')
		cbuf_level = BW_CLAMP(atoi(line + 7 + n), 0, RESTRICT_MAX);
	return true;
}

int Cmd_Restriction(const char *name)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name))
			return c->level;
	return -1;
}

bool Cmd_SetRestriction(const char *name, int level)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name)) {
			c->level = BW_CLAMP(level, 0, RESTRICT_MAX);
			return true;
		}
	return false;
}

bool Cmd_AliasLevel(const char *name, int *level)
{
	for (alias_t *a = alias_list; a; a = a->next)
		if (!Str_ICmp(a->name, name)) {
			*level = a->level;
			return true;
		}
	return false;
}

bool Cmd_SetAliasLevel(const char *name, int level)
{
	for (alias_t *a = alias_list; a; a = a->next)
		if (!Str_ICmp(a->name, name)) {
			a->level = level < 0 ? -1 : BW_MIN(level, RESTRICT_MAX);
			return true;
		}
	return false;
}

void Cmd_ExecuteStringAtLevel(const char *line, int level)
{
	int saved = cmd_execlevel;
	cmd_execlevel = BW_CLAMP(level, 0, saved);
	Cmd_ExecuteString(line);
	cmd_execlevel = saved;
}

void Cmd_TokenizeInto(cmd_tokens_t *t, const char *text)
{
	size_t used = 0;
	const char *p = text;
	t->argc = 0;
	Str_Copy(t->source, text, sizeof(t->source));
	while (*p && t->argc < CMD_MAX_ARGS) {
		const char *start;
		char *out = t->store + used;
		size_t room = sizeof(t->store) - used;
		size_t n = 0;
		while (*p && (unsigned char)*p <= ' ' && *p != '\n')
			p++;
		if (!*p || *p == '\n')
			break;
		if (p[0] == '/' && p[1] == '/')
			break;
		start = p;
		if (p[0] == '\\' && p[1] == '"') {
			n = Str_ParseEscaped(&p, out, room);
		} else if (*p == '"') {
			p++;
			while (*p && *p != '"' && *p != '\n') {
				if (n + 1 < room)
					out[n++] = *p;
				p++;
			}
			if (*p == '"')
				p++;
		} else {
			while ((unsigned char)*p > ' ') {
				if (n + 1 < room)
					out[n++] = *p;
				p++;
			}
		}
		if (room == 0)
			break;
		out[n] = 0;
		t->argstart[t->argc] = (int)(start - text);
		t->argend[t->argc] = (int)(p - text);
		t->argv[t->argc++] = out;
		used += n + 1;
	}
}

void Cmd_Add(const char *name, cmd_fn fn)
{
	cmd_t *c;
	for (c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name)) {
			c->fn = fn;
			return;
		}
	c = Mem_Alloc(sizeof(*c));
	c->name = Mem_Strdup(name);
	c->fn = fn;
	c->next = cmd_list;
	cmd_list = c;
}

void Cmd_AddD(const char *name, cmd_fn fn, const char *description)
{
	Cmd_Add(name, fn);
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name)) {
			Mem_Free(c->description);
			c->description = description ? Mem_Strdup(description) : NULL;
		}
}

/* gives a command a description if it has none */
void Cmd_Describe(const char *name, const char *description)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name) && !c->description)
			c->description = Mem_Strdup(description);
}

const char *Cmd_Description(const char *name)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name))
			return c->description;
	return NULL;
}

void Cmd_ForEach(cmd_walk_fn fn, void *ctx)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		fn(c->name, c->description, ctx);
}

void Cmd_ForEachAlias(cmd_walk_fn fn, void *ctx)
{
	for (alias_t *a = alias_list; a; a = a->next)
		fn(a->name, a->text, ctx);
}

const char *Cmd_AliasText(const char *name)
{
	for (alias_t *a = alias_list; a; a = a->next)
		if (!Str_ICmp(a->name, name))
			return a->text;
	return NULL;
}

void Cmd_WriteAliases(strbuf_t *sb)
{
	for (alias_t *a = alias_list; a; a = a->next) {
		SB_Printf(sb, "alias %s \"", a->name);
		for (const char *p = a->text; *p; p++)
			if (*p != '"' && *p != '\n')
				SB_Append(sb, p, 1);
		SB_AppendStr(sb, "\"\n");
	}
}

bool Cmd_Exists(const char *name)
{
	for (cmd_t *c = cmd_list; c; c = c->next)
		if (!Str_ICmp(c->name, name))
			return true;
	return false;
}

void Cmd_Remove(const char *name)
{
	for (cmd_t **l = &cmd_list; *l; l = &(*l)->next)
		if (!Str_ICmp((*l)->name, name)) {
			cmd_t *c = *l;
			*l = c->next;
			Mem_Free(c->description);
			Mem_Free(c->name);
			Mem_Free(c);
			return;
		}
}

void Cmd_SetFallback(cmd_fallback_fn fn) { cmd_fallback = fn; }
void Cmd_SetPreHook(cmd_fallback_fn fn) { cmd_prehook = fn; }

const char *Cmd_Args(void) { return cmd_args; }

static cmd_macro_fn macro_provider;
void Cmd_SetMacroProvider(cmd_macro_fn fn) { macro_provider = fn; }

/* a cvar or macro reference starting at p ("$name" or "${name}"); returns
 * its length, with its text in *out */
static size_t macro_at(const char *p, const char **out)
{
	cvar_t *v;
	char name[128];
	size_t n = 0, len;
	if (p[0] != '$')
		return 0;
	if (p[1] == '{') {
		const char *e = strchr(p + 2, '}');
		if (!e || (size_t)(e - p - 2) >= sizeof(name))
			return 0;
		memcpy(name, p + 2, (size_t)(e - p - 2));
		name[e - p - 2] = 0;
		len = (size_t)(e - p) + 1;
	} else {
		while ((unsigned char)p[1 + n] > ' ' && p[1 + n] != '"' && p[1 + n] != ';' && p[1 + n] != '$' && n + 1 < sizeof(name)) {
			name[n] = p[1 + n];
			n++;
		}
		name[n] = 0;
		len = n + 1;
	}
	if (!name[0])
		return 0;
	if ((v = Cvar_Find(name)))
		*out = v->string;
	else if (!macro_provider || !(*out = macro_provider(name)))
		return 0;
	return len;
}

bool Cmd_ExpandMacros(const char *in, char *out, size_t outsize)
{
	bool any = false, quoted = false;
	size_t o = 0;
	for (const char *p = in; *p && o + 1 < outsize;) {
		const char *v;
		size_t n;
		if (*p == '"')
			quoted = !quoted;
		/* "$$" stands for a lone $ */
		if (p[0] == '$' && p[1] == '$') {
			out[o++] = '$';
			p += 2;
			any = true;
			continue;
		}
		if (!quoted && (n = macro_at(p, &v))) {
			o += Str_Copy(out + o, v, outsize - o);
			if (o >= outsize)
				o = outsize - 1;
			p += n;
			any = true;
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = 0;
	return any;
}

/* commands that read $ references themselves, or store text to run later */
static bool keeps_macros(const char *name)
{
	static const char *const names[] = {"alias", "bind", "if", "set_calc", "seta_calc", "defer", "in_bind",
	                                    "bindtoggle", "newalias", NULL};
	for (int i = 0; names[i]; i++)
		if (!Str_ICmp(names[i], name))
			return true;
	return false;
}

/* an alias's text with $1.. $9 and $* replaced by the arguments it was run with */
static void alias_args(const char *text, const cmd_tokens_t *t, const char *line, strbuf_t *out)
{
	for (const char *p = text; *p; p++) {
		if (p[0] == '$' && p[1] >= '1' && p[1] <= '9') {
			int i = p[1] - '0';
			if (i < t->argc)
				SB_AppendStr(out, t->argv[i]);
			p++;
			continue;
		}
		if (p[0] == '$' && p[1] == '*') {
			if (t->argc > 1)
				SB_AppendStr(out, line + t->argstart[1]);
			p++;
			continue;
		}
		SB_Append(out, p, 1);
	}
}

void Cmd_ExecuteString(const char *line)
{
	static int depth;
	cmd_tokens_t local;
	char expanded[8192];
	const char *saved_args = cmd_args;
	Cmd_TokenizeInto(&local, line);
	if (!local.argc)
		return;
	if (!keeps_macros(local.argv[0]) && strchr(line, '$') && depth < 4 &&
	    Cmd_ExpandMacros(line, expanded, sizeof(expanded))) {
		depth++;
		Cmd_ExecuteString(expanded);
		depth--;
		return;
	}
	if (!strcmp(local.argv[0], "__lvl"))
		return; /* only the buffer acts on these */
	cmd_args = local.argc > 1 ? line + local.argstart[1] : "";

	/* commands the game registered come first, so it can override ours */
	if (cmd_prehook && cmd_prehook(line, local.argc, local.argv)) {
		cmd_args = saved_args;
		return;
	}
	for (cmd_t *c = cmd_list; c; c = c->next) {
		if (!Str_ICmp(c->name, local.argv[0])) {
			if (c->level > cmd_execlevel)
				Con_Printf("%s was restricted (level %d, you have %d)\n", c->name, c->level, cmd_execlevel);
			else
				c->fn(local.argc, local.argv);
			cmd_args = saved_args;
			return;
		}
	}
	/* an alias runs its text in place of the line */
	for (alias_t *a = alias_list; a; a = a->next) {
		if (!Str_ICmp(a->name, local.argv[0])) {
			strbuf_t sb = {0};
			if (local.argc > 1 && strchr(a->text, '$'))
				alias_args(a->text, &local, line, &sb);
			else
				SB_AppendStr(&sb, a->text);
			if (a->level >= 0 && a->level < cmd_execlevel) {
				/* its own level, never above the caller's */
				int saved = cmd_execlevel;
				cmd_execlevel = a->level;
				Cbuf_InsertText(sb.data ? sb.data : "");
				cmd_execlevel = saved;
			} else {
				Cbuf_InsertText(sb.data ? sb.data : "");
			}
			SB_Free(&sb);
			cmd_args = saved_args;
			return;
		}
	}
	if (Cvar_Command(local.argc, local.argv)) {
		cmd_args = saved_args;
		return;
	}
	if (!cmd_fallback || !cmd_fallback(line, local.argc, local.argv))
		Con_Printf("unknown command \"%s\"\n", local.argv[0]);
	cmd_args = saved_args;
}

void Cbuf_AddText(const char *text)
{
	if (cmd_execlevel != tail_level) {
		SB_AppendStr(&cbuf, lvl_fence(cmd_execlevel));
		tail_level = cmd_execlevel;
	}
	SB_AppendStr(&cbuf, text);
}

void Cbuf_AddTextAtLevel(const char *text, int level)
{
	int saved = cmd_execlevel;
	cmd_execlevel = BW_CLAMP(level, 0, saved);
	Cbuf_AddText(text);
	cmd_execlevel = saved;
}

void Cbuf_InsertText(const char *text)
{
	strbuf_t n = {0};
	bool fence = cmd_execlevel != cbuf_level;
	if (fence)
		SB_AppendStr(&n, lvl_fence(cmd_execlevel));
	SB_AppendStr(&n, text);
	if (n.len && n.data[n.len - 1] != '\n')
		SB_Append(&n, "\n", 1);
	if (fence)
		SB_AppendStr(&n, lvl_fence(cbuf_level));
	if (!cbuf.len)
		tail_level = cbuf_level;
	if (cbuf.len)
		SB_Append(&n, cbuf.data, cbuf.len);
	SB_Free(&cbuf);
	cbuf = n;
}

typedef struct deferred_s {
	double when;
	int level;
	char *text;
	struct deferred_s *next;
} deferred_t;
static deferred_t *deferred;

void Cbuf_AddDeferred(double seconds, const char *text)
{
	deferred_t *d = Mem_Alloc(sizeof(*d));
	d->when = Sys_Seconds() + seconds;
	d->text = Mem_Strdup(text);
	d->level = cmd_execlevel;
	d->next = deferred;
	deferred = d;
}

static void run_deferred(void)
{
	double now = Sys_Seconds();
	for (deferred_t **l = &deferred; *l;) {
		deferred_t *d = *l;
		if (d->when > now) {
			l = &d->next;
			continue;
		}
		*l = d->next;
		{
			int saved = cmd_execlevel;
			cmd_execlevel = d->level;
			Cbuf_AddText(d->text);
			Cbuf_AddText("\n");
			cmd_execlevel = saved;
		}
		Mem_Free(d->text);
		Mem_Free(d);
	}
}

void Cbuf_Execute(void)
{
	char line[8192];
	if (deferred)
		run_deferred();
	Cmd_CheckLogfile();
	if (cbuf_wait > 0) {
		cbuf_wait--;
		return;
	}
	while (cbuf.len && !cbuf_wait) {
		size_t i, n;
		int quotes = 0;
		for (i = 0; i < cbuf.len; i++) {
			char ch = cbuf.data[i];
			if (ch == '"')
				quotes ^= 1;
			if (ch == '\n' || (ch == ';' && !quotes))
				break;
		}
		n = i < sizeof(line) - 1 ? i : sizeof(line) - 1;
		memcpy(line, cbuf.data, n);
		line[n] = 0;
		if (i < cbuf.len)
			i++;
		memmove(cbuf.data, cbuf.data + i, cbuf.len - i);
		cbuf.len -= i;
		cbuf.data[cbuf.len] = 0;
		if (!lvl_line(line)) {
			int saved = cmd_execlevel;
			cmd_execlevel = cbuf_level;
			Cmd_ExecuteString(line);
			cmd_execlevel = saved;
		}
	}
	if (!cbuf.len)
		tail_level = cbuf_level;
	/* "wait" paused the buffer: this frame counts as the first one waited */
	if (cbuf_wait > 0)
		cbuf_wait--;
}

static void Cmd_Echo_f(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++)
		Con_Printf(i + 1 < argc ? "%s " : "%s", argv[i]);
	Con_Printf("\n");
}

static void Cmd_Exec_f(int argc, const char **argv)
{
	size_t len;
	char *text;
	if (argc < 2) {
		Con_Printf("usage: exec <file>\n");
		return;
	}
	text = FS_LoadFile(argv[1], &len);
	if (!text) {
		Con_Printf("couldn't exec %s\n", argv[1]);
		return;
	}
	Con_DPrintf("execing %s\n", argv[1]);
	Cbuf_InsertText(text);
	Mem_Free(text);
}

/* "wait [frames]" holds the rest of the buffer for that many frames */
static void Cmd_Wait_f(int argc, const char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1;
	cbuf_wait = n > 0 ? n : 1;
}

void Cmd_SetAlias(const char *name, const char *text)
{
	alias_t *a;
	for (a = alias_list; a; a = a->next)
		if (!Str_ICmp(a->name, name))
			break;
	if (!a) {
		a = Mem_Alloc(sizeof(*a));
		a->level = -1;
		a->name = Mem_Strdup(name);
		a->next = alias_list;
		alias_list = a;
	} else {
		Mem_Free(a->text);
	}
	a->text = Mem_Strdup(text);
}

/* "alias name "commands"": with no text it prints the alias, with no name all */
static void Cmd_Alias_f(int argc, const char **argv)
{
	alias_t *a;
	if (argc < 2) {
		for (a = alias_list; a; a = a->next)
			Con_Printf("%s : %s\n", a->name, a->text);
		return;
	}
	for (a = alias_list; a; a = a->next)
		if (!Str_ICmp(a->name, argv[1]))
			break;
	if (argc < 3) {
		if (a)
			Con_Printf("%s : %s\n", a->name, a->text);
		return;
	}
	if (!a) {
		a = Mem_Alloc(sizeof(*a));
		a->level = -1;
		a->name = Mem_Strdup(argv[1]);
		a->next = alias_list;
		alias_list = a;
	} else {
		Mem_Free(a->text);
	}
	/* the rest of the line, so unquoted multi-word text works too */
	{
		const char *rest = Cmd_Args();
		while (*rest && (unsigned char)*rest > ' ')
			rest++;
		while (*rest && (unsigned char)*rest <= ' ')
			rest++;
		if (argc == 3)
			a->text = Mem_Strdup(argv[2]);
		else
			a->text = Mem_Strdup(rest);
	}
}

static void Cmd_Unalias_f(int argc, const char **argv)
{
	for (alias_t **l = &alias_list; argc > 1 && *l; l = &(*l)->next)
		if (!Str_ICmp((*l)->name, argv[1])) {
			alias_t *a = *l;
			*l = a->next;
			Mem_Free(a->name);
			Mem_Free(a->text);
			Mem_Free(a);
			return;
		}
}

static void Cmd_List_f(int argc, const char **argv)
{
	int n = 0;
	for (cmd_t *c = cmd_list; c; c = c->next) {
		if (argc > 1 && !Str_WildMatch(argv[1], c->name, true) && !Str_WildMatch(va("%s*", argv[1]), c->name, true))
			continue;
		if (c->description)
			Con_Printf("%-24s %s\n", c->name, c->description);
		else
			Con_Printf("%s\n", c->name);
		n++;
	}
	Con_Printf("%d commands\n", n);
}

void Cmd_Init(void)
{
	Cmd_Add("echo", Cmd_Echo_f);
	Cmd_Add("exec", Cmd_Exec_f);
	Cmd_Add("wait", Cmd_Wait_f);
	Cmd_Add("cmdlist", Cmd_List_f);
	Cmd_Add("alias", Cmd_Alias_f);
	Cmd_Add("unalias", Cmd_Unalias_f);
	Cmd_InitExtra();
	Cmd_InitCompat();
	Mf_Init();
	Worker_Init();
	Plug_Init();
	Terrain_Init();
}
