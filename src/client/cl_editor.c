/* Bulwark engine - the text editor behind "edit" and "aliasedit".
 *
 * edit <file>[:line] opens a file from the game's search path (or a new
 * one) in a full-screen editor; Ctrl+S writes it to the writable game
 * directory. aliasedit <alias> edits an alias's text, one command a line,
 * and Ctrl+S sets it. Escape closes; with unsaved changes it asks first.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../render/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABW 4

static bool active, modified, ctrl, shift, confirmclose;
static bool isalias;
static char target[128];
static char **lines;
static int numlines, cur_line, cur_col, top;
static char status[160];
static double statustime;

bool ED_Active(void) { return active; }

static void free_lines(void)
{
	for (int i = 0; i < numlines; i++)
		Mem_Free(lines[i]);
	Mem_Free(lines);
	lines = NULL;
	numlines = 0;
}

static void insert_line(int at, const char *text, size_t len)
{
	char *l = Mem_Alloc(len + 1);
	memcpy(l, text, len);
	if (!(numlines & 63))
		lines = Mem_Realloc(lines, sizeof(char *) * (numlines + 64));
	memmove(lines + at + 1, lines + at, sizeof(char *) * (numlines - at));
	lines[at] = l;
	numlines++;
}

static void remove_line(int at)
{
	Mem_Free(lines[at]);
	memmove(lines + at, lines + at + 1, sizeof(char *) * (numlines - at - 1));
	numlines--;
}

static void set_text(const char *text)
{
	const char *p = text;
	free_lines();
	for (;;) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n && p[n - 1] == '\r')
			n--;
		insert_line(numlines, p, n);
		if (!e)
			break;
		p = e + 1;
	}
}

static void set_status(const char *s)
{
	Str_Copy(status, s, sizeof(status));
	statustime = cl.realtime;
}

static void open_editor(const char *name, const char *text, bool alias, int line)
{
	set_text(text ? text : "");
	Str_Copy(target, name, sizeof(target));
	isalias = alias;
	active = true;
	modified = confirmclose = false;
	cur_line = BW_MAX(0, BW_MIN(line, numlines - 1));
	cur_col = 0;
	top = BW_MAX(0, cur_line - 5);
	set_status(text ? "" : "new file");
	Con_SetActive(false);
}

static void Edit_f(int argc, const char **argv)
{
	char name[128], *colon;
	int line = 0;
	size_t len;
	char *text;
	if (argc < 2) {
		Con_Printf("edit [filename[:line]]: edit a file\n");
		return;
	}
	Str_Copy(name, argv[1], sizeof(name));
	if ((colon = strrchr(name, ':')) && colon[1] >= '0' && colon[1] <= '9') {
		line = atoi(colon + 1) - 1;
		*colon = 0;
	}
	if (!FS_SafeName(name)) {
		Con_Printf("edit: %s: not a file the game may write\n", name);
		return;
	}
	text = FS_LoadFile(name, &len);
	open_editor(name, text, false, line);
	Mem_Free(text);
}

static void AliasEdit_f(int argc, const char **argv)
{
	const char *t = argc > 1 ? Cmd_AliasText(argv[1]) : NULL;
	strbuf_t sb = {0};
	if (!t) {
		Con_Printf("Not an alias\n");
		return;
	}
	/* one command a line */
	for (const char *p = t; *p; p++) {
		bool quoted = false;
		for (const char *q = t; q < p; q++)
			if (*q == '"')
				quoted = !quoted;
		if (*p == ';' && !quoted) {
			SB_Append(&sb, "\n", 1);
			while (p[1] == ' ')
				p++;
		} else {
			SB_Append(&sb, p, 1);
		}
	}
	SB_Append(&sb, "", 1);
	open_editor(argv[1], sb.data, true, 0);
	SB_Free(&sb);
}

static void save(void)
{
	strbuf_t sb = {0};
	for (int i = 0; i < numlines; i++) {
		SB_AppendStr(&sb, lines[i]);
		if (i + 1 < numlines)
			SB_AppendStr(&sb, isalias ? "; " : "\n");
	}
	SB_Append(&sb, "", 1);
	sb.len--;
	if (isalias) {
		Cmd_SetAlias(target, sb.data);
		set_status(va("alias %s set", target));
		modified = false;
	} else {
		char ospath[512];
		FILE *f;
		if (!isalias && sb.len && sb.data[sb.len - 1] != '\n')
			SB_Append(&sb, "\n", 1);
		if (FS_WritePath(target, ospath, sizeof(ospath)) && FS_CreatePath(ospath) && (f = fopen(ospath, "wb"))) {
			fwrite(sb.data, 1, sb.len, f);
			fclose(f);
			set_status(va("wrote %s", target));
			modified = false;
		} else {
			set_status(va("couldn't write %s", target));
		}
	}
	SB_Free(&sb);
}

static void insert_text(const char *s, size_t n)
{
	char *l = lines[cur_line];
	size_t len = strlen(l);
	char *nl = Mem_Alloc(len + n + 1);
	memcpy(nl, l, (size_t)cur_col);
	memcpy(nl + cur_col, s, n);
	memcpy(nl + cur_col + n, l + cur_col, len - cur_col);
	Mem_Free(l);
	lines[cur_line] = nl;
	cur_col += (int)n;
	modified = true;
}

/* steps over whole UTF-8 sequences */
static int prev_col(const char *l, int c)
{
	do
		c--;
	while (c > 0 && ((unsigned char)l[c] & 0xc0) == 0x80);
	return BW_MAX(c, 0);
}

static int next_col(const char *l, int c)
{
	int len = (int)strlen(l);
	if (c >= len)
		return len;
	do
		c++;
	while (c < len && ((unsigned char)l[c] & 0xc0) == 0x80);
	return c;
}

bool ED_KeyEvent(int key, bool down, int unicode)
{
	char *l;
	int len, page = 20;
	if (!active)
		return false;
	if (key == K_LCTRL)
		ctrl = down;
	if (key == K_LSHIFT)
		shift = down;
	if (!down)
		return true;
	l = lines[cur_line];
	len = (int)strlen(l);
	cur_col = BW_MIN(cur_col, len);
	if (key != K_ESCAPE && key != K_LCTRL && key != K_LSHIFT)
		confirmclose = false;
	if (ctrl && (key == 's' || key == 'S')) {
		save();
		return true;
	}
	switch (key) {
	case K_ESCAPE:
		if (modified && !confirmclose) {
			confirmclose = true;
			set_status("unsaved changes: Escape again to discard them, Ctrl+S to save");
			return true;
		}
		active = false;
		free_lines();
		return true;
	case K_UPARROW: cur_line = BW_MAX(0, cur_line - 1); break;
	case K_DOWNARROW: cur_line = BW_MIN(numlines - 1, cur_line + 1); break;
	case K_PGUP: case K_MWHEELUP: cur_line = BW_MAX(0, cur_line - (key == K_PGUP ? page : 3)); break;
	case K_PGDN: case K_MWHEELDOWN: cur_line = BW_MIN(numlines - 1, cur_line + (key == K_PGDN ? page : 3)); break;
	case K_LEFTARROW:
		if (cur_col > 0)
			cur_col = prev_col(l, cur_col);
		else if (cur_line > 0)
			cur_col = (int)strlen(lines[--cur_line]);
		return true;
	case K_RIGHTARROW:
		if (cur_col < len)
			cur_col = next_col(l, cur_col);
		else if (cur_line + 1 < numlines) {
			cur_line++;
			cur_col = 0;
		}
		return true;
	case K_HOME:
		if (ctrl)
			cur_line = 0;
		cur_col = 0;
		return true;
	case K_END:
		if (ctrl)
			cur_line = numlines - 1;
		cur_col = (int)strlen(lines[cur_line]);
		return true;
	case K_ENTER: case K_KP_ENTER: {
		/* split the line, keeping its indentation */
		int indent = 0;
		char *rest;
		while (indent < cur_col && (l[indent] == ' ' || l[indent] == '\t'))
			indent++;
		rest = Mem_Alloc((size_t)indent + (size_t)(len - cur_col) + 1);
		memcpy(rest, l, (size_t)indent);
		memcpy(rest + indent, l + cur_col, (size_t)(len - cur_col));
		l[cur_col] = 0;
		insert_line(cur_line + 1, rest, strlen(rest));
		Mem_Free(rest);
		cur_line++;
		cur_col = indent;
		modified = true;
		return true;
	}
	case K_BACKSPACE:
		if (cur_col > 0) {
			int p = prev_col(l, cur_col);
			memmove(l + p, l + cur_col, (size_t)(len - cur_col) + 1);
			cur_col = p;
			modified = true;
		} else if (cur_line > 0) {
			char *above = lines[cur_line - 1];
			int alen = (int)strlen(above);
			char *joined = Mem_Alloc((size_t)alen + (size_t)len + 1);
			memcpy(joined, above, (size_t)alen);
			memcpy(joined + alen, l, (size_t)len);
			Mem_Free(above);
			lines[cur_line - 1] = joined;
			remove_line(cur_line);
			cur_line--;
			cur_col = alen;
			modified = true;
		}
		return true;
	case K_DEL:
		if (cur_col < len) {
			int n = next_col(l, cur_col);
			memmove(l + cur_col, l + n, (size_t)(len - n) + 1);
			modified = true;
		} else if (cur_line + 1 < numlines) {
			char *below = lines[cur_line + 1];
			char *joined = Mem_Alloc((size_t)len + strlen(below) + 1);
			memcpy(joined, l, (size_t)len);
			strcpy(joined + len, below);
			Mem_Free(l);
			lines[cur_line] = joined;
			remove_line(cur_line + 1);
			modified = true;
		}
		return true;
	case K_TAB:
		insert_text("\t", 1);
		return true;
	default:
		if (ctrl || unicode < 32 || unicode == 127)
			return true;
		{
			/* UTF-8 */
			char u[4];
			size_t n;
			if (unicode < 0x80)
				u[0] = (char)unicode, n = 1;
			else if (unicode < 0x800)
				u[0] = (char)(0xc0 | (unicode >> 6)), u[1] = (char)(0x80 | (unicode & 63)), n = 2;
			else
				u[0] = (char)(0xe0 | ((unicode >> 12) & 15)), u[1] = (char)(0x80 | ((unicode >> 6) & 63)),
				u[2] = (char)(0x80 | (unicode & 63)), n = 3;
			insert_text(u, n);
		}
		return true;
	}
	cur_col = BW_MIN(cur_col, (int)strlen(lines[cur_line]));
	(void)shift;
	return true;
}

/* the line as drawn: tabs become spaces up to the next tab stop */
static void expand(const char *l, int upto, char *out, size_t size)
{
	size_t o = 0;
	int col = 0;
	for (int i = 0; l[i] && (upto < 0 || i < upto) && o + TABW + 1 < size; i++) {
		if (l[i] == '\t') {
			do
				out[o++] = ' ', col++;
			while (col % TABW);
		} else {
			out[o++] = l[i];
			if (((unsigned char)l[i] & 0xc0) != 0x80)
				col++;
		}
	}
	out[o] = 0;
}

void ED_Draw(int w, int h)
{
	static const float bg[3] = {0.06f, 0.07f, 0.09f}, bar[3] = {0.16f, 0.18f, 0.24f};
	static const float text[3] = {0.9f, 0.9f, 0.88f}, dim[3] = {0.45f, 0.5f, 0.55f}, hi[3] = {1, 0.8f, 0.3f};
	float ts, lh, gutter, y;
	int rows;
	char buf[2048];
	if (!active)
		return;
	ts = BW_MAX(10.0f, (float)(int)(h / 56.0f));
	lh = ts * 1.3f;
	rows = BW_MAX(1, (int)((h - lh * 2.5f) / lh));
	if (cur_line < top)
		top = cur_line;
	if (cur_line >= top + rows)
		top = cur_line - rows + 1;
	gutter = R_TextWidth("00000 ", ts);
	R_Rect(0, 0, (float)w, (float)h, bg, 0.97f);
	R_Rect(0, 0, (float)w, lh * 1.2f, bar, 1);
	snprintf(buf, sizeof(buf), "%s %s%s", isalias ? "alias" : "edit", target, modified ? " *" : "");
	R_Text(8, lh * 0.15f, buf, ts, hi, 1);
	snprintf(buf, sizeof(buf), "line %d/%d  col %d", cur_line + 1, numlines, cur_col + 1);
	R_Text((float)w - R_TextWidth(buf, ts) - 8, lh * 0.15f, buf, ts, dim, 1);
	y = lh * 1.5f;
	for (int i = top; i < numlines && i < top + rows; i++, y += lh) {
		snprintf(buf, sizeof(buf), "%5d", i + 1);
		R_Text(4, y, buf, ts, i == cur_line ? hi : dim, 1);
		expand(lines[i], -1, buf, sizeof(buf));
		R_Text(gutter, y, buf, ts, text, 1);
		if (i == cur_line && ((int)(cl.realtime * 2) & 1) == 0) {
			expand(lines[i], cur_col, buf, sizeof(buf));
			R_Rect(gutter + R_TextWidth(buf, ts), y, BW_MAX(1.0f, ts / 8), ts, hi, 1);
		}
	}
	R_Rect(0, (float)h - lh, (float)w, lh, bar, 1);
	R_Text(8, (float)h - lh * 0.85f,
	       status[0] && cl.realtime - statustime < 4 ? status : "Ctrl+S save   Esc close   arrows, PgUp/PgDn, Home/End move",
	       ts, status[0] && cl.realtime - statustime < 4 ? hi : dim, 1);
}

void ED_Init(void)
{
	Cmd_AddD("edit", Edit_f, "edit [filename[:line]]: edit a file");
	Cmd_AddD("aliasedit", AliasEdit_f, "aliasedit <alias>: edit an alias's commands, one a line");
}
