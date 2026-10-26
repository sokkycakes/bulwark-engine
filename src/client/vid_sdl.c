/* Bulwark engine - SDL2 window, GL context and input. ISC licence, see LICENSE. */
#include "vid.h"
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cvar.h"
#include "../common/cmd.h"

#include <stdlib.h>

#include <SDL.h>

static SDL_Window *window;
static SDL_GLContext glctx;
static bool grabbed;
static cvar_t *vid_vsync;

/* --- game controllers ------------------------------------------------------ */

#define MAX_PADS 4
static struct {
	SDL_GameController *gc;
	SDL_JoystickID id;
	bool ltrig, rtrig; /* trigger keys, pressed past half travel */
} pads[MAX_PADS];

static int pad_for_id(SDL_JoystickID id)
{
	for (int i = 0; i < MAX_PADS; i++)
		if (pads[i].gc && pads[i].id == id)
			return i;
	return -1;
}

static void pad_open(int index)
{
	SDL_GameController *gc;
	int slot = -1;
	if (!SDL_IsGameController(index))
		return;
	for (int i = 0; i < MAX_PADS; i++)
		if (!pads[i].gc) {
			slot = i;
			break;
		}
	if (slot < 0 || !(gc = SDL_GameControllerOpen(index)))
		return;
	pads[slot].gc = gc;
	pads[slot].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
	pads[slot].ltrig = pads[slot].rtrig = false;
	Con_Printf("controller %d: %s\n", slot, SDL_GameControllerName(gc));
	IN_ControllerAdded(slot);
}

static void pad_close(SDL_JoystickID id)
{
	int i = pad_for_id(id);
	if (i < 0)
		return;
	Con_Printf("controller %d disconnected\n", i);
	SDL_GameControllerClose(pads[i].gc);
	memset(&pads[i], 0, sizeof(pads[i]));
	IN_ControllerRemoved(i);
}

static int pad_button(int b)
{
	switch (b) {
	case SDL_CONTROLLER_BUTTON_A: return K_GP_A;
	case SDL_CONTROLLER_BUTTON_B: return K_GP_B;
	case SDL_CONTROLLER_BUTTON_X: return K_GP_X;
	case SDL_CONTROLLER_BUTTON_Y: return K_GP_Y;
	case SDL_CONTROLLER_BUTTON_BACK: return K_GP_VIEW;
	case SDL_CONTROLLER_BUTTON_START: return K_GP_MENU;
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: return K_GP_LTHUMB;
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return K_GP_RTHUMB;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_GP_LSHOULDER;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_GP_RSHOULDER;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_GP_DPAD_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_GP_DPAD_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_GP_DPAD_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_GP_DPAD_RIGHT;
	default: return 0;
	}
}

static void pad_axis(int slot, int axis, int16_t raw)
{
	float v = raw < 0 ? raw / 32768.0f : raw / 32767.0f;
	if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
		bool *held = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? &pads[slot].ltrig : &pads[slot].rtrig;
		int key = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? K_GP_LTRIGGER : K_GP_RTRIGGER;
		/* a little hysteresis so a resting finger doesn't chatter */
		if (!*held && v > 0.5f)
			*held = true, IN_KeyEvent(key, true, 0);
		else if (*held && v < 0.4f)
			*held = false, IN_KeyEvent(key, false, 0);
	}
	IN_JoyAxis(slot, axis, v);
}

/* "joy_virtual" attaches a pretend controller and "joy_virtual_set axis|button
 * n value" moves it, for trying controller handling without hardware */
static SDL_Joystick *virtual_pad;

static void JoyVirtual_f(int argc, const char **argv)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	int index;
	if (virtual_pad)
		return;
	index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
	                                  SDL_CONTROLLER_BUTTON_MAX, 0);
	if (index < 0 || !(virtual_pad = SDL_JoystickOpen(index)))
		Con_Printf("joy_virtual: %s\n", SDL_GetError());
#endif
}

static void JoyVirtualSet_f(int argc, const char **argv)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	if (!virtual_pad || argc < 4) {
		Con_Printf("joy_virtual_set axis|button <n> <value>: after joy_virtual\n");
		return;
	}
	if (!strcmp(argv[1], "axis"))
		SDL_JoystickSetVirtualAxis(virtual_pad, atoi(argv[2]), (Sint16)BW_MAX(-32768, BW_MIN(32767, atof(argv[3]) * 32767)));
	else
		SDL_JoystickSetVirtualButton(virtual_pad, atoi(argv[2]), (Uint8)atoi(argv[3]));
#endif
}

void VID_Rumble(int dev, float low, float high, float seconds)
{
	if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerRumble(pads[dev].gc, (Uint16)(BW_MAX(0, BW_MIN(low, 1)) * 65535),
		                         (Uint16)(BW_MAX(0, BW_MIN(high, 1)) * 65535), (Uint32)(BW_MAX(seconds, 0) * 1000));
}

void VID_RumbleTriggers(int dev, float left, float right, float seconds)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerRumbleTriggers(pads[dev].gc, (Uint16)(BW_MAX(0, BW_MIN(left, 1)) * 65535),
		                                 (Uint16)(BW_MAX(0, BW_MIN(right, 1)) * 65535),
		                                 (Uint32)(BW_MAX(seconds, 0) * 1000));
#endif
}

void VID_SetControllerLED(int dev, const float *rgb)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerSetLED(pads[dev].gc, (Uint8)(BW_MAX(0, BW_MIN(rgb[0], 1)) * 255),
		                         (Uint8)(BW_MAX(0, BW_MIN(rgb[1], 1)) * 255), (Uint8)(BW_MAX(0, BW_MIN(rgb[2], 1)) * 255));
#endif
}

static int map_key(SDL_Keycode k)
{
	if (k >= 'a' && k <= 'z')
		return (int)k;
	if (k >= 32 && k < 127)
		return (int)k;
	switch (k) {
	case SDLK_TAB: return K_TAB;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP: return K_UPARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;
	case SDLK_LALT: case SDLK_RALT: return K_LALT;
	case SDLK_LCTRL: case SDLK_RCTRL: return K_LCTRL;
	case SDLK_LSHIFT: case SDLK_RSHIFT: return K_LSHIFT;
	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_INSERT: return K_INS;
	case SDLK_DELETE: return K_DEL;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;
	case SDLK_PAUSE: return K_PAUSE;
	case SDLK_NUMLOCKCLEAR: return K_KP_NUMLOCK;
	case SDLK_CAPSLOCK: return K_CAPSLOCK;
	case SDLK_SCROLLLOCK: return K_SCROLLLOCK;
	case SDLK_KP_0: return K_KP_INS;
	case SDLK_KP_1: return K_KP_END;
	case SDLK_KP_2: return K_KP_DOWNARROW;
	case SDLK_KP_3: return K_KP_PGDN;
	case SDLK_KP_4: return K_KP_LEFTARROW;
	case SDLK_KP_5: return K_KP_5;
	case SDLK_KP_6: return K_KP_RIGHTARROW;
	case SDLK_KP_7: return K_KP_HOME;
	case SDLK_KP_8: return K_KP_UPARROW;
	case SDLK_KP_9: return K_KP_PGUP;
	case SDLK_KP_PERIOD: return K_KP_DEL;
	case SDLK_KP_DIVIDE: return K_KP_SLASH;
	case SDLK_KP_MULTIPLY: return K_KP_STAR;
	case SDLK_KP_MINUS: return K_KP_MINUS;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_KP_EQUALS: return K_KP_EQUALS;
	case SDLK_PRINTSCREEN: return K_PRINTSCREEN;
	default: return 0;
	}
}

bool VID_Init(int width, int height, bool fullscreen)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		Con_Printf("SDL_Init: %s\n", SDL_GetError());
		return false;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	window = SDL_CreateWindow("Bulwark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
	                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
	                              (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!window) {
		Con_Printf("SDL_CreateWindow: %s\n", SDL_GetError());
		SDL_Quit();
		return false;
	}
	glctx = SDL_GL_CreateContext(window);
	if (!glctx) {
		Con_Printf("OpenGL 3.3 core context: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		window = NULL;
		SDL_Quit();
		return false;
	}
	vid_vsync = Cvar_Get("vid_vsync", "1", CVAR_ARCHIVE, "Wait for the display before showing a frame");
	SDL_GL_SetSwapInterval(vid_vsync->value ? 1 : 0);
	SDL_StartTextInput();
	/* controllers are optional: devices already plugged in arrive as
	 * CONTROLLERDEVICEADDED events */
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
		Con_DPrintf("no game controller support: %s\n", SDL_GetError());
	Cmd_Add("joy_virtual", JoyVirtual_f);
	Cmd_Add("joy_virtual_set", JoyVirtualSet_f);
	return true;
}

void VID_Shutdown(void)
{
	if (!window)
		return;
	for (int i = 0; i < MAX_PADS; i++)
		if (pads[i].gc)
			SDL_GameControllerClose(pads[i].gc);
	memset(pads, 0, sizeof(pads));
	SDL_GL_DeleteContext(glctx);
	SDL_DestroyWindow(window);
	window = NULL;
	SDL_Quit();
}

bool VID_Active(void) { return window != NULL; }

void VID_GetSize(int *w, int *h)
{
	if (window)
		SDL_GL_GetDrawableSize(window, w, h);
	else
		*w = 640, *h = 480;
}

void VID_Swap(void)
{
	static int vsync = -1;
	if (!window)
		return;
	if (vsync != (int)vid_vsync->value) {
		vsync = (int)vid_vsync->value;
		SDL_GL_SetSwapInterval(vsync ? 1 : 0);
	}
	SDL_GL_SwapWindow(window);
}

void *VID_GetProcAddress(const char *name) { return SDL_GL_GetProcAddress(name); }

void VID_SetMouseGrab(bool grab)
{
	if (!window || grab == grabbed)
		return;
	grabbed = grab;
	SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
}

void VID_SetCaption(const char *text)
{
	if (window)
		SDL_SetWindowTitle(window, text);
}

void VID_GetMousePos(float *x, float *y)
{
	int ix, iy;
	SDL_GetMouseState(&ix, &iy);
	*x = (float)ix;
	*y = (float)iy;
}

void VID_SetMousePos(float x, float y)
{
	if (window)
		SDL_WarpMouseInWindow(window, (int)x, (int)y);
}

static int mouse_button(int b)
{
	switch (b) {
	case SDL_BUTTON_LEFT: return K_MOUSE1;
	case SDL_BUTTON_RIGHT: return K_MOUSE2;
	case SDL_BUTTON_MIDDLE: return K_MOUSE3;
	case SDL_BUTTON_X1: return K_MOUSE4;
	case SDL_BUTTON_X2: return K_MOUSE5;
	default: return 0;
	}
}

/* decodes one UTF-8 code point */
static int utf8_first(const char *s)
{
	const unsigned char *u = (const unsigned char *)s;
	if (u[0] < 0x80)
		return u[0];
	if ((u[0] & 0xe0) == 0xc0)
		return ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
	if ((u[0] & 0xf0) == 0xe0)
		return ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
	return ((u[0] & 0x07) << 18) | ((u[1] & 0x3f) << 12) | ((u[2] & 0x3f) << 6) | (u[3] & 0x3f);
}

bool VID_PumpEvents(void)
{
	SDL_Event ev;
	/* a printable key's character arrives in the TEXTINPUT that follows its
	 * KEYDOWN, so the key event waits for it */
	int pending_key = 0;
	if (!window)
		return true;
	while (SDL_PollEvent(&ev)) {
		if (pending_key && ev.type != SDL_TEXTINPUT) {
			IN_KeyEvent(pending_key, true, 0);
			pending_key = 0;
		}
		switch (ev.type) {
		case SDL_QUIT:
			return false;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			int k = map_key(ev.key.keysym.sym);
			if (!k)
				break;
			if (ev.type == SDL_KEYDOWN && k >= 32 && k < 127 && !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_ALT)))
				pending_key = k;
			else
				IN_KeyEvent(k, ev.type == SDL_KEYDOWN, ev.type == SDL_KEYDOWN && k < 32 ? k : 0);
			break;
		}
		case SDL_TEXTINPUT: {
			int c = utf8_first(ev.text.text);
			IN_KeyEvent(pending_key, true, c);
			pending_key = 0;
			break;
		}
		case SDL_MOUSEMOTION:
			if (grabbed)
				IN_MouseMove((float)ev.motion.xrel, (float)ev.motion.yrel);
			else
				IN_MouseAbs((float)ev.motion.x, (float)ev.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int k = mouse_button(ev.button.button);
			if (k)
				IN_KeyEvent(k, ev.type == SDL_MOUSEBUTTONDOWN, 0);
			break;
		}
		case SDL_MOUSEWHEEL:
			if (ev.wheel.y > 0) {
				IN_KeyEvent(K_MWHEELUP, true, 0);
				IN_KeyEvent(K_MWHEELUP, false, 0);
			} else if (ev.wheel.y < 0) {
				IN_KeyEvent(K_MWHEELDOWN, true, 0);
				IN_KeyEvent(K_MWHEELDOWN, false, 0);
			}
			break;
		case SDL_CONTROLLERDEVICEADDED:
			pad_open(ev.cdevice.which);
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			pad_close(ev.cdevice.which);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			int k = pad_button(ev.cbutton.button);
			if (k && pad_for_id(ev.cbutton.which) >= 0)
				IN_KeyEvent(k, ev.type == SDL_CONTROLLERBUTTONDOWN, 0);
			break;
		}
		case SDL_CONTROLLERAXISMOTION: {
			int slot = pad_for_id(ev.caxis.which);
			if (slot >= 0)
				pad_axis(slot, ev.caxis.axis, ev.caxis.value);
			break;
		}
		case SDL_WINDOWEVENT:
			if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				IN_ReleaseAll();
			break;
		}
	}
	if (pending_key)
		IN_KeyEvent(pending_key, true, 0);
	return true;
}
