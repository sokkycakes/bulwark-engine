/* Bulwark engine - 3DS screens and input. ISC licence, see LICENSE.
 *
 * The game draws on the top screen (400x240) through gl_ctr.c. The bottom
 * screen shows the engine's text output and works as a touch pad: a tap
 * clicks where it lands (mapped onto the top screen) in menus, and a drag
 * turns the view in game.
 *
 * Buttons arrive as controller 0, so the engine's controller bindings
 * apply (cl_input.c pad_defaults):
 *   circle pad  left stick (move)      C-stick    right stick (look, New 3DS)
 *   A B X Y     A B X Y                START      menu      SELECT  view
 *   R           right trigger (attack) L          left trigger
 *   ZR          right shoulder         ZL         left shoulder (New 3DS)
 *   D-pad       D-pad
 * L + SELECT toggles the console; with the console down, A opens the
 * system keyboard and runs what was typed. */
#include <3ds.h>
#include <string.h>

#include "../../client/client.h"
#include "../../client/console.h"
#include "../../client/keys.h"
#include "../../client/vid.h"
#include "../../common/cmd.h"
#include "../../common/common.h"
#include "ctr.h"

static bool active;
static bool grabbed;
static float mousex, mousey;
static bool touching;
static int lasttx, lastty;

static const struct {
	u32 bit;
	int key;
} buttons[] = {
	{KEY_A, K_GP_A},           {KEY_B, K_GP_B},           {KEY_X, K_GP_X},
	{KEY_Y, K_GP_Y},           {KEY_START, K_GP_MENU},    {KEY_SELECT, K_GP_VIEW},
	{KEY_R, K_GP_RTRIGGER},    {KEY_L, K_GP_LTRIGGER},    {KEY_ZR, K_GP_RSHOULDER},
	{KEY_ZL, K_GP_LSHOULDER},  {KEY_DUP, K_GP_DPAD_UP},   {KEY_DDOWN, K_GP_DPAD_DOWN},
	{KEY_DLEFT, K_GP_DPAD_LEFT}, {KEY_DRIGHT, K_GP_DPAD_RIGHT},
};

static void meminfo_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	CTR_PrintMemory("now");
}

bool VID_Init(int width, int height, bool fullscreen)
{
	(void)width, (void)height, (void)fullscreen;
	CTRGL_RegisterCvars();
	if (!CTRGL_Init())
		return false;
	active = true;
	IN_ControllerAdded(0);
	Cmd_AddD("ctr_meminfo", meminfo_f, "ctr_meminfo: print how much of the 3DS's memory is in use");
	return true;
}

void VID_Shutdown(void)
{
	if (active)
		CTRGL_Shutdown();
	active = false;
}

bool VID_Active(void) { return active; }
bool VID_Focused(void) { return true; }
void VID_ApplyMode(void) {}

void VID_GetSize(int *w, int *h)
{
	*w = CTR_WIDTH;
	*h = CTR_HEIGHT;
}

void VID_Swap(void) { CTRGL_EndFrame(); }
void *VID_GetProcAddress(const char *name) { return CTRGL_GetProc(name); }
void VID_SetMouseGrab(bool grab) { grabbed = grab; }
void VID_SetCaption(const char *text) { (void)text; }
bool VID_SetClipboard(const char *text) { (void)text; return false; }

void VID_GetMousePos(float *x, float *y)
{
	*x = mousex;
	*y = mousey;
}

void VID_SetMousePos(float x, float y)
{
	mousex = x;
	mousey = y;
}

void VID_Rumble(int dev, float low, float high, float seconds) { (void)dev, (void)low, (void)high, (void)seconds; }
void VID_RumbleTriggers(int dev, float left, float right, float seconds) { (void)dev, (void)left, (void)right, (void)seconds; }
void VID_SetControllerLED(int dev, const float *rgb) { (void)dev, (void)rgb; }

/* the system keyboard, for typing a console command */
static void type_command(void)
{
	SwkbdState kb;
	char text[256];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof(text) - 1);
	swkbdSetHintText(&kb, "console command");
	if (swkbdInputText(&kb, text, sizeof(text)) == SWKBD_BUTTON_CONFIRM && text[0]) {
		Con_Printf("]%s\n", text);
		Cbuf_AddText(text);
		Cbuf_AddText("\n");
	}
}

static float stick(int v)
{
	/* the circle pad reads about +-156 at its rim */
	float f = v / 150.0f;
	return BW_CLAMP(f, -1.0f, 1.0f);
}

bool VID_PumpEvents(void)
{
	u32 down, up, held;
	circlePosition cp, cs;
	touchPosition tp;
	if (!aptMainLoop())
		return false;
	hidScanInput();
	down = hidKeysDown();
	up = hidKeysUp();
	held = hidKeysHeld();

	if ((down & KEY_SELECT) && (held & KEY_L)) {
		Cbuf_AddText("toggleconsole\n");
		down &= ~(KEY_SELECT | KEY_L);
	} else if ((down & KEY_A) && Con_IsActive()) {
		type_command();
		down &= ~KEY_A;
	}
	for (size_t i = 0; i < BW_ARRAY_COUNT(buttons); i++) {
		if (down & buttons[i].bit)
			IN_KeyEvent(buttons[i].key, true, 0);
		if (up & buttons[i].bit)
			IN_KeyEvent(buttons[i].key, false, 0);
	}

	hidCircleRead(&cp);
	IN_JoyAxis(0, 0, stick(cp.dx));
	IN_JoyAxis(0, 1, -stick(cp.dy)); /* SDL's convention: down is positive */
	hidCstickRead(&cs);
	IN_JoyAxis(0, 2, stick(cs.dx));
	IN_JoyAxis(0, 3, -stick(cs.dy));

	if (held & KEY_TOUCH) {
		hidTouchRead(&tp);
		if (grabbed) {
			if (touching)
				IN_MouseMove((float)(tp.px - lasttx) * 2, (float)(tp.py - lastty) * 2);
		} else {
			/* the bottom screen (320x240) stands for the top one (400x240) */
			mousex = tp.px * (CTR_WIDTH / 320.0f);
			mousey = tp.py;
			IN_MouseAbs(mousex, mousey);
			if (!touching)
				IN_KeyEvent(K_MOUSE1, true, 0);
		}
		lasttx = tp.px, lastty = tp.py;
		touching = true;
	} else if (touching) {
		if (!grabbed)
			IN_KeyEvent(K_MOUSE1, false, 0);
		touching = false;
	}
	return true;
}
