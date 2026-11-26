/* Bulwark engine - drop-down console text and line editing.
 * ISC licence, see LICENSE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../common/cmd.h"
#include "../common/common.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "client.h"
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
static cvar_t *con_suggestions;

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

/* --- completion ------------------------------------------------------------------ */

/* Tab completes the word at the cursor: a command, cvar or alias at the
 * start of a command (after a ';' too), or an argument whose kind the
 * command is known to take (a map, a config file, a cvar, a key...).
 * Pressing it again steps through the matches, Shift+Tab steps back. The
 * matches for the word being typed are listed under the input line. */

enum { A_NONE, A_COMMAND, A_CVAR, A_ALIAS, A_KEY, A_MAP, A_CFG, A_DEMO, A_SAVE, A_SOUND, A_MODEL, A_SKY };

static const struct {
	const char *cmd;
	int arg, kind;
} argkinds[] = {
	{"map", 1, A_MAP},          {"changelevel", 1, A_MAP},  {"devmap", 1, A_MAP},
	{"mapviewer", 1, A_MAP},    {"exec", 1, A_CFG},         {"cfg_load", 1, A_CFG},     {"playdemo", 1, A_DEMO},
	{"timedemo", 1, A_DEMO},    {"set", 1, A_CVAR},         {"seta", 1, A_CVAR},        {"sets", 1, A_CVAR},
	{"set_calc", 1, A_CVAR},    {"toggle", 1, A_CVAR},      {"inc", 1, A_CVAR},         {"reset", 1, A_CVAR},
	{"cvarreset", 1, A_CVAR},   {"cvar_reset", 1, A_CVAR},  {"cycle", 1, A_CVAR},       {"vstr", 1, A_CVAR},
	{"help", 1, A_COMMAND},     {"bind", 1, A_KEY},         {"unbind", 1, A_KEY},       {"bindlevel", 1, A_KEY},
	{"bind", 2, A_COMMAND},     {"unalias", 1, A_ALIAS},    {"load", 1, A_SAVE},        {"save", 1, A_SAVE},
	{"play", 1, A_SOUND},       {"play2", 1, A_SOUND},      {"playsound", 1, A_SOUND},  {"modelviewer", 1, A_MODEL},
	{"sky", 1, A_SKY},          {"loadsky", 1, A_SKY},
};

#define MAX_MATCHES 2048

typedef struct {
	char word[CON_INPUT]; /* the part already typed */
	size_t n;
	int kind;
	int start; /* where the word starts in the input */
	char match[MAX_MATCHES][96];
	int num;
	char hint[160]; /* the command's description, when there's nothing to list */
} completion_t;

#ifdef BW_LOWMEM
/* 192 KB, taken the first time the console is open */
static completion_t *comp_p;
static completion_t *comp_get(void)
{
	if (!comp_p)
		comp_p = Mem_Alloc(sizeof(*comp_p));
	return comp_p;
}
#define comp (*comp_get())
#else
static completion_t comp;
#endif
static char comp_for[CON_INPUT] = "\1"; /* the input comp was made for */
static int comp_cursor = -1;
static int cycle = -1, cycle_end; /* the match Tab put in, and where it ends */

static void add_match(const char *name)
{
	char clean[96];
	if (Str_NICmp(name, comp.word, comp.n) || comp.num >= MAX_MATCHES)
		return;
	Str_Copy(clean, name, sizeof(clean));
	for (int i = 0; i < comp.num; i++)
		if (!Str_ICmp(comp.match[i], clean))
			return;
	Str_Copy(comp.match[comp.num++], clean, sizeof(comp.match[0]));
}

static void match_cmd(const char *name, const char *text, void *ctx) { add_match(name); }
static void match_key(const char *name, void *ctx) { add_match(name); }

typedef struct {
	const char *strip; /* leading folder to take off */
	const char *suffix; /* ending to take off ("rt" for sky faces), with the extension */
} filenames_t;

static void match_file(const char *name, const char *searchpath, void *ctx)
{
	const filenames_t *f = ctx;
	char n[96];
	size_t len = strlen(f->strip);
	if (!Str_NICmp(name, f->strip, len))
		name += len;
	Str_Copy(n, name, sizeof(n));
	if (f->suffix) {
		char *dot = strrchr(n, '.');
		if (dot)
			*dot = 0;
		if (f->suffix[0] && strlen(n) > strlen(f->suffix) &&
		    !Str_ICmp(n + strlen(n) - strlen(f->suffix), f->suffix)) {
			n[strlen(n) - strlen(f->suffix)] = 0;
			if (n[0] && n[strlen(n) - 1] == '_')
				n[strlen(n) - 1] = 0; /* Quake's sky_rt as well as GoldSrc's skyrt */
		}
	}
	add_match(n);
}

/* files under folder whose names start with the word, ext like ".bsp" */
static void list_files(const char *folder, const char *ext, const char *suffix)
{
	char pattern[CON_INPUT + 64];
	filenames_t f = {folder, suffix};
	snprintf(pattern, sizeof(pattern), "%s%s*%s", folder, comp.word, ext);
	if (strchr(comp.word, '*') || strchr(comp.word, '?') || !FS_SafeName(pattern))
		return;
	FS_List(pattern, true, match_file, &f);
}

static int compare_match(const void *a, const void *b) { return Str_ICmp(a, b); }

/* fills comp for the word ending at the cursor */
static void gather(void)
{
	char tok[3][64] = {{0}};
	int ntok = 0, start = cursor;
	bool quoted = false, inword = false;
	if (comp_cursor == cursor && !strcmp(comp_for, input))
		return;
	Str_Copy(comp_for, input, sizeof(comp_for));
	comp_cursor = cursor;
	memset(comp.word, 0, sizeof(comp.word));
	comp.num = comp.n = 0;
	comp.hint[0] = 0;
	comp.kind = A_NONE;
	/* the command the cursor is in, and which word of it */
	for (int i = 0; i < cursor; i++) {
		char c = input[i];
		if (c == '"')
			quoted = !quoted;
		if (!quoted && c == ';') {
			ntok = 0;
			inword = false;
			continue;
		}
		if (!quoted && (c == ' ' || c == '\t')) {
			if (inword) {
				int len = BW_MIN(i - start, 63);
				if (ntok < 3) {
					memcpy(tok[ntok], input + start, (size_t)len);
					tok[ntok][len] = 0;
				}
				ntok++;
			}
			inword = false;
			continue;
		}
		if (!inword) {
			inword = true;
			start = c == '"' ? i + 1 : i;
		}
	}
	if (!inword)
		start = cursor;
	comp.start = start;
	comp.n = (size_t)(cursor - start);
	memcpy(comp.word, input + start, comp.n);
	if (ntok == 0) {
		comp.kind = A_COMMAND;
	} else {
		const char *cmd = tok[0];
		for (size_t i = 0; i < BW_ARRAY_COUNT(argkinds); i++)
			if (argkinds[i].arg == ntok && !Str_ICmp(argkinds[i].cmd, cmd))
				comp.kind = argkinds[i].kind;
		if (comp.kind == A_NONE && !Str_ICmp(cmd, "bind") && ntok > 2)
			comp.kind = A_NONE; /* the rest of a binding is free text */
		if (comp.kind == A_NONE) {
			/* say what the command or cvar is instead */
			cvar_t *v = Cvar_Find(cmd);
			const char *d = Cmd_Description(cmd);
			if (v)
				snprintf(comp.hint, sizeof(comp.hint), "%s \"%s\"  %s", v->name, v->string,
				         v->description ? v->description : "");
			else if (d)
				snprintf(comp.hint, sizeof(comp.hint), "%s  %s", cmd, d);
		}
	}
	switch (comp.kind) {
	case A_COMMAND:
		if (!comp.n && ntok == 0)
			break; /* not every command for an empty line */
		for (cvar_t *v = Cvar_First(); v; v = v->next)
			add_match(v->name);
		Cmd_ForEach(match_cmd, NULL);
		Cmd_ForEachAlias(match_cmd, NULL);
		break;
	case A_CVAR:
		for (cvar_t *v = Cvar_First(); v; v = v->next)
			add_match(v->name);
		break;
	case A_ALIAS:
		Cmd_ForEachAlias(match_cmd, NULL);
		break;
	case A_KEY:
		Key_ForEachName(match_key, NULL);
		break;
	case A_MAP:
		list_files("maps/", ".bsp", "");
		break;
	case A_CFG:
		list_files("", ".cfg", NULL);
		break;
	case A_DEMO:
		list_files("demos/", ".dem", NULL);
		list_files("demos/", ".mvd", NULL);
		list_files("", ".dem", NULL);
		break;
	case A_SAVE:
		list_files("saves/", ".sav", "");
		break;
	case A_SOUND:
		list_files("sound/", ".wav", NULL);
		list_files("sound/", ".ogg", NULL);
		break;
	case A_MODEL:
		list_files("", ".mdl", NULL);
		list_files("", ".iqm", NULL);
		list_files("", ".md3", NULL);
		break;
	case A_SKY:
		list_files("env/", "rt.*", "rt");
		list_files("gfx/env/", "rt.*", "rt");
		list_files("materials/skybox/", "rt.vtf", "rt");
		break;
	}
	qsort(comp.match, (size_t)comp.num, sizeof(comp.match[0]), compare_match);
}

/* put text in place of the input from comp.start to end */
static void replace_word(int end, const char *text, bool space)
{
	char out[CON_INPUT];
	snprintf(out, sizeof(out), "%.*s%s%s%s", comp.start, input, text, space && input[end] != ' ' ? " " : "",
	         input + end);
	Str_Copy(input, out, sizeof(input));
	cursor = (int)BW_MIN(strlen(input), (size_t)comp.start + strlen(text) + (space ? 1 : 0));
}

const char *Con_Suggestion(int i, char *info, size_t size, bool *selected);

static void complete(bool back)
{
	if (cycle >= 0 && comp.num > 1) {
		/* again: the next match in place of the last */
		cycle = (cycle + (back ? comp.num - 1 : 1)) % comp.num;
		replace_word(cycle_end, comp.match[cycle], false);
		cycle_end = cursor;
		Str_Copy(comp_for, input, sizeof(comp_for));
		comp_cursor = cursor;
		return;
	}
	gather();
	if (!comp.num)
		return;
	if (comp.num == 1) {
		replace_word(cursor, comp.match[0], true);
		return;
	}
	{
		/* as far as every match agrees, then list them */
		char common[96];
		Str_Copy(common, comp.match[0], sizeof(common));
		for (int i = 1; i < comp.num; i++) {
			size_t k = 0;
			while (common[k] && tolower((unsigned char)common[k]) == tolower((unsigned char)comp.match[i][k]))
				k++;
			common[k] = 0;
		}
		if (strlen(common) > comp.n) {
			replace_word(cursor, common, false);
			return;
		}
	}
	if (!con_suggestions->value) {
		char info[160];
		Con_Printf("]%s\n", input);
		for (int i = 0; i < comp.num; i++)
			Con_Printf("  %s %s\n", Con_Suggestion(i, info, sizeof(info), NULL), info);
	}
	/* nothing more in common: Tab now steps through them */
	cycle = back ? comp.num - 1 : 0;
	replace_word(cursor, comp.match[cycle], false);
	cycle_end = cursor;
	Str_Copy(comp_for, input, sizeof(comp_for));
	comp_cursor = cursor;
}

int Con_Suggestions(void)
{
	if (!active)
		return 0;
	if (cycle < 0)
		gather();
	return comp.num ? comp.num : comp.hint[0] ? -1 : 0;
}

const char *Con_Suggestion(int i, char *info, size_t size, bool *selected)
{
	const char *name;
	info[0] = 0;
	if (selected)
		*selected = i == cycle;
	if (i < 0 || i >= comp.num)
		return i == 0 && comp.hint[0] ? comp.hint : NULL;
	name = comp.match[i];
	switch (comp.kind) {
	case A_COMMAND:
	case A_CVAR: {
		cvar_t *v = Cvar_Find(name);
		const char *d = Cmd_Description(name);
		if (v)
			snprintf(info, size, "\"%s\"  %s", v->string, v->description ? v->description : "");
		else if (d)
			Str_Copy(info, d, size);
		else if (Cmd_AliasText(name))
			snprintf(info, size, "alias: %s", Cmd_AliasText(name));
		break;
	}
	case A_KEY: {
		const char *b = Key_GetBinding(Key_NumForName(name));
		if (b[0])
			snprintf(info, size, "\"%s\"", b);
		break;
	}
	}
	return name;
}

void Con_KeyEvent(int key, int unicode)
{
	size_t len = strlen(input);
	if (key != K_TAB && key != K_LSHIFT)
		cycle = -1;
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
		complete(Key_IsDown(K_LSHIFT));
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
	con_suggestions = Cvar_Get("con_suggestions", "1", CVAR_ARCHIVE,
	                           "List what Tab would complete under the console input");
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
