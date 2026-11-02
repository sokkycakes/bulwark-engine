/* Bulwark engine - key bindings, movement buttons, mouse look and usercmd
 * generation. ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../ui/ui.h"
#include "../render/render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static char *bindings[K_MAX];
static uint8_t bindlevel[K_MAX]; /* the level a binding runs at, less than RESTRICT_MAX after bindlevel */
static bool keydown[K_MAX];

static cvar_t *cl_forwardspeed, *cl_backspeed, *cl_sidespeed, *cl_upspeed, *cl_yawspeed, *cl_pitchspeed;
static cvar_t *cl_movespeedkey, *sensitivity, *m_pitch, *m_yaw, *m_filter, *cl_run;
static cvar_t *in_lastdevice;
static cvar_t *joy_enable, *joy_deadzone, *joy_curve, *joy_yawspeed, *joy_pitchspeed, *joy_invert, *joy_movescale;

/* the newest value of each controller axis */
static float joyaxis[6];

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
	{"GP_START", K_GP_MENU}, {"GP_BACK", K_GP_VIEW}, {"GP_MENU", K_GP_MENU}, {"GP_VIEW", K_GP_VIEW},
	{"GP_LTHUMB", K_GP_LTHUMB}, {"GP_RTHUMB", K_GP_RTHUMB}, {"GP_DPAD_UP", K_GP_DPAD_UP},
	{"GP_DPAD_DOWN", K_GP_DPAD_DOWN}, {"GP_DPAD_LEFT", K_GP_DPAD_LEFT}, {"GP_DPAD_RIGHT", K_GP_DPAD_RIGHT},
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

void Key_ForEachName(void (*fn)(const char *name, void *ctx), void *ctx)
{
	for (const keyname_t *k = keynames; k->name; k++)
		fn(k->name, ctx);
}

const char *Key_GetBinding(int key) { return key >= 0 && key < K_MAX && bindings[key] ? bindings[key] : ""; }

void Key_SetBinding(int key, const char *b)
{
	if (key < 0 || key >= K_MAX)
		return;
	Mem_Free(bindings[key]);
	bindings[key] = b && *b ? Mem_Strdup(b) : NULL;
	/* a bind a server or game code made runs at their level when pressed */
	bindlevel[key] = (uint8_t)BW_CLAMP(Cmd_ExecLevel(), 0, RESTRICT_MAX);
}

bool Key_IsDown(int key) { return key >= 0 && key < K_MAX && keydown[key]; }

void Key_WriteBindings(strbuf_t *sb)
{
	for (int k = 0; k < K_MAX; k++)
		if (bindings[k] && bindlevel[k] < RESTRICT_MAX)
			SB_Printf(sb, "bindlevel %s %d \"%s\"\n", Key_NameForNum(k), bindlevel[k], bindings[k]);
		else if (bindings[k])
			SB_Printf(sb, "bind %s \"%s\"\n", Key_NameForNum(k), bindings[k]);
}

/* bindlevel <key> <level> [command]: a binding that runs at a lower
 * execution level, so it can't reach restricted commands */
static void Bindlevel_f(int argc, const char **argv)
{
	int k;
	if (argc < 3) {
		Con_Printf("%s <key> <level> [command] : attach a command to a key\n", argv[0]);
		return;
	}
	k = Key_NumForName(argv[1]);
	if (k < 0 || k >= K_MAX) {
		Con_Printf("\"%s\" isn't a valid key\n", argv[1]);
		return;
	}
	if (argc > 3) {
		char cmd[1024] = "";
		for (int i = 3; i < argc; i++) {
			if (i > 3)
				Str_Cat(cmd, " ", sizeof(cmd));
			Str_Cat(cmd, argv[i], sizeof(cmd));
		}
		Key_SetBinding(k, cmd);
	}
	if (bindings[k])
		bindlevel[k] = (uint8_t)BW_CLAMP(atoi(argv[2]), 0, BW_MIN(RESTRICT_MAX, Cmd_ExecLevel()));
	Con_Printf("\"%s\" (level %d) = \"%s\"\n", argv[1], bindings[k] ? bindlevel[k] : RESTRICT_MAX, Key_GetBinding(k));
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
	B_BUTTON6, B_BUTTON7, B_BUTTON8, B_STRAFE, B_KLOOK, B_ROLLLEFT, B_ROLLRIGHT, B_COUNT
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
BUTTON_CMDS(strafe, B_STRAFE)
BUTTON_CMDS(klook, B_KLOOK)
BUTTON_CMDS(rollleft, B_ROLLLEFT)
BUTTON_CMDS(rollright, B_ROLLRIGHT)
static cvar_t *cl_6dof, *cl_rollspeed;

/* the mouse always looks around; +mlook and -mlook are kept so old configs
 * that bind them don't print errors */
static void mlook_f(int argc, const char **argv) {}

/* +button0 .. +button19 by number: 0 and 2 are attack and jump, 3-8 the
 * buttons above, 1 is bit 8 (256) and 9-19 the bits after it */
static kbutton_t numbered[20];
static void numbered_button(int argc, const char **argv)
{
	int n = atoi(argv[0] + 7);
	if (n < 0 || n >= 20)
		return;
	if (argv[0][0] == '+')
		key_down(&numbered[n], argc, argv);
	else
		key_up(&numbered[n], argc, argv);
}

static uint32_t numbered_bits(void)
{
	uint32_t bits = 0;
	for (int n = 0; n < 20; n++) {
		kbutton_t *b = &numbered[n];
		if (!b->active && !b->impulse_down)
			continue;
		if (n == 0)
			bits |= 1;
		else if (n == 1)
			bits |= 256;
		else if (n == 2)
			bits |= 2;
		else if (n <= 8)
			bits |= 1u << (n - 1);
		else
			bits |= 512u << (n - 9);
	}
	return bits;
}

static void Impulse_f(int argc, const char **argv)
{
	if (argc > 1)
		pending_impulse = atoi(argv[1]);
}

/* the pitch limit, which 6dof lifts (and lets the view roll instead) */
static void clamp_pitch(void)
{
	if (cl_6dof && cl_6dof->ivalue)
		return;
	if (cl.viewangles[0] > 89)
		cl.viewangles[0] = 89;
	if (cl.viewangles[0] < -89)
		cl.viewangles[0] = -89;
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

/* in_lastdevice: what the player last used, 0 keyboard and mouse, 1 a
 * controller, so a HUD can show the matching button prompts */
static void last_device(int pad)
{
	if (in_lastdevice && in_lastdevice->ivalue != pad)
		Cvar_SetDirect(in_lastdevice, pad ? "1" : "0");
}

void IN_KeyEvent(int key, bool down, int unicode)
{
	const char *b;
	if (key < 0 || key >= K_MAX)
		return;
	if (down)
		last_device(key >= K_JOY1 && key <= K_GP_RTRIGGER);
	if (down && keydown[key] && key != K_BACKSPACE && !Con_IsActive() && bindings[key] && bindings[key][0] == '+')
		return; /* key repeat on a held button */
	keydown[key] = down;

	if (Con_ChatMode() && down) {
		Con_ChatKey(key, unicode);
		return;
	}
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
	} else if (ED_Active() && (down || !held_command[key])) {
		/* the text editor takes every key while open */
		ED_KeyEvent(key, down, unicode);
		return;
	} else if (V_Active() && (down || !held_command[key])) {
		/* the model and map viewers take every key while open */
		V_KeyEvent(key, down);
		return;
	} else if (EM_Active() && (down || !held_command[key])) {
		/* the engine's menus take every key while open */
		EM_KeyEvent(key, down, unicode);
		return;
	} else if ((down || !held_command[key]) && WC_KeyEvent(key, down, unicode)) {
		return; /* the game's HTML menu page */
	} else if ((down || !held_command[key]) && UI_KeyEvent(key, down, unicode)) {
		return; /* a menu or other input view is open */
	} else if (down || !held_command[key]) {
		/* a film takes every key while it plays */
		if (CIN_KeyEvent(key, down))
			return;
		/* a showpic picture with a command takes clicks on it */
		if (key == K_MOUSE1 && down && R_ShowpicTouch(in_mouse_x / R_Get2DScale(), in_mouse_y / R_Get2DScale()))
			return;
		/* an open menu, then the game, get first look at keys */
		if (MQC_InputEvent(down ? 0 : 1, (float)key, (float)unicode, 0))
			return;
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
	/* GoldSrc configs bind Escape to "cancelselect", and GoldSrc opens its
	   menu on Escape regardless, so do both */
	if (key == K_ESCAPE && !Str_ICmp(b, "cancelselect")) {
		Cbuf_AddTextAtLevel("cancelselect\n", bindlevel[key]);
		Cbuf_AddText("togglemenu\n");
		return;
	}
	if (b[0] == '+') {
		char cmd[1024];
		Mem_Free(held_command[key]);
		held_command[key] = Mem_Strdup(b);
		snprintf(cmd, sizeof(cmd), "%s %d\n", b, key);
		Cbuf_AddTextAtLevel(cmd, bindlevel[key]);
	} else {
		Cbuf_AddTextAtLevel(va("%s\n", b), bindlevel[key]);
	}
}

void IN_MouseMove(float dx, float dy)
{
	static float olddx, olddy;
	float mx = dx, my = dy;
	if (dx || dy)
		last_device(0);
	if (Con_IsActive() || EM_Active() || ED_Active())
		return;
	if (V_MouseMove(dx, dy))
		return;
	if (MQC_InputEvent(2, dx, dy, 0))
		return;
	if (CSQC_InputEvent(2, dx, dy, 0))
		return;
	if (HUD_MouseMove(dx, dy))
		return;
	if (m_filter->ivalue) {
		mx = (dx + olddx) * 0.5f;
		my = (dy + olddy) * 0.5f;
	}
	olddx = dx;
	olddy = dy;
	cl.viewangles[1] -= m_yaw->value * mx * sensitivity->value;
	cl.viewangles[0] += m_pitch->value * my * sensitivity->value;
	clamp_pitch();
	cl.viewangles[1] = fmodf(cl.viewangles[1], 360.0f);
}

/* --- controllers ------------------------------------------------------------ */

void IN_JoyAxis(int dev, int axis, float value)
{
	if (axis < 0 || axis >= 6)
		return;
	/* IE_JOYAXIS is only a report: Nuclide returns true for every event
	 * type it doesn't handle, so its answer can't mean "taken" */
	if (!Con_IsActive())
		CSQC_InputEvent(6, (float)axis, value, dev);
	joyaxis[axis] = value;
}

/* layout like a console shooter's, applied only to an unbound pad so a
 * config's own choices win */
static const struct {
	int key;
	const char *cmd;
} pad_defaults[] = {
	{K_GP_A, "+jump"}, {K_GP_B, "+duck"}, {K_GP_X, "+reload"}, {K_GP_Y, "+use"},
	{K_GP_RTRIGGER, "+attack"}, {K_GP_LTRIGGER, "+attack2"}, {K_GP_RSHOULDER, "weapnext"},
	{K_GP_LSHOULDER, "weapprev"}, {K_GP_LTHUMB, "+sprint"}, {K_GP_VIEW, "+showscores"},
	{K_GP_MENU, "togglemenu"}, {K_GP_DPAD_UP, "slot1"}, {K_GP_DPAD_RIGHT, "slot2"},
	{K_GP_DPAD_DOWN, "slot3"}, {K_GP_DPAD_LEFT, "slot4"},
};

void IN_ControllerAdded(int dev)
{
	for (int i = 0; i < (int)BW_ARRAY_COUNT(pad_defaults); i++)
		if (bindings[pad_defaults[i].key])
			return;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(pad_defaults); i++)
		Key_SetBinding(pad_defaults[i].key, pad_defaults[i].cmd);
}

void IN_ControllerRemoved(int dev)
{
	memset(joyaxis, 0, sizeof(joyaxis));
	IN_ReleaseAll();
}

/* a stick's deflection past the dead zone, rescaled to 0..1 and shaped */
static float stick(float v)
{
	float dz = BW_MAX(0, BW_MIN(joy_deadzone->value, 0.95f));
	float a = fabsf(v);
	if (a <= dz)
		return 0;
	a = BW_MIN(1, (a - dz) / (1 - dz));
	a = powf(a, BW_MAX(0.1f, joy_curve->value));
	return v < 0 ? -a : a;
}

/* --- commands ------------------------------------------------------------- */

/* rotate <degrees> [seconds]: turn the view, at once or over a time */
static float rotate_left, rotate_rate;

static void Rotate_f(int argc, const char **argv)
{
	float secs;
	if (argc < 2) {
		Con_Printf("rotate <degrees> [seconds]: turn the view left (negative turns right)\n");
		return;
	}
	secs = argc > 2 ? (float)atof(argv[2]) : 0;
	if (secs <= 0) {
		cl.viewangles[1] += (float)atof(argv[1]);
		rotate_left = 0;
		return;
	}
	rotate_left = (float)atof(argv[1]);
	rotate_rate = fabsf(rotate_left) / secs;
}

static void fill_command(usercmd_t *u, double now)
{
	float speed = (cl_run->ivalue ^ buttons[B_SPEED].active) ? 1.0f : cl_movespeedkey->value;
	float dt = (float)cl.frametime;
	float joy_move[2];
	unsigned bits = 0;
	/* keyboard turning; +strafe makes the turn keys strafe, +klook makes
	 * forward and back look up and down */
	bool strafe = buttons[B_STRAFE].active, klook = buttons[B_KLOOK].active;
	float turn = button_amount(&buttons[B_LEFT]) - button_amount(&buttons[B_RIGHT]);
	float walk = button_amount(&buttons[B_FORWARD]), walkback = button_amount(&buttons[B_BACK]);
	if (!strafe)
		cl.viewangles[1] += dt * cl_yawspeed->value * turn;
	if (rotate_left != 0) {
		float step = BW_MIN(fabsf(rotate_left), rotate_rate * dt);
		step = rotate_left < 0 ? -step : step;
		cl.viewangles[1] += step;
		rotate_left -= step;
	}
	cl.viewangles[0] += dt * cl_pitchspeed->value * (button_amount(&buttons[B_LOOKDOWN]) - button_amount(&buttons[B_LOOKUP]));
	if (cl_6dof->ivalue)
		cl.viewangles[2] += dt * cl_rollspeed->value * (button_amount(&buttons[B_ROLLRIGHT]) - button_amount(&buttons[B_ROLLLEFT]));
	if (klook) {
		cl.viewangles[0] += dt * cl_pitchspeed->value * (walkback - walk);
		walk = walkback = 0;
	}
	clamp_pitch();

	/* controller sticks: right one turns, left one moves */
	if (joy_enable->ivalue && !Con_IsActive()) {
		float lx = stick(joyaxis[0]), ly = stick(joyaxis[1]), rx = stick(joyaxis[2]), ry = stick(joyaxis[3]);
		if (lx || ly || rx || ry)
			last_device(1);
		cl.viewangles[1] -= dt * joy_yawspeed->value * rx;
		cl.viewangles[0] += dt * joy_pitchspeed->value * ry * (joy_invert->ivalue ? -1 : 1);
		clamp_pitch();
		joy_move[0] = -ly * joy_movescale->value;
		joy_move[1] = lx * joy_movescale->value;
	} else {
		joy_move[0] = joy_move[1] = 0;
	}

	VectorCopy(cl.viewangles, u->angles);
	u->movevalues[0] = speed * (cl_forwardspeed->value * walk - cl_backspeed->value * walkback);
	u->movevalues[1] = speed * cl_sidespeed->value *
	                   (button_amount(&buttons[B_MOVERIGHT]) - button_amount(&buttons[B_MOVELEFT]) - (strafe ? turn : 0));
	u->movevalues[0] += speed * (joy_move[0] > 0 ? cl_forwardspeed->value : cl_backspeed->value) * joy_move[0];
	u->movevalues[1] += speed * cl_sidespeed->value * joy_move[1];
	for (int k = 0; k < 2; k++) {
		/* keys and stick together never exceed full speed */
		float lim = k ? speed * cl_sidespeed->value : speed * BW_MAX(cl_forwardspeed->value, cl_backspeed->value);
		u->movevalues[k] = BW_MAX(-lim, BW_MIN(u->movevalues[k], lim));
	}
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
	bits |= numbered_bits();
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
	CL_DemoUsercmd(u);
	cl.clientcommandframe++;
	cl.cmd_start = now;
	pending_impulse = 0;
	for (int i = 0; i < B_COUNT; i++)
		buttons[i].impulse_down = false;
	for (int i = 0; i < 20; i++)
		numbered[i].impulse_down = false;
	memset(&cl.cmds[cl.clientcommandframe % CL_CMD_BACKUP], 0, sizeof(usercmd_t));
}

bool CL_GetInputState(uint32_t seq, usercmd_t *out)
{
	if (seq > cl.clientcommandframe || cl.clientcommandframe - seq >= CL_CMD_BACKUP)
		return false;
	*out = cl.cmds[seq % CL_CMD_BACKUP];
	return out->sequence == seq;
}

/* +p1 <button> .. -p4 <button>: a button for one player of a split screen.
 * There is one view here, so player 1's run as plain +button and the
 * others' have no one to go to. */
static void Seat_f(int argc, const char **argv)
{
	int seat = argv[0][2] - '1';
	if (seat != 0) {
		Con_DPrintf("%s: there is only one player view (seat 1)\n", argv[0]);
		return;
	}
	Cmd_ExecuteString(va("%c%s", argv[0][0], Cmd_Args()));
}

void IN_Init(void)
{
	for (int i = 1; i <= 4; i++) {
		Cmd_AddD(va("+p%d", i), Seat_f, "+pN <button>: press a button for split screen player N");
		Cmd_AddD(va("-p%d", i), Seat_f, "-pN <button>: release a button for split screen player N");
	}
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
	in_lastdevice = Cvar_Get("in_lastdevice", "0", CVAR_READONLY, "What was used last: 0 keyboard and mouse, 1 a controller (for button prompts)");
	joy_enable = Cvar_Get("joy_enable", "1", CVAR_ARCHIVE, "Use game controllers' sticks for moving and looking");
	joy_deadzone = Cvar_Get("joy_deadzone", "0.2", CVAR_ARCHIVE, "Stick travel ignored around the centre, 0..1");
	joy_curve = Cvar_Get("joy_curve", "2", CVAR_ARCHIVE, "Stick response exponent: 1 is linear, higher is finer near the centre");
	joy_yawspeed = Cvar_Get("joy_yawspeed", "220", CVAR_ARCHIVE, "Turn speed at full right-stick deflection, degrees per second");
	joy_pitchspeed = Cvar_Get("joy_pitchspeed", "160", CVAR_ARCHIVE, "Look speed at full right-stick deflection, degrees per second");
	joy_invert = Cvar_Get("joy_invert", "0", CVAR_ARCHIVE, "Invert the right stick's vertical look");
	joy_movescale = Cvar_Get("joy_movescale", "1", CVAR_ARCHIVE, "Scale for left-stick movement");
	m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE, "Average mouse movement over two frames");

	Cmd_Add("bind", Bind_f);
	Cmd_Add("unbind", Unbind_f);
	Cmd_Add("unbindall", Unbindall_f);
	Cmd_Add("bindlist", Bindlist_f);
	Cmd_AddD("rotate", Rotate_f, "rotate <degrees> [seconds]: turn the view");
	Cmd_AddD("bindlevel", Bindlevel_f, "bindlevel <key> <level> [command]: a binding that runs at a lower execution level");
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
	ADD(strafe);
	ADD(klook);
	ADD(rollleft);
	ADD(rollright);
	cl_6dof = Cvar_Get("cl_6dof", "0", 0, "No pitch limit and a rolling view (the server's 6dof cheat sets it)");
	cl_rollspeed = Cvar_Get("cl_rollspeed", "200", CVAR_ARCHIVE, "Degrees a second +rollleft and +rollright turn in 6dof");
	Cmd_Add("+mlook", mlook_f);
	Cmd_Add("-mlook", mlook_f);
#undef ADD
	for (int n = 0; n < 20; n++) {
		char name[16];
		if (n >= 3 && n <= 8)
			continue; /* registered above */
		snprintf(name, sizeof(name), "+button%d", n);
		Cmd_Add(name, numbered_button);
		name[0] = '-';
		Cmd_Add(name, numbered_button);
	}
}

/* the pointer moved while not grabbed (menus, cursor mode) */
float in_mouse_x, in_mouse_y;
void IN_MouseAbs(float x, float y)
{
	in_mouse_x = x;
	in_mouse_y = y;
	UI_MouseMove(x, y);
	if (!Con_IsActive() && !MQC_InputEvent(3, x, y, 0))
		CSQC_InputEvent(3, x, y, 0);
}

/* focus lost: let go of everything so no button stays held */
void IN_ReleaseAll(void)
{
	for (int k = 0; k < K_MAX; k++)
		if (keydown[k])
			IN_KeyEvent(k, false, 0);
}
