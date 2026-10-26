/* Bulwark engine - console commands and the command buffer. ISC licence, see LICENSE. */
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <stdio.h>

typedef struct cmd_s {
	char *name;
	cmd_fn fn;
	struct cmd_s *next;
} cmd_t;

static cmd_t *cmd_list;
static cmd_fallback_fn cmd_fallback;
static strbuf_t cbuf;
static const char *cmd_args = "";
static int cbuf_wait;

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
		if (*p == '"') {
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
			Mem_Free(c->name);
			Mem_Free(c);
			return;
		}
}

void Cmd_SetFallback(cmd_fallback_fn fn) { cmd_fallback = fn; }

const char *Cmd_Args(void) { return cmd_args; }

void Cmd_ExecuteString(const char *line)
{
	cmd_tokens_t local;
	const char *saved_args = cmd_args;
	Cmd_TokenizeInto(&local, line);
	if (!local.argc)
		return;
	cmd_args = local.argc > 1 ? line + local.argstart[1] : "";

	for (cmd_t *c = cmd_list; c; c = c->next) {
		if (!Str_ICmp(c->name, local.argv[0])) {
			c->fn(local.argc, local.argv);
			cmd_args = saved_args;
			return;
		}
	}
	if (cmd_fallback && cmd_fallback(line, local.argc, local.argv)) {
		cmd_args = saved_args;
		return;
	}
	if (!Cvar_Command(local.argc, local.argv))
		Con_Printf("unknown command \"%s\"\n", local.argv[0]);
	cmd_args = saved_args;
}

void Cbuf_AddText(const char *text) { SB_AppendStr(&cbuf, text); }

void Cbuf_InsertText(const char *text)
{
	strbuf_t n = {0};
	SB_AppendStr(&n, text);
	if (n.len && n.data[n.len - 1] != '\n')
		SB_Append(&n, "\n", 1);
	if (cbuf.len)
		SB_Append(&n, cbuf.data, cbuf.len);
	SB_Free(&cbuf);
	cbuf = n;
}

void Cbuf_Execute(void)
{
	char line[8192];
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
		Cmd_ExecuteString(line);
	}
	cbuf_wait = 0;
}

static void Cmd_Echo_f(int argc, const char **argv)
{
	(void)argv;
	if (argc > 1)
		Con_Printf("%s\n", Cmd_Args());
	else
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

static void Cmd_Wait_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	cbuf_wait = 1;
}

static void Cmd_List_f(int argc, const char **argv)
{
	int n = 0;
	for (cmd_t *c = cmd_list; c; c = c->next) {
		if (argc > 1 && !Str_WildMatch(argv[1], c->name, true))
			continue;
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
}
