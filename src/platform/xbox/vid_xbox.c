/* Bulwark engine - Xbox screen and controllers. ISC licence, see LICENSE.
 *
 * The game draws at 640x480 through gl_xbox.c. Controllers come through
 * nxdk's SDL2 (its USB stack), as game controllers, so the engine's pad
 * bindings apply (cl_input.c pad_defaults):
 *   left stick   move               right stick  look
 *   A B X Y      A B X Y            START        menu      BACK  view
 *   right trigger attack            left trigger  secondary attack
 *   white / black  left / right shoulder          D-pad      D-pad
 * BACK + START together toggles the console. */
#include <SDL.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#include "../../client/client.h"
#include "../../client/console.h"
#include "../../client/keys.h"
#include "../../client/vid.h"
#include "../../common/cmd.h"
#include "../../common/common.h"
#include "../../common/cvar.h"
#include "xbox.h"

#define MAX_PADS 4

static bool active;
static bool grabbed;
static float mousex, mousey;

static struct {
	SDL_GameController *gc;
	SDL_JoystickID id;
	bool ltrig, rtrig;
	bool back, start;
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
	memset(&pads[slot], 0, sizeof(pads[slot]));
	pads[slot].gc = gc;
	pads[slot].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
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
		if (!*held && v > 0.5f)
			*held = true, IN_KeyEvent(key, true, 0);
		else if (*held && v < 0.4f)
			*held = false, IN_KeyEvent(key, false, 0);
	}
	IN_JoyAxis(slot, axis, v);
}

static void meminfo_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	XBX_PrintMemory("now");
}

/* xbox_irqtime [seconds]: spins reading the time stamp counter, and counts
 * every gap in it as time the CPU spent elsewhere: interrupts, their DPCs,
 * and other threads. That measures what interrupts cost the game without a
 * profiler, on a console or in an emulator. The game stops meanwhile. */
static void irqtime_f(int argc, const char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 1;
	static const unsigned edges[] = {2, 5, 10, 20, 50, 100, 1000}; /* microseconds */
	unsigned counts[8] = {0};
	double stolen[8] = {0};
	secs = BW_CLAMP(secs, 0.1, 10);
	LARGE_INTEGER pf;
	QueryPerformanceFrequency(&pf);
	LARGE_INTEGER p0, p1;
	QueryPerformanceCounter(&p0);
	uint64_t c0 = __rdtsc(), last = c0;
	/* calibrate the counter's rate against the performance counter first */
	for (;;) {
		QueryPerformanceCounter(&p1);
		if (p1.QuadPart - p0.QuadPart > pf.QuadPart / 20)
			break;
	}
	double tsc_hz = (double)(__rdtsc() - c0) * (double)pf.QuadPart / (double)(p1.QuadPart - p0.QuadPart);
	uint64_t gap_min = (uint64_t)(tsc_hz * 2e-6), end;
	c0 = last = __rdtsc();
	end = c0 + (uint64_t)(tsc_hz * secs);
	DWORD tick0 = GetTickCount(), vbl0 = XBGL_VblankCount();
	while (last < end) {
		uint64_t now = __rdtsc();
		uint64_t d = now - last;
		last = now;
		if (d >= gap_min) {
			double us = (double)d * 1e6 / tsc_hz;
			int b = 0;
			while (b < 7 && us >= edges[b])
				b++;
			counts[b]++, stolen[b] += us;
		}
	}
	DWORD ticks = GetTickCount() - tick0, vbls = XBGL_VblankCount() - vbl0;
	double total = 0;
	unsigned n = 0;
	for (int b = 0; b < 8; b++)
		total += stolen[b], n += counts[b];
	Con_Printf("irqtime: %.2f s at %.0f MHz: %u gaps took %.1f%% (%.1f us each); kernel ticks %lu, vblanks %lu\n", secs,
	           tsc_hz / 1e6, n, total / (secs * 1e4), n ? total / n : 0, (unsigned long)ticks, (unsigned long)vbls);
	for (int b = 1; b < 8; b++)
		if (counts[b])
			Con_Printf("  %4u-%-4u us: %6u gaps, %.1f%%\n", edges[b - 1], b < 7 ? edges[b] : 99999, counts[b],
			           stolen[b] / (secs * 1e4));
}

bool VID_Init(int width, int height, bool fullscreen)
{
	(void)width, (void)height, (void)fullscreen;
	XBGL_RegisterCvars();
	if (!XBGL_Init())
		return false;
	if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
		Con_Printf("SDL controllers: %s\n", SDL_GetError());
	active = true;
	Cmd_AddD("xbox_meminfo", meminfo_f, "xbox_meminfo: print how much of the Xbox's memory is free");
	Cmd_AddD("xbox_irqtime", irqtime_f, "xbox_irqtime [seconds]: how much of the CPU's time interrupts take");
	return true;
}

void VID_Shutdown(void)
{
	if (active)
		XBGL_Shutdown();
	active = false;
}

bool VID_Active(void) { return active; }
void VID_ApplyMode(void) {}

void VID_GetSize(int *w, int *h)
{
	*w = XBGL_Width();
	*h = XBGL_Height();
}

void VID_Swap(void) { XBGL_EndFrame(); }
void *VID_GetProcAddress(const char *name) { return XBGL_GetProc(name); }
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

void VID_Rumble(int dev, float low, float high, float seconds)
{
	if (dev >= 0 && dev < MAX_PADS && pads[dev].gc)
		SDL_GameControllerRumble(pads[dev].gc, (Uint16)(BW_CLAMP(low, 0, 1) * 65535), (Uint16)(BW_CLAMP(high, 0, 1) * 65535),
		                         (Uint32)(BW_MAX(seconds, 0) * 1000));
}

void VID_RumbleTriggers(int dev, float left, float right, float seconds) { (void)dev, (void)left, (void)right, (void)seconds; }
void VID_SetControllerLED(int dev, const float *rgb) { (void)dev, (void)rgb; }

bool VID_PumpEvents(void)
{
	SDL_Event ev;
	int slot, key;
	{
		/* gl_xbox_stats: how often the main loop comes round, even when no frame reaches the screen */
		static double since;
		static int loops;
		static cvar_t *stats;
		if (!stats)
			stats = Cvar_Find("gl_xbox_stats");
		loops++;
		/* Sys_Seconds, not GetTickCount, which lags when clock interrupts are late (see frame_stats) */
		double now = Sys_Seconds();
		if (stats && stats->ivalue && now - since > 5) {
			Con_Printf("xbox: main loop %d times in %.1f s\n", loops, now - since);
			since = now;
			loops = 0;
		}
	}
	/* one pump a frame: SDL_PollEvent pumps on every call, and each pump
	 * polls the USB root hubs and re-reads every pad, so a moving stick's
	 * several events a frame would do that several times */
	SDL_PumpEvents();
	while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
		switch (ev.type) {
		case SDL_CONTROLLERDEVICEADDED: pad_open(ev.cdevice.which); break;
		case SDL_CONTROLLERDEVICEREMOVED: pad_close(ev.cdevice.which); break;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			if ((slot = pad_for_id(ev.cbutton.which)) < 0 || !(key = pad_button(ev.cbutton.button)))
				break;
			if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
				pads[slot].back = ev.type == SDL_CONTROLLERBUTTONDOWN;
			if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)
				pads[slot].start = ev.type == SDL_CONTROLLERBUTTONDOWN;
			if (ev.type == SDL_CONTROLLERBUTTONDOWN && pads[slot].back && pads[slot].start) {
				Cbuf_AddText("toggleconsole\n");
				break;
			}
			IN_KeyEvent(key, ev.type == SDL_CONTROLLERBUTTONDOWN, 0);
			break;
		case SDL_CONTROLLERAXISMOTION:
			if ((slot = pad_for_id(ev.caxis.which)) >= 0)
				pad_axis(slot, ev.caxis.axis, ev.caxis.value);
			break;
		default: break;
		}
	}
	return true;
}
