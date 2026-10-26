/* Bulwark engine - key bindings, movement buttons, mouse look and usercmd
 * generation. ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static char *bindings[K_MAX];
static bool keydown[K_MAX];

static cvar_t *cl_forwardspeed, *cl_backspeed, *cl_sidespeed, *cl_upspeed, *cl_yawspeed, *cl_pitchspeed;
static cvar_t *cl_movespeedkey, *sensitivity, *m_pitch, *m_yaw, *m_filter, *cl_run;

/* --- key names ------------------------------------------------------------ */

typedef struct {
	const char *name;
	int key;
} keyname_t;

static const keyname_t keynames[] = {
	{"TAB", K_TAB}, {"ENTER", K_ENTER}, {"ESCAPE", K_ESCAPE}, {"SPACE", K_SPACE}, {"BACKSPACE", K_BACKSPACE},
	{"UPARROW", K_UPARROW}, {"DOWNARROW", K_DOWNARROW}, {"LEFTARROW", K_LEFTARROW}, {"RIGHTARROW", K_RIGHTARROW},
	{"ALT", K_LALT}, {"LALT", K_LALT}, {"RALT", K_LALT}, {"CTRL", K_LCTRL}, {"LCTRL", K_LCTRL}, {"RCTRL", K_LCTRL},
	{"SHIFT", K_LSHIFT}, {"LSHIFT", K_LSHIFT}, {"RSHIFT", K_LSHIFT},
	{"F1", K_F1}, {"F2", K_F2}, {"F3", K_F3}, {"F4", K_F4}, {"F5", K_F5}, {"F6", K_F6}, {"F7", K_F7},
	{"F8", K_F8}, {"F9", K_F9}, {"F10", K_F10}, {"F11", K_F11}, {"F12", K_F12},
	{"INS", K_INS}, {"DEL", K_DEL}, {"PGDN", K_PGDN}, {"PGUP", K_PGUP}, {"HOME", K_HOME}, {"END", K_END},
	{"PAUSE", K_PAUSE}, {"CAPSLOCK", K_CAPSLOCK}, {"SCROLLLOCK", K_SCROLLLOCK}, {"NUMLOCK", K_KP_NUMLOCK},
	{"KP_INS", K_KP_INS}, {"KP_END", K_KP_END}, {"KP_DOWNARROW", K_KP_DOWNARROW}, {"KP_PGDN", K_KP_PGDN},
	{"KP_LEFTARROW", K_KP_LEFTARROW}, {"KP_5", K_KP_5}, {"KP_RIGHTARROW", K_KP_RIGHTARROW}, {"KP_HOME", K_KP_HOME},
	{"KP_UPARROW", K_KP_UPARROW}, {"KP_PGUP", K_KP_PGUP}, {"KP_DEL", K_KP_DEL}, {"KP_SLASH", K_KP_SLASH},
	{"KP_STAR", K_KP_STAR}, {"KP_MINUS", K_KP_MINUS}, {"KP_PLUS", K_KP_PLUS}, {"KP_ENTER", K_KP_ENTER},
	{"KP_EQUALS", K_KP_EQUALS}, {"PRINTSCREEN", K_PRINTSCREEN},
	{"MOUSE1", K_MOUSE1}, {"MOUSE2", K_MOUSE2}, {"MOUSE3", K_MOUSE3}, {"MOUSE4", K_MOUSE4}, {"MOUSE5", K_MOUSE5},
	{"MWHEELUP", K_MWHEELUP}, {"MWHEELDOWN", K_MWHEELDOWN},
	{"SEMICOLON", ';'}, {"TILDE", '~'}, {"BACKQUOTE", '`'}, {"BACKSLASH", '\\'}, {"QUOTES", '"'}, {"PLUS", '+'},
	{"MINUS", '-'}, {"APOSTROPHE", '\''},
	{"GP_A", K_GP_A}, {"GP_B", K_GP_B}, {"GP_X", K_GP_X}, {"GP_Y", K_GP_Y}, {"GP_LSHOULDER", K_GP_LSHOULDER},
	{"GP_RSHOULDER", K_GP_RSHOULDER}, {"GP_LTRIGGER", K_GP_LTRIGGER}, {"GP_RTRIGGER", K_GP_RTRIGGER},
	{"GP_START", K_GP_MENU}, {"GP_BACK", K_GP_VIEW},
	{NULL, 0}};

int Key_NumForName(const char *name)
{
	if (!name || !*name)
		return -1;
	if (!name[1]) {
		int c = (unsigned char)name[0];
		return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
	}
	for (const keyname_t *k = keynames; k->name; k++)
		if (!Str_ICmp(k->name, name))
			return k->key;
	if (!Str_NICmp(name, "K_", 2))
		return Key_NumForName(name + 2);
	if (name[0] == '#')
		return atoi(name + 1);
	return -1;
}

const char *Key_NameForNum(int key)
{
	static char buf[8];
	if (key > 32 && key < 127 && key != ';' && key != '"') {
		buf[0] = (char)(key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key);
		buf[1] = 0;
		return buf;
	}
	for (const keyname_t *k = keynames; k->name; k++)
		if (k->key == key)
			return k->name;
	snprintf(buf, sizeof(buf), "#%d", key);
	return buf;
}

const char *Key_GetBinding(int key) { return key >= 0 && key < K_MAX && bindings[key] ? bindings[key] : ""; }

void Key_SetBinding(int key, const char *b)
{
	if (key < 0 || key >= K_MAX)
		return;
	Mem_Free(bindings[key]);
	bindings[key] = b && *b ? Mem_Strdup(b) : NULL;
}

bool Key_IsDown(int key) { return key >= 0 && key < K_MAX && keydown[key]; }

void Key_WriteBindings(strbuf_t *sb)
{
	for (int k = 0; k < K_MAX; k++)
		if (bindings[k])
			SB_Printf(sb, "bind %s \"%s\"\n", Key_NameForNum(k), bindings[k]);
}

static void Bind_f(int argc, const char **argv)
{
	int k;
	if (argc < 2) {
		Con_Printf("usage: bind <key> [command]\n");
		return;
	}
	k = Key_NumForName(argv[1]);
	if (k < 0) {
		Con_Printf("\"%s\" isn't a valid key\n", argv[1]);
		return;
	}
	if (argc == 2) {
		Con_Printf("\"%s\" = \"%s\"\n", argv[1], Key_GetBinding(k));
		return;
	}
	{
		char cmd[1024] = "";
		for (int i = 2; i < argc; i++) {
			if (i > 2)
				Str_Cat(cmd, " ", sizeof(cmd));
			Str_Cat(cmd, argv[i], sizeof(cmd));
		}
		Key_SetBinding(k, cmd);
	}
}

static void Unbind_f(int argc, const char **argv)
{
	if (argc >= 2)
		Key_SetBinding(Key_NumForName(argv[1]), NULL);
}

static void Unbindall_f(int argc, const char **argv)
{
	for (int k = 0; k < K_MAX; k++)
		Key_SetBinding(k, NULL);
}

static void Bindlist_f(int argc, const char **argv)
{
	for (int k = 0; k < K_MAX; k++)
		if (bindings[k])
			Con_Printf("%-12s \"%s\"\n", Key_NameForNum(k), bindings[k]);
}

/* --- buttons ----------------------------------------------------------------- */

typedef struct {
	int down[2]; /* keys holding it */
	bool active;
	bool impulse_down; /* pressed since the last command */
} kbutton_t;

enum {
	B_FORWARD, B_BACK, B_MOVELEFT, B_MOVERIGHT, B_MOVEUP, B_MOVEDOWN, B_LEFT, B_RIGHT, B_LOOKUP, B_LOOKDOWN, B_SPEED,
	B_JUMP, B_ATTACK, B_ATTACK2, B_USE, B_RELOAD, B_DUCK, B_SPRINT, B_PRONE, B_BUTTON3, B_BUTTON4, B_BUTTON5,
	B_BUTTON6, B_BUTTON7, B_BUTTON8, B_COUNT
};
static kbutton_t buttons[B_COUNT];
static int pending_impulse;

static void key_down(kbutton_t *b, int argc, const char **argv)
{
	int k = argc > 1 ? atoi(argv[1]) : -1;
	if (k == b->down[0] || k == b->down[1])
		return;
	if (!b->down[0])
		b->down[0] = k;
	else if (!b->down[1])
		b->down[1] = k;
	else
		return;
	if (!b->active)
		b->impulse_down = true;
	b->active = true;
}

static void key_up(kbutton_t *b, int argc, const char **argv)
{
	int k;
	if (argc < 2) { /* typed manually or from a script: release all */
		b->down[0] = b->down[1] = 0;
		b->active = false;
		return;
	}
	k = atoi(argv[1]);
	if (b->down[0] == k)
		b->down[0] = 0;
	else if (b->down[1] == k)
		b->down[1] = 0;
	else
		return;
	if (!b->down[0] && !b->down[1])
		b->active = false;
}

#define BUTTON_CMDS(name, idx)                                                                                         \
	static void name##_down(int argc, const char **argv) { key_down(&buttons[idx], argc, argv); }                       \
	static void name##_up(int argc, const char **argv) { key_up(&buttons[idx], argc, argv); }

BUTTON_CMDS(forward, B_FORWARD)
BUTTON_CMDS(back, B_BACK)
BUTTON_CMDS(moveleft, B_MOVELEFT)
BUTTON_CMDS(moveright, B_MOVERIGHT)
BUTTON_CMDS(moveup, B_MOVEUP)
BUTTON_CMDS(movedown, B_MOVEDOWN)
BUTTON_CMDS(left, B_LEFT)
BUTTON_CMDS(right, B_RIGHT)
BUTTON_CMDS(lookup, B_LOOKUP)
BUTTON_CMDS(lookdown, B_LOOKDOWN)
BUTTON_CMDS(speed, B_SPEED)
BUTTON_CMDS(jump, B_JUMP)
BUTTON_CMDS(attack, B_ATTACK)
BUTTON_CMDS(attack2, B_ATTACK2)
BUTTON_CMDS(use, B_USE)
BUTTON_CMDS(reload, B_RELOAD)
BUTTON_CMDS(duck, B_DUCK)
BUTTON_CMDS(sprint, B_SPRINT)
BUTTON_CMDS(prone, B_PRONE)
BUTTON_CMDS(button3, B_BUTTON3)
BUTTON_CMDS(button4, B_BUTTON4)
BUTTON_CMDS(button5, B_BUTTON5)
BUTTON_CMDS(button6, B_BUTTON6)
BUTTON_CMDS(button7, B_BUTTON7)
BUTTON_CMDS(button8, B_BUTTON8)

static void Impulse_f(int argc, const char **argv)
{
	if (argc > 1)
		pending_impulse = atoi(argv[1]);
}

static float button_amount(kbutton_t *b)
{
	float v = b->active || b->impulse_down ? 1.0f : 0.0f;
	return v;
}

/* --- key events ------------------------------------------------------------- */

/* the +command a key started, so its release sends the matching -command
 * even if the binding changed while it was held */
static char *held_command[K_MAX];

void IN_KeyEvent(int key, bool down, int unicode)
{
	const char *b;
	if (key < 0 || key >= K_MAX)
		return;
	if (down && keydown[key] && key != K_BACKSPACE && !Con_IsActive() && bindings[key] && bindings[key][0] == '+')
		return; /* key repeat on a held button */
	keydown[key] = down;

	if (key == '`' || key == '~') {
		if (down)
			Con_ToggleConsole();
		return;
	}
	if (Con_IsActive()) {
		if (down && key == K_ESCAPE)
			Con_SetActive(false);
		else if (down)
			Con_KeyEvent(key, unicode);
		if (down || !held_command[key])
			return;
	} else if ((down || !held_command[key]) && UI_KeyEvent(key, down, unicode)) {
		return; /* a menu or other input view is open */
	} else if (down || !held_command[key]) {
		/* the game gets first look at keys while playing */
		if (CSQC_InputEvent(down ? 0 : 1, (float)key, (float)unicode, 0))
			return;
	}

	if (!down) {
		if (held_command[key]) {
			char cmd[1024];
			snprintf(cmd, sizeof(cmd), "-%s %d\n", held_command[key] + 1, key);
			Cbuf_AddText(cmd);
			Mem_Free(held_command[key]);
			held_command[key] = NULL;
		}
		return;
	}
	b = Key_GetBinding(key);
	if (!*b) {
		if (key == K_ESCAPE)
			Cbuf_AddText("togglemenu\n");
		return;
	}
	if (b[0] == '+') {
		char cmd[1024];
		Mem_Free(held_command[key]);
		held_command[key] = Mem_Strdup(b);
		snprintf(cmd, sizeof(cmd), "%s %d\n", b, key);
		Cbuf_AddText(cmd);
	} else {
		Cbuf_AddText(b);
		Cbuf_AddText("\n");
	}
}

void IN_MouseMove(float dx, float dy)
{
	static float olddx, olddy;
	float mx = dx, my = dy;
	if (Con_IsActive())
		return;
	if (CSQC_InputEvent(2, dx, dy, 0))
		return;
	if (m_filter->ivalue) {
		mx = (dx + olddx) * 0.5f;
		my = (dy + olddy) * 0.5f;
	}
	olddx = dx;
	olddy = dy;
	cl.viewangles[1] -= m_yaw->value * mx * sensitivity->value;
	cl.viewangles[0] += m_pitch->value * my * sensitivity->value;
	if (cl.viewangles[0] > 89)
		cl.viewangles[0] = 89;
	if (cl.viewangles[0] < -89)
		cl.viewangles[0] = -89;
	cl.viewangles[1] = fmodf(cl.viewangles[1], 360.0f);
}

/* --- commands ------------------------------------------------------------- */

static void fill_command(usercmd_t *u, double now)
{
	float speed = (cl_run->ivalue ^ buttons[B_SPEED].active) ? 1.0f : cl_movespeedkey->value;
	float dt = (float)cl.frametime;
	unsigned bits = 0;
	/* keyboard turning */
	cl.viewangles[1] += dt * cl_yawspeed->value * (button_amount(&buttons[B_LEFT]) - button_amount(&buttons[B_RIGHT]));
	cl.viewangles[0] += dt * cl_pitchspeed->value * (button_amount(&buttons[B_LOOKDOWN]) - button_amount(&buttons[B_LOOKUP]));
	if (cl.viewangles[0] > 89)
		cl.viewangles[0] = 89;
	if (cl.viewangles[0] < -89)
		cl.viewangles[0] = -89;

	VectorCopy(cl.viewangles, u->angles);
	u->movevalues[0] = speed * (cl_forwardspeed->value * button_amount(&buttons[B_FORWARD]) -
	                            cl_backspeed->value * button_amount(&buttons[B_BACK]));
	u->movevalues[1] = speed * cl_sidespeed->value *
	                   (button_amount(&buttons[B_MOVERIGHT]) - button_amount(&buttons[B_MOVELEFT]));
	u->movevalues[2] = speed * cl_upspeed->value * (button_amount(&buttons[B_MOVEUP]) - button_amount(&buttons[B_MOVEDOWN]));

	/* the bit layout games expect: attack=1, jump=2, then button3.. */
	if (buttons[B_ATTACK].active || buttons[B_ATTACK].impulse_down)
		bits |= 1;
	if (buttons[B_JUMP].active || buttons[B_JUMP].impulse_down)
		bits |= 2;
	if (buttons[B_PRONE].active || buttons[B_BUTTON3].active)
		bits |= 4;
	if (buttons[B_RELOAD].active || buttons[B_BUTTON4].active)
		bits |= 8;
	if (buttons[B_USE].active || buttons[B_BUTTON5].active)
		bits |= 16;
	if (buttons[B_ATTACK2].active || buttons[B_BUTTON6].active)
		bits |= 32;
	if (buttons[B_SPRINT].active || buttons[B_BUTTON7].active)
		bits |= 64;
	if (buttons[B_DUCK].active || buttons[B_BUTTON8].active)
		bits |= 128;
	u->buttons = bits;
	u->impulse = (uint8_t)pending_impulse;
	u->sequence = cl.clientcommandframe;
	u->msec = (float)BW_MIN(now - cl.cmd_start, 0.25);
	u->clienttime = cl.realtime;
	u->servertime_ms = (uint32_t)(cl.time * 1000.0);
}

void CL_UpdateCurrentCommand(double now)
{
	fill_command(&cl.cmds[cl.clientcommandframe % CL_CMD_BACKUP], now);
}

void CL_BuildCommand(double now)
{
	usercmd_t *u = &cl.cmds[cl.clientcommandframe % CL_CMD_BACKUP];
	fill_command(u, now);
	/* the game may rewrite the inputs (menus, weapon selection...) */
	if (cl.csqc) {
		CSQC_SetInputGlobals(cl.csqc, u);
		CSQC_InputFrame();
		CSQC_ReadInputGlobals(cl.csqc, u);
	}
	/* the server receives whole milliseconds, so predict with the same */
	u->msec = roundf(u->msec * 1000) / 1000;
	cl.clientcommandframe++;
	cl.cmd_start = now;
	pending_impulse = 0;
	for (int i = 0; i < B_COUNT; i++)
		buttons[i].impulse_down = false;
	memset(&cl.cmds[cl.clientcommandframe % CL_CMD_BACKUP], 0, sizeof(usercmd_t));
}

bool CL_GetInputState(uint32_t seq, usercmd_t *out)
{
	if (seq > cl.clientcommandframe || cl.clientcommandframe - seq >= CL_CMD_BACKUP)
		return false;
	*out = cl.cmds[seq % CL_CMD_BACKUP];
	return out->sequence == seq;
}

void IN_Init(void)
{
	cl_forwardspeed = Cvar_Get("cl_forwardspeed", "450", CVAR_ARCHIVE, "Forward move value at full speed");
	cl_backspeed = Cvar_Get("cl_backspeed", "450", CVAR_ARCHIVE, "Backward move value");
	cl_sidespeed = Cvar_Get("cl_sidespeed", "450", CVAR_ARCHIVE, "Strafe move value");
	cl_upspeed = Cvar_Get("cl_upspeed", "320", CVAR_ARCHIVE, "Swim/fly vertical move value");
	cl_yawspeed = Cvar_Get("cl_yawspeed", "210", CVAR_ARCHIVE, "Keyboard turn speed, degrees per second");
	cl_pitchspeed = Cvar_Get("cl_pitchspeed", "225", CVAR_ARCHIVE, "Keyboard look speed, degrees per second");
	cl_movespeedkey = Cvar_Get("cl_movespeedkey", "0.3", CVAR_ARCHIVE, "Speed scale while +speed (walk) is held");
	cl_run = Cvar_Get("cl_run", "1", CVAR_ARCHIVE, "Run by default; +speed walks");
	sensitivity = Cvar_Get("sensitivity", "3", CVAR_ARCHIVE, "Mouse sensitivity");
	m_pitch = Cvar_Get("m_pitch", "0.022", CVAR_ARCHIVE, "Degrees per mouse count vertically (negative inverts)");
	m_yaw = Cvar_Get("m_yaw", "0.022", CVAR_ARCHIVE, "Degrees per mouse count horizontally");
	m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE, "Average mouse movement over two frames");

	Cmd_Add("bind", Bind_f);
	Cmd_Add("unbind", Unbind_f);
	Cmd_Add("unbindall", Unbindall_f);
	Cmd_Add("bindlist", Bindlist_f);
	Cmd_Add("impulse", Impulse_f);
#define ADD(n)                                                                                                         \
	Cmd_Add("+" #n, n##_down);                                                                                         \
	Cmd_Add("-" #n, n##_up)
	ADD(forward);
	ADD(back);
	ADD(moveleft);
	ADD(moveright);
	ADD(moveup);
	ADD(movedown);
	ADD(left);
	ADD(right);
	ADD(lookup);
	ADD(lookdown);
	ADD(speed);
	ADD(jump);
	ADD(attack);
	ADD(attack2);
	ADD(use);
	ADD(reload);
	ADD(duck);
	ADD(sprint);
	ADD(prone);
	ADD(button3);
	ADD(button4);
	ADD(button5);
	ADD(button6);
	ADD(button7);
	ADD(button8);
#undef ADD
}

/* the pointer moved while not grabbed (menus, cursor mode) */
float in_mouse_x, in_mouse_y;
void IN_MouseAbs(float x, float y)
{
	in_mouse_x = x;
	in_mouse_y = y;
	UI_MouseMove(x, y);
	if (!Con_IsActive())
		CSQC_InputEvent(3, x, y, 0);
}

/* focus lost: let go of everything so no button stays held */
void IN_ReleaseAll(void)
{
	for (int k = 0; k < K_MAX; k++)
		if (keydown[k])
			IN_KeyEvent(k, false, 0);
}
