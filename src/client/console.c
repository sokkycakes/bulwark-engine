/* Bulwark engine - drop-down console text and line editing.
 * ISC licence, see LICENSE. */
#include <stdio.h>
#include <string.h>

#include "../common/cmd.h"
#include "../common/common.h"
#include "../common/cvar.h"
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
static void Con_Clear_f(int argc, const char **argv)
{
	memset(lines, 0, sizeof(lines));
	head = 0;
}

static void complete(void)
{
	/* complete the first word against commands and cvars */
	size_t n = strlen(input);
	const char *match = NULL;
	int count = 0;
	if (!n || strchr(input, ' '))
		return;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (!Str_NICmp(v->name, input, n)) {
			if (!match)
				match = v->name;
			count++;
		}
	if (count == 1) {
		snprintf(input, sizeof(input), "%s ", match);
		cursor = (int)strlen(input);
	} else if (count > 1) {
		Con_Printf("]%s\n", input);
		for (cvar_t *v = Cvar_First(); v; v = v->next)
			if (!Str_NICmp(v->name, input, n))
				Con_Printf("  %s \"%s\"\n", v->name, v->string);
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
}
