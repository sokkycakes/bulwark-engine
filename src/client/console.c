/* Bulwark engine - drop-down console text and line editing.
 * ISC licence, see LICENSE. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "../common/cmd.h"
#include "../common/common.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "console.h"
#include "keys.h"

static char lines[CON_LINES][256];
static double linetimes[CON_LINES];
static int head; /* index of the line being written */
static char input[CON_INPUT];
static int cursor;
static char history[32][CON_INPUT];
static int hist_head, hist_pos;
static bool active;
static int scroll;

static void new_line(void)
{
	head = (head + 1) % CON_LINES;
	lines[head][0] = 0;
	linetimes[head] = host_time;
}

/* receives everything printed (Con_Print also writes stdout) */
static void con_hook(const char *text)
{
	for (const char *p = text; *p; p++) {
		if (*p == '\n') {
			new_line();
			continue;
		}
		{
			size_t n = strlen(lines[head]);
			if (n >= sizeof(lines[0]) - 1) {
				new_line();
				n = 0;
			}
			lines[head][n] = *p;
			lines[head][n + 1] = 0;
			linetimes[head] = host_time;
		}
	}
}

const char *Con_Line(int back)
{
	if (back < 0 || back >= CON_LINES - 1)
		return NULL;
	return lines[(head - back + CON_LINES) % CON_LINES];
}

double Con_LineTime(int back) { return linetimes[(head - back + CON_LINES) % CON_LINES]; }
const char *Con_InputLine(int *c)
{
	if (c)
		*c = cursor;
	return input;
}
int Con_Scroll(void) { return scroll; }
bool Con_IsActive(void) { return active; }
void Con_SetActive(bool a) { active = a; }
void Con_ToggleConsole(void) { active = !active; }

static void Con_Toggle_f(int argc, const char **argv) { Con_ToggleConsole(); }
static void Con_Close_f(int argc, const char **argv) { active = false; }
static void Con_Show_f(int argc, const char **argv) { active = true; }

/* condump <file>: the scrollback, oldest first, without colour codes */
static void Con_Dump_f(int argc, const char **argv)
{
	char rel[256], path[1024];
	FILE *f;
	int first = -1;
	if (argc < 2) {
		Con_Printf("usage: condump <file>\n");
		return;
	}
	snprintf(rel, sizeof(rel), "%s%s", argv[1], strchr(argv[1], '.') ? "" : ".txt");
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("condump: can't write %s\n", rel);
		return;
	}
	for (int back = CON_LINES - 2; back >= 0; back--)
		if (Con_Line(back) && Con_Line(back)[0]) {
			first = back;
			break;
		}
	for (int back = first; back >= 0; back--) {
		const char *l = Con_Line(back);
		for (const char *p = l; *p; p++) {
			if (*p == '^' && p[1] >= '0' && p[1] <= '9') {
				p++;
				continue;
			}
			fputc(*p, f);
		}
		fputc('\n', f);
	}
	fclose(f);
	Con_Printf("dumped console text to %s\n", rel);
}

/* --- chat input (messagemode) --------------------------------------------------- */

static int chat_mode; /* 0 none, 1 say, 2 say_team */
static char chat_input[CON_INPUT];
static int chat_cursor;

int Con_ChatMode(void) { return chat_mode; }
const char *Con_ChatInput(void) { return chat_input; }

static void open_chat(int mode)
{
	chat_mode = mode;
	chat_input[0] = 0;
	chat_cursor = 0;
}

static void Con_MessageMode_f(int argc, const char **argv) { open_chat(1); }
static void Con_MessageMode2_f(int argc, const char **argv) { open_chat(2); }

void Con_ChatKey(int key, int unicode)
{
	size_t len = strlen(chat_input);
	switch (key) {
	case K_ENTER:
	case K_KP_ENTER:
		if (chat_input[0]) {
			char cmd[CON_INPUT + 32];
			/* the text goes over as one quoted argument */
			snprintf(cmd, sizeof(cmd), "%s \"", chat_mode == 2 ? "say_team" : "say");
			for (const char *p = chat_input; *p; p++)
				if (*p != '"') {
					size_t n = strlen(cmd);
					if (n + 3 < sizeof(cmd)) {
						cmd[n] = *p;
						cmd[n + 1] = 0;
					}
				}
			Str_Cat(cmd, "\"\n", sizeof(cmd));
			Cbuf_AddText(cmd);
		}
		chat_mode = 0;
		return;
	case K_ESCAPE:
		chat_mode = 0;
		return;
	case K_BACKSPACE:
		if (chat_cursor > 0) {
			memmove(chat_input + chat_cursor - 1, chat_input + chat_cursor, len - (size_t)chat_cursor + 1);
			chat_cursor--;
		}
		return;
	case K_DEL:
		if ((size_t)chat_cursor < len)
			memmove(chat_input + chat_cursor, chat_input + chat_cursor + 1, len - (size_t)chat_cursor);
		return;
	case K_LEFTARROW:
		if (chat_cursor > 0)
			chat_cursor--;
		return;
	case K_RIGHTARROW:
		if ((size_t)chat_cursor < len)
			chat_cursor++;
		return;
	case K_HOME:
		chat_cursor = 0;
		return;
	case K_END:
		chat_cursor = (int)len;
		return;
	default:
		break;
	}
	if (unicode >= 32 && unicode < 127 && len + 1 < CON_INPUT) {
		memmove(chat_input + chat_cursor + 1, chat_input + chat_cursor, len - (size_t)chat_cursor + 1);
		chat_input[chat_cursor++] = (char)unicode;
	}
}
static void Con_Clear_f(int argc, const char **argv)
{
	memset(lines, 0, sizeof(lines));
	head = 0;
}

typedef struct {
	const char *prefix;
	size_t n;
	int count;
	char common[CON_INPUT]; /* the longest start every match shares */
	bool print;
} completion_t;

static void consider(completion_t *c, const char *name)
{
	if (Str_NICmp(name, c->prefix, c->n))
		return;
	if (!c->count++) {
		Str_Copy(c->common, name, sizeof(c->common));
	} else {
		size_t i = 0;
		while (c->common[i] && name[i] && tolower((unsigned char)c->common[i]) == tolower((unsigned char)name[i]))
			i++;
		c->common[i] = 0;
	}
}

static void consider_cmd(const char *name, const char *text, void *ctx)
{
	completion_t *c = ctx;
	if (c->print) {
		if (!Str_NICmp(name, c->prefix, c->n))
			Con_Printf("  %s\n", name);
	} else {
		consider(c, name);
	}
}

static void complete(void)
{
	/* complete the first word against commands, aliases and cvars */
	completion_t c = {input, strlen(input), 0, "", false};
	if (!c.n || strchr(input, ' '))
		return;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		consider(&c, v->name);
	Cmd_ForEach(consider_cmd, &c);
	Cmd_ForEachAlias(consider_cmd, &c);
	if (c.count == 1) {
		snprintf(input, sizeof(input), "%s ", c.common);
		cursor = (int)strlen(input);
	} else if (c.count > 1) {
		Con_Printf("]%s\n", input);
		for (cvar_t *v = Cvar_First(); v; v = v->next)
			if (!Str_NICmp(v->name, input, c.n))
				Con_Printf("  %s \"%s\"\n", v->name, v->string);
		c.print = true;
		Cmd_ForEach(consider_cmd, &c);
		Cmd_ForEachAlias(consider_cmd, &c);
		if (strlen(c.common) > c.n) {
			Str_Copy(input, c.common, sizeof(input));
			cursor = (int)strlen(input);
		}
	}
}

void Con_KeyEvent(int key, int unicode)
{
	size_t len = strlen(input);
	switch (key) {
	case K_ENTER:
	case K_KP_ENTER:
		Con_Printf("]%s\n", input);
		if (input[0]) {
			Str_Copy(history[hist_head++ % 32], input, CON_INPUT);
			hist_pos = hist_head;
			Cbuf_AddText(input);
			Cbuf_AddText("\n");
		}
		input[0] = 0;
		cursor = 0;
		scroll = 0;
		return;
	case K_BACKSPACE:
		if (cursor > 0) {
			memmove(input + cursor - 1, input + cursor, len - (size_t)cursor + 1);
			cursor--;
		}
		return;
	case K_DEL:
		if ((size_t)cursor < len)
			memmove(input + cursor, input + cursor + 1, len - (size_t)cursor);
		return;
	case K_LEFTARROW:
		if (cursor > 0)
			cursor--;
		return;
	case K_RIGHTARROW:
		if ((size_t)cursor < len)
			cursor++;
		return;
	case K_HOME:
		cursor = 0;
		return;
	case K_END:
		cursor = (int)len;
		return;
	case K_UPARROW:
		if (hist_pos > 0 && hist_pos > hist_head - 32) {
			hist_pos--;
			Str_Copy(input, history[hist_pos % 32], CON_INPUT);
			cursor = (int)strlen(input);
		}
		return;
	case K_DOWNARROW:
		if (hist_pos < hist_head) {
			hist_pos++;
			if (hist_pos == hist_head)
				input[0] = 0;
			else
				Str_Copy(input, history[hist_pos % 32], CON_INPUT);
			cursor = (int)strlen(input);
		}
		return;
	case K_PGUP:
	case K_MWHEELUP:
		scroll = BW_MIN(scroll + 4, CON_LINES - 64);
		return;
	case K_PGDN:
	case K_MWHEELDOWN:
		scroll = BW_MAX(scroll - 4, 0);
		return;
	case K_TAB:
		complete();
		return;
	default:
		break;
	}
	if (unicode >= 32 && unicode < 127 && len + 1 < CON_INPUT) {
		memmove(input + cursor + 1, input + cursor, len - (size_t)cursor + 1);
		input[cursor++] = (char)unicode;
	}
}

void Con_Init(void)
{
	Con_SetHook(con_hook);
	Cmd_Add("toggleconsole", Con_Toggle_f);
	Cmd_Add("clear", Con_Clear_f);
	Cmd_AddD("conclear", Con_Clear_f, "Clear the console text");
	Cmd_AddD("conclose", Con_Close_f, "Close the console");
	Cmd_AddD("hideconsole", Con_Close_f, "Close the console");
	Cmd_AddD("showconsole", Con_Show_f, "Open the console");
	Cmd_AddD("conactivate", Con_Show_f, "Open the console");
	Cmd_AddD("condump", Con_Dump_f, "condump <file>: write the console text to a file");
	Cmd_AddD("messagemode", Con_MessageMode_f, "Type a chat message to everyone");
	Cmd_AddD("messagemode2", Con_MessageMode2_f, "Type a chat message to your team");
}
