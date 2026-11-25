/* Bulwark engine - SDL2 window, GL context and input. ISC licence, see LICENSE. */
#include "vid.h"
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cvar.h"
#include "../common/cmd.h"
#include "in_pads.h"
#include "steaminput.h"

#include <stdlib.h>

#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

static SDL_Window *window;
static SDL_GLContext glctx;
static bool grabbed;
static cvar_t *vid_vsync, *vid_width, *vid_height, *vid_fullscreen;

#ifdef __EMSCRIPTEN__
/* In a browser the page (tools/web/shell) owns the canvas's size, the
 * pointer lock and fullscreen, since browsers allow those only from a click
 * or key press. The engine says what it wants; the page makes it so. */
/* clang-format off */
EM_JS(void, web_set_grab, (int grab), {
	if (Module.bulwarkSetGrab) Module.bulwarkSetGrab(!!grab);
});
EM_JS(void, web_set_fullscreen, (int on), {
	if (Module.bulwarkSetFullscreen) Module.bulwarkSetFullscreen(!!on);
});
EM_JS(int, web_is_fullscreen, (void), {
	return Module.bulwarkIsFullscreen ? (Module.bulwarkIsFullscreen() ? 1 : 0) : 0;
});
EM_JS(void, web_set_title, (const char *text), {
	if (Module.bulwarkSetTitle) Module.bulwarkSetTitle(UTF8ToString(text));
});
/* clang-format on */

/* the page lost the pointer lock on its own (Esc, alt-tab): the game hears
 * an Esc, so it opens its menu as it would on the desktop, unless the Esc
 * the player pressed already reached it */
EMSCRIPTEN_KEEPALIVE void bulwark_pointer_lock_lost(int esc_seen)
{
	if (!grabbed)
		return;
	if (!esc_seen) {
		IN_KeyEvent(K_ESCAPE, true, 0);
		IN_KeyEvent(K_ESCAPE, false, 0);
	}
}

/* the page's window or fullscreen state changed size: follow the canvas */
static void web_follow_canvas(void)
{
	double cw, ch;
	int w, h;
	if (!window || emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS)
		return;
	SDL_GetWindowSize(window, &w, &h);
	if ((int)cw > 0 && (int)ch > 0 && ((int)cw != w || (int)ch != h))
		SDL_SetWindowSize(window, (int)cw, (int)ch);
}
#endif

/* --- game controllers ------------------------------------------------------ */

/* slots hold an SDL controller or a Steam Input one (in_steam.c); a slot is
 * a split screen device, device = slot + 1 */
static struct {
	SDL_GameController *gc;
	SDL_JoystickID id;
	uint64_t steam;    /* a Steam Input handle, when the slot is Steam Input's */
	si_set_t glyphs;   /* the prompts this pad wants */
	bool ltrig, rtrig; /* trigger keys, pressed past half travel */
} pads[MAX_PADS];

static cvar_t *in_padglyphs;
static cvar_t *joy_rumble; /* scales every rumble, 0 turns it off */
static int last_pad = -1;

static int pad_for_id(SDL_JoystickID id)
{
	for (int i = 0; i < MAX_PADS; i++)
		if (pads[i].gc && pads[i].id == id)
			return i;
	return -1;
}

/* the pad a player used last: its prompts are what HUDs show */
void Pad_Touched(int slot)
{
	if (slot < 0 || slot >= MAX_PADS || slot == last_pad)
		return;
	last_pad = slot;
	if (in_padglyphs)
		Cvar_SetDirect(in_padglyphs, si_set_name(pads[slot].glyphs));
}

int Pad_ClaimSteam(uint64_t handle)
{
	for (int i = 0; i < MAX_PADS; i++)
		if (!pads[i].gc && !pads[i].steam) {
			memset(&pads[i], 0, sizeof(pads[i]));
			pads[i].steam = handle;
			pads[i].glyphs = si_set_for_controller(handle);
			return i;
		}
	return -1;
}

void Pad_Release(int slot)
{
	if (slot >= 0 && slot < MAX_PADS && pads[slot].steam)
		memset(&pads[slot], 0, sizeof(pads[slot]));
}

/* SDL_GameControllerGetSteamHandle came with SDL 2.30: looked up when the
 * SDL this runs with has it */
static uint64_t pad_steam_handle(SDL_GameController *gc)
{
#if SDL_VERSION_ATLEAST(2, 30, 0)
	SDL_version v;
	SDL_GetVersion(&v);
	if (SDL_VERSIONNUM(v.major, v.minor, v.patch) >= SDL_VERSIONNUM(2, 30, 0))
		return SDL_GameControllerGetSteamHandle(gc);
#endif
	return 0;
}

/* what a pad really is, and so its prompts. Run by Steam (as a Steam app
 * or as a non-Steam game), a game sees Steam's virtual gamepad instead of
 * the device: SDL 2.30 reads Steam's SteamVirtualGamepadInfo file and
 * reports the real device; an older SDL reports Valve's virtual pad id, and
 * the file is read here instead */
static si_set_t pad_glyphs(SDL_GameController *gc, char *how, size_t howsize)
{
	uint16_t vid = SDL_GameControllerGetVendor(gc), pid = SDL_GameControllerGetProduct(gc);
	int type = (int)SDL_GameControllerGetType(gc);
	const char *name = SDL_GameControllerName(gc);
	snprintf(how, howsize, "%s", pad_steam_handle(gc) ? ", through Steam Input's virtual gamepad" : "");
	if (vid == SI_VID_VALVE && pid == SI_PID_STEAM_VIRTUAL_GAMEPAD) {
		int slot = si_virtual_gamepad_slot(name);
		uint16_t rv = 0, rp = 0;
		if (slot < 0)
			slot = SDL_GameControllerGetPlayerIndex(gc); /* XInput's index on Windows */
		if (si_virtual_gamepad_info(slot, &rv, &rp, NULL, 0) && rv) {
			vid = rv, pid = rp, type = SI_SDLTYPE_UNKNOWN;
			snprintf(how, howsize, ", through Steam's virtual gamepad %d", slot);
		} else {
			/* no word on what's behind it: on a Deck it is the Deck */
			const char *deck = getenv("SteamDeck");
			snprintf(how, howsize, ", Steam's virtual gamepad");
			if (deck && atoi(deck))
				return SI_SET_STEAMDECK;
		}
	}
	return si_set_for_device(vid, pid, type);
}

static void pad_open(int index)
{
	SDL_GameController *gc;
	char how[96];
	int slot = -1;
	if (!SDL_IsGameController(index))
		return;
	for (int i = 0; i < MAX_PADS; i++)
		if (!pads[i].gc && !pads[i].steam) {
			slot = i;
			break;
		}
	if (slot < 0 || !(gc = SDL_GameControllerOpen(index)))
		return;
	pads[slot].gc = gc;
	pads[slot].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
	pads[slot].ltrig = pads[slot].rtrig = false;
	pads[slot].glyphs = pad_glyphs(gc, how, sizeof(how));
	Con_Printf("controller %d: %s (%s glyphs%s)\n", slot, SDL_GameControllerName(gc),
	           si_set_name(pads[slot].glyphs), how);
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
#if SDL_VERSION_ATLEAST(2, 0, 14)
	/* SDL's paddles 1-4 are the Deck's R4, L4, R5, L5 */
	case SDL_CONTROLLER_BUTTON_PADDLE1: return K_GP_R4;
	case SDL_CONTROLLER_BUTTON_PADDLE2: return K_GP_L4;
	case SDL_CONTROLLER_BUTTON_PADDLE3: return K_GP_R5;
	case SDL_CONTROLLER_BUTTON_PADDLE4: return K_GP_L5;
	case SDL_CONTROLLER_BUTTON_TOUCHPAD: return K_GP_TOUCHPAD;
	case SDL_CONTROLLER_BUTTON_MISC1: return K_GP_MISC1;
#endif
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
			*held = true, IN_DeviceKeyEvent(slot + 1, key, true, 0);
		else if (*held && v < 0.4f)
			*held = false, IN_DeviceKeyEvent(slot + 1, key, false, 0);
	}
	IN_JoyAxis(slot, axis, v);
}

/* "joy_virtual [count]" attaches pretend controllers (up to four, for split
 * screen) and "joy_virtual_set [pad] axis|button n value" moves one, for
 * trying controller handling without hardware */
static SDL_Joystick *virtual_pads[MAX_PADS];

static void JoyVirtual_f(int argc, const char **argv)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	int want = argc > 1 ? BW_CLAMP(atoi(argv[1]), 1, MAX_PADS) : 1;
	for (int i = 0; i < want; i++) {
		int index;
		if (virtual_pads[i])
			continue;
		index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
		                                  SDL_CONTROLLER_BUTTON_MAX, 0);
		if (index < 0 || !(virtual_pads[i] = SDL_JoystickOpen(index))) {
			Con_Printf("joy_virtual: %s\n", SDL_GetError());
			return;
		}
	}
#endif
}

/* joy_virtual_dev <vendor> <product> [name]: a pretend controller that
 * says it is that device ("joy_virtual_dev 0x28de 0x1205" is a Steam Deck,
 * "0x28de 0x11ff Steam Virtual Gamepad pad 0" Steam's virtual gamepad) */
static void JoyVirtualDev_f(int argc, const char **argv)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
	SDL_VirtualJoystickDesc d;
	int index, i;
	if (argc < 3) {
		Con_Printf("joy_virtual_dev <vendor> <product> [name]\n");
		return;
	}
	for (i = 0; i < MAX_PADS && virtual_pads[i]; i++)
		;
	if (i == MAX_PADS)
		return;
	SDL_zero(d);
	d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	d.naxes = SDL_CONTROLLER_AXIS_MAX;
	d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	d.vendor_id = (Uint16)strtoul(argv[1], NULL, 0);
	d.product_id = (Uint16)strtoul(argv[2], NULL, 0);
	d.name = argc > 3 ? argv[3] : "Virtual controller";
	index = SDL_JoystickAttachVirtualEx(&d);
	if (index < 0 || !(virtual_pads[i] = SDL_JoystickOpen(index)))
		Con_Printf("joy_virtual_dev: %s\n", SDL_GetError());
#else
	Con_Printf("joy_virtual_dev needs SDL 2.24\n");
#endif
}

static void JoyVirtualSet_f(int argc, const char **argv)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	int pad = 0;
	SDL_Joystick *j;
	if (argc == 5) {
		pad = atoi(argv[1]);
		argv++, argc--;
	}
	j = pad >= 0 && pad < MAX_PADS ? virtual_pads[pad] : NULL;
	if (!j || argc < 4) {
		Con_Printf("joy_virtual_set [pad] axis|button <n> <value>: after joy_virtual\n");
		return;
	}
	if (!strcmp(argv[1], "axis"))
		SDL_JoystickSetVirtualAxis(j, atoi(argv[2]), (Sint16)BW_MAX(-32768, BW_MIN(32767, atof(argv[3]) * 32767)));
	else
		SDL_JoystickSetVirtualButton(j, atoi(argv[2]), (Uint8)atoi(argv[3]));
#endif
}

static float rumble_scale(void)
{
	return joy_rumble ? BW_MAX(0, BW_MIN(joy_rumble->value, 1)) : 1;
}

void VID_Rumble(int dev, float low, float high, float seconds)
{
	low *= rumble_scale(), high *= rumble_scale();
	if (dev >= 0 && dev < MAX_PADS && pads[dev].steam)
		INS_Rumble(dev, low, high, seconds);
	else if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerRumble(pads[dev].gc, (Uint16)(BW_MAX(0, BW_MIN(low, 1)) * 65535),
		                         (Uint16)(BW_MAX(0, BW_MIN(high, 1)) * 65535), (Uint32)(BW_MAX(seconds, 0) * 1000));
}

void VID_RumbleTriggers(int dev, float left, float right, float seconds)
{
	left *= rumble_scale(), right *= rumble_scale();
#if SDL_VERSION_ATLEAST(2, 0, 14)
	if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerRumbleTriggers(pads[dev].gc, (Uint16)(BW_MAX(0, BW_MIN(left, 1)) * 65535),
		                                 (Uint16)(BW_MAX(0, BW_MIN(right, 1)) * 65535),
		                                 (Uint32)(BW_MAX(seconds, 0) * 1000));
#endif
}

void VID_SetControllerLED(int dev, const float *rgb)
{
	if (dev >= 0 && dev < MAX_PADS && pads[dev].steam) {
		INS_LED(dev, rgb);
		return;
	}
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

/* vid_restart/vid_reload: apply vid_width, vid_height and vid_fullscreen
 * (0 windowed, 1 exclusive, 2 desktop). The GL context lives on, so the
 * games are told the renderer restarted rather than everything reloading. */
static void apply_mode(void)
{
	int w = vid_width->ivalue, h = vid_height->ivalue, fs = vid_fullscreen->ivalue;
	if (!window)
		return;
#ifdef __EMSCRIPTEN__
	/* the page is the window: its size is the browser's, and fullscreen is
	 * the page's (only a click or key may start it, so the page waits for one) */
	(void)w, (void)h;
	web_set_fullscreen(fs != 0);
	web_follow_canvas();
	return;
#endif
	if (fs) {
		if (fs == 1 && w > 0 && h > 0) {
			SDL_DisplayMode mode;
			if (SDL_GetWindowDisplayMode(window, &mode) == 0) {
				mode.w = w;
				mode.h = h;
				SDL_SetWindowDisplayMode(window, &mode);
			}
		}
		SDL_SetWindowFullscreen(window, fs == 1 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
	} else {
		SDL_SetWindowFullscreen(window, 0);
		if (w > 0 && h > 0) {
			SDL_SetWindowSize(window, w, h);
			SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		}
	}
}

void VID_ApplyMode(void) { apply_mode(); }

static void VidRestart_f(int argc, const char **argv)
{
	apply_mode();
	CSQC_RendererRestarted();
	MQC_RendererRestarted();
}

static void VidToggle_f(int argc, const char **argv)
{
#ifdef __EMSCRIPTEN__
	/* the player may have left fullscreen with Esc or F11 */
	Cvar_Set("vid_fullscreen", web_is_fullscreen() ? "0" : "2");
	apply_mode();
	return;
#endif
	Cvar_Set("vid_fullscreen", vid_fullscreen->ivalue ? "0" : "2");
	apply_mode();
}

/* in_restart: close every controller and open the ones plugged in */
static void InRestart_f(int argc, const char **argv)
{
	for (int i = 0; i < MAX_PADS; i++)
		if (pads[i].gc)
			pad_close(pads[i].id);
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_IsGameController(i))
			pad_open(i);
}

/* in_deviceids: the controllers and their slots */
static void InDeviceIds_f(int argc, const char **argv)
{
	Con_Printf("keyboard and mouse: device 0\n");
	for (int i = 0; i < MAX_PADS; i++)
		if (pads[i].gc)
			Con_Printf("controller %d: %s (%s glyphs)%s\n", i, SDL_GameControllerName(pads[i].gc),
			           si_set_name(pads[i].glyphs), INS_Active() ? ", unused while Steam Input has controllers" : "");
		else if (pads[i].steam)
			Con_Printf("controller %d: Steam Input %llu (%s glyphs)\n", i, (unsigned long long)pads[i].steam,
			           si_set_name(pads[i].glyphs));
}

/* the key that "what" names, or the controller key bound to the command
 * "what" (the first in this order, so A beats the d-pad for the same command) */
static int pad_key_for(const char *what)
{
	static const int order[] = {K_GP_A, K_GP_B, K_GP_X, K_GP_Y, K_GP_RTRIGGER, K_GP_LTRIGGER, K_GP_RSHOULDER,
	                            K_GP_LSHOULDER, K_GP_LTHUMB, K_GP_RTHUMB, K_GP_MENU, K_GP_VIEW, K_GP_DPAD_UP,
	                            K_GP_DPAD_DOWN, K_GP_DPAD_LEFT, K_GP_DPAD_RIGHT, K_GP_L4, K_GP_R4, K_GP_L5,
	                            K_GP_R5, K_GP_TOUCHPAD, K_GP_MISC1};
	int k = Key_NumForName(what);
	if (k > 0)
		return k;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(order); i++)
		if (!Str_ICmp(Key_GetBinding(order[i]), what))
			return order[i];
	return -1;
}

static si_button_t key_button(int key)
{
	switch (key) {
	case K_GP_A: return SI_BTN_SOUTH;
	case K_GP_B: return SI_BTN_EAST;
	case K_GP_X: return SI_BTN_WEST;
	case K_GP_Y: return SI_BTN_NORTH;
	case K_GP_LSHOULDER: return SI_BTN_LB;
	case K_GP_RSHOULDER: return SI_BTN_RB;
	case K_GP_LTRIGGER: return SI_BTN_LT;
	case K_GP_RTRIGGER: return SI_BTN_RT;
	case K_GP_LTHUMB: return SI_BTN_LCLICK;
	case K_GP_RTHUMB: return SI_BTN_RCLICK;
	case K_GP_MENU: return SI_BTN_START;
	case K_GP_VIEW: return SI_BTN_BACK;
	case K_GP_DPAD_UP: return SI_BTN_DUP;
	case K_GP_DPAD_DOWN: return SI_BTN_DDOWN;
	case K_GP_DPAD_LEFT: return SI_BTN_DLEFT;
	case K_GP_DPAD_RIGHT: return SI_BTN_DRIGHT;
	case K_GP_L4: return SI_BTN_L4;
	case K_GP_L5: return SI_BTN_L5;
	case K_GP_R4: return SI_BTN_R4;
	case K_GP_R5: return SI_BTN_R5;
	case K_GP_TOUCHPAD: return SI_BTN_CPAD;
	default: return SI_BTN_NONE;
	}
}

/* IN_PadGlyph: a prompt for a key or command on a controller */
static bool pad_glyph(const char *what, int dev, char *path, size_t pathsize, char *label, size_t labelsize)
{
	int slot = dev > 0 ? dev - 1 : last_pad, key;
	si_set_t set;
	si_button_t btn;
	if (slot < 0 || slot >= MAX_PADS) {
		/* nobody has used one: the first connected, else Xbox's look */
		for (slot = 0; slot < MAX_PADS && !pads[slot].gc && !pads[slot].steam; slot++)
			;
	}
	if (slot < MAX_PADS && pads[slot].steam)
		return INS_Glyph(slot, what, path, pathsize, label, labelsize);
	set = slot < MAX_PADS && pads[slot].gc ? pads[slot].glyphs : SI_SET_XBOX;
	key = pad_key_for(what);
	if (key < 0 || !(btn = key_button(key)))
		return false;
	snprintf(path, pathsize, si_xelu_file(set, btn) ? "{images}/glyphs/%s/%s.png" : "", si_set_name(set),
	         si_button_name(btn));
	snprintf(label, labelsize, "%s", si_button_label(set, btn));
	return true;
}

/* in_glyph <key or command> [device]: the prompt a HUD would show */
static void InGlyph_f(int argc, const char **argv)
{
	char path[128], label[64];
	if (argc < 2) {
		Con_Printf("in_glyph <key or command> [device]: e.g. in_glyph +jump\n");
		return;
	}
	if (pad_glyph(argv[1], argc > 2 ? atoi(argv[2]) : -1, path, sizeof(path), label, sizeof(label)))
		Con_Printf("%s: %s \"%s\"\n", argv[1], path[0] ? path : "(no image)", label);
	else
		Con_Printf("%s: no controller binding\n", argv[1]);
}

/* vid_modelist: the display's modes */
static void VidModes_f(int argc, const char **argv)
{
	int display = window ? SDL_GetWindowDisplayIndex(window) : 0;
	for (int i = 0; i < SDL_GetNumDisplayModes(display); i++) {
		SDL_DisplayMode m;
		if (SDL_GetDisplayMode(display, i, &m) == 0)
			Con_Printf("%dx%d %dHz\n", m.w, m.h, m.refresh_rate);
	}
}

bool VID_Init(int width, int height, bool fullscreen)
{
#ifdef SDL_HINT_APP_NAME
	SDL_SetHint(SDL_HINT_APP_NAME, "Bulwark"); /* window class and audio stream name */
#endif
#ifdef __EMSCRIPTEN__
	/* SDL drops controller input while its window lacks keyboard focus, and
	 * in a page that's whenever focus sits on the page rather than the
	 * canvas (after a tap on a handheld's screen, say). The page is the
	 * window: a hidden tab gets no frames anyway. */
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#endif
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		Con_Printf("SDL_Init: %s\n", SDL_GetError());
		return false;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __EMSCRIPTEN__
	/* WebGL 2, which is OpenGL ES 3.0: the shaders get an ES header (r_gl.c) */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	/* the page sizes the canvas; the window takes its size from there */
	fullscreen = false;
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	window = SDL_CreateWindow("Bulwark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
	                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
#ifdef __EMSCRIPTEN__
	                              SDL_WINDOW_ALLOW_HIGHDPI | /* sharp on phones and handhelds */
#endif
	                              (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!window) {
		Con_Printf("SDL_CreateWindow: %s\n", SDL_GetError());
		SDL_Quit();
		return false;
	}
	glctx = SDL_GL_CreateContext(window);
	if (!glctx) {
#ifdef __EMSCRIPTEN__
		Con_Printf("WebGL 2 context: %s\n", SDL_GetError());
#else
		Con_Printf("OpenGL 3.3 core context: %s\n", SDL_GetError());
#endif
		SDL_DestroyWindow(window);
		window = NULL;
		SDL_Quit();
		return false;
	}
	vid_vsync = Cvar_Get("vid_vsync", "1", CVAR_ARCHIVE, "Wait for the display before showing a frame");
	vid_width = Cvar_Get("vid_width", va("%d", width), CVAR_ARCHIVE, "Window or screen width (vid_restart applies it)");
	vid_height = Cvar_Get("vid_height", va("%d", height), CVAR_ARCHIVE, "Window or screen height (vid_restart applies it)");
	vid_fullscreen = Cvar_Get("vid_fullscreen", fullscreen ? "2" : "0", CVAR_ARCHIVE,
	                          "0 windowed, 1 fullscreen at vid_width x vid_height, 2 fullscreen at the desktop size");
	Cmd_AddD("vid_restart", VidRestart_f, "Apply the video mode cvars and reload the games' renderer resources");
	Cmd_AddD("vid_reload", VidRestart_f, "Reload the games' renderer resources");
	Cmd_AddD("vid_toggle", VidToggle_f, "Switch between fullscreen and a window");
	Cmd_AddD("vid_modelist", VidModes_f, "List the display's video modes");
	Cmd_AddD("in_restart", InRestart_f, "Reopen the game controllers");
	Cmd_AddD("in_deviceids", InDeviceIds_f, "List the input devices");
	SDL_GL_SetSwapInterval(vid_vsync->value ? 1 : 0);
	SDL_StartTextInput();
	in_padglyphs = Cvar_Get("in_padglyphs", "xbox", CVAR_READONLY,
	                        "Button prompts for the controller used last: xbox, xbox360, ps4, ps5, switch, steamdeck "
	                        "or steamcontroller ({images}/glyphs/<set>/)");
	IN_PadGlyph = pad_glyph;
	joy_rumble = Cvar_Get("joy_rumble", "1", CVAR_ARCHIVE, "Controller vibration strength, 0 (off) to 1");
	Cmd_AddD("in_glyph", InGlyph_f, "Show the controller prompt for a key or command");
	/* Steam Input first: whether Steam runs decides how SDL should treat
	 * Valve's controllers */
	{
		char *base = SDL_GetBasePath();
		INS_Init(base);
		SDL_free(base);
	}
	/* buttons by position, as Steam Input names them: A is the bottom
	 * button on every pad, a Switch pad's B included */
	SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#ifdef SDL_HINT_JOYSTICK_HIDAPI_STEAM
	/* without Steam, read a Steam Controller directly (SDL leaves it to
	 * Steam by default, since both reading it at once fights) */
	if (!si_active() && si_launch() == SI_LAUNCH_PLAIN)
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
#endif
	/* controllers are optional: devices already plugged in arrive as
	 * CONTROLLERDEVICEADDED events */
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
		Con_DPrintf("no game controller support: %s\n", SDL_GetError());
	Cmd_Add("joy_virtual", JoyVirtual_f);
	Cmd_Add("joy_virtual_set", JoyVirtualSet_f);
	Cmd_Add("joy_virtual_dev", JoyVirtualDev_f);
	return true;
}

void VID_Shutdown(void)
{
	if (!window)
		return;
	INS_Shutdown();
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
#ifdef __EMSCRIPTEN__
	web_set_grab(grab);
#else
	SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
#endif
}

void VID_SetCaption(const char *text)
{
#ifdef __EMSCRIPTEN__
	web_set_title(text);
#else
	if (window)
		SDL_SetWindowTitle(window, text);
#endif
}

bool VID_SetClipboard(const char *text) { return SDL_SetClipboardText(text) == 0; }

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
#ifdef __EMSCRIPTEN__
	web_follow_canvas();
#endif
	while (SDL_PollEvent(&ev)) {
		if (pending_key && ev.type != SDL_TEXTINPUT) {
			IN_KeyEvent(pending_key, true, 0);
			pending_key = 0;
		}
		switch (ev.type) {
		case SDL_QUIT:
#ifdef __EMSCRIPTEN__
			/* the page closing: nothing to quit to, and the config is
			 * saved from the page's pagehide */
			break;
#else
			return false;
#endif
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
			int k = pad_button(ev.cbutton.button), slot = pad_for_id(ev.cbutton.which);
			/* while Steam Input has the controllers, SDL's copy of them is
			 * Steam's virtual gamepad (or the device itself) */
			if (!k || slot < 0 || (INS_Active() && ev.type == SDL_CONTROLLERBUTTONDOWN))
				break;
			if (ev.type == SDL_CONTROLLERBUTTONDOWN)
				Pad_Touched(slot);
			IN_DeviceKeyEvent(slot + 1, k, ev.type == SDL_CONTROLLERBUTTONDOWN, 0);
			break;
		}
		case SDL_CONTROLLERAXISMOTION: {
			int slot = pad_for_id(ev.caxis.which);
			if (slot < 0 || (INS_Active() && ev.caxis.value))
				break;
			if (abs(ev.caxis.value) > 16000)
				Pad_Touched(slot);
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
	INS_Frame();
	return true;
}
