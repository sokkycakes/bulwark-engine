/* Bulwark engine - what the engine itself draws over the game: screen
 * tints and fades, the chat input line, notify lines, centre prints the game
 * doesn't take, and the fps, position and network readouts.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../client/client.h"
#include "../client/console.h"

#include <math.h>
#include <stdio.h>

static const float white[3] = {1, 1, 1};

/* frame times for the fps readout and the net graph */
#define HISTORY 128
static float frametimes[HISTORY];
static float netrtt[HISTORY];
static int histpos;
static double lastframe;
static float lastorigin[3];
static double lastorigin_time;
static float speed;

static void draw_screen_tints(int w, int h)
{
	float dt = (float)cl.frametime;
	if (cl_screen.cshift[3] > 0)
		R_DrawFill(0, 0, (float)w, (float)h, cl_screen.cshift, cl_screen.cshift[3], 0);
	if (cl_screen.flash[3] > 0) {
		R_DrawFill(0, 0, (float)w, (float)h, cl_screen.flash, cl_screen.flash[3], 0);
		cl_screen.flash[3] -= cl_screen.flash_fade * dt;
	}
	if (cl_screen.fading) {
		float f = (float)((cl.realtime - cl_screen.fade_start) / cl_screen.fade_time);
		float a = cl_screen.fade_out ? BW_MIN(f, 1) : 1 - BW_MIN(f, 1);
		if (a > 0)
			R_DrawFill(0, 0, (float)w, (float)h, cl_screen.fade_color, a, 0);
		/* a finished fade-in is over; a fade-out holds until fadein or v_cshift */
		if (f >= 1 && !cl_screen.fade_out)
			cl_screen.fading = false;
	}
}

static void draw_notify(int w, int h, float size)
{
	int shown = 0, max = BW_CLAMP(cl_screen.notifylines->ivalue, 0, 32);
	float y = 4;
	const char *lines[32];
	if (!cl_screen.drawnotify->ivalue || Con_IsActive())
		return;
	/* the newest lines still within the notify time, drawn oldest first */
	for (int back = 1; back < CON_LINES - 1 && shown < max; back++) {
		const char *l = Con_Line(back);
		if (!l)
			break;
		if (host_time - Con_LineTime(back) > cl_screen.notifytime->value)
			break;
		if (*l)
			lines[shown++] = l;
	}
	for (int i = shown - 1; i >= 0; i--, y += size)
		R_DrawString(4, y, lines[i], size, size, white, 1, 0, true);
}

static void draw_chat(int w, int h, float size)
{
	char line[CON_INPUT + 32];
	int mode = Con_ChatMode();
	if (!mode)
		return;
	snprintf(line, sizeof(line), "%s %s%s", mode == 2 ? "say_team:" : "say:", Con_ChatInput(),
	         ((int)(cl.realtime * 3)) & 1 ? "_" : "");
	R_DrawString(8, (float)h * 0.6f, line, size, size, white, 1, 0, false);
}

static void draw_centerprint(int w, int h, float size)
{
	const char *p = cl_screen.centerprint;
	float y;
	int lines = 1;
	if (!*p || cl.realtime - cl_screen.centerprint_time > 3)
		return;
	for (const char *q = p; *q; q++)
		lines += *q == '\n';
	y = h * 0.35f - lines * size * 0.5f;
	while (*p) {
		char line[256];
		size_t n = strcspn(p, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)BW_MIN(n, sizeof(line) - 1), p);
		R_DrawString((w - R_StringWidth(line, size, size, true)) * 0.5f, y, line, size, size, white, 1, 0, true);
		y += size;
		p += n;
		if (*p == '\n')
			p++;
	}
}

static void draw_readouts(int w, int h, float size)
{
	char text[256];
	float y = 4;
	int fpsmode = BW_MAX(cl_screen.showfps->ivalue, (int)Cvar_Value("show_fps"));
	if (fpsmode) {
		float avg = 0, worst = 0;
		for (int i = 0; i < HISTORY; i++) {
			avg += frametimes[i];
			worst = BW_MAX(worst, frametimes[i]);
		}
		avg /= HISTORY;
		if (fpsmode >= 2)
			snprintf(text, sizeof(text), "%4.0f fps  %5.2f ms avg  %5.2f ms worst", avg > 0 ? 1 / avg : 0, avg * 1000,
			         worst * 1000);
		else
			snprintf(text, sizeof(text), "%4.0f fps", avg > 0 ? 1 / avg : 0);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, y, text, size, size, white, 1, 0, false);
		y += size;
	}
	if (cl_screen.showpos->ivalue) {
		const float *o = r_scene.mainorigin;
		snprintf(text, sizeof(text), "pos: %.2f %.2f %.2f", o[0], o[1], o[2]);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, y, text, size, size, white, 1, 0, false);
		y += size;
		snprintf(text, sizeof(text), "ang: %.2f %.2f %.2f", cl.viewangles[0], cl.viewangles[1], cl.viewangles[2]);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, y, text, size, size, white, 1, 0, false);
		y += size;
		snprintf(text, sizeof(text), "vel: %.2f", speed);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, y, text, size, size, white, 1, 0, false);
	}
	if (cl_screen.netgraph->ivalue && cl.state >= ca_loading) {
		float gy = h * 0.7f;
		snprintf(text, sizeof(text), "ping %d ms  loss %d%%  in %.1f k/s  out %.1f k/s", (int)(cl.chan.rtt * 1000),
		         (int)(cl.chan.loss * 100), cl.chan.bytes_in_sec / 1024, cl.chan.bytes_out_sec / 1024);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, gy, text, size, size, white, 1, 0, false);
		gy += size;
		snprintf(text, sizeof(text), "tick %.0f  updaterate %s  cmdrate %s  dropped %u", cl.tickrate,
		         Cvar_String("cl_updaterate"), Cvar_String("cl_cmdrate"), cl.chan.dropped);
		R_DrawString(w - R_StringWidth(text, size, size, false) - 4, gy, text, size, size, white, 1, 0, false);
		if (cl_screen.netgraph->ivalue >= 2) {
			/* one bar per frame: frame time in green, latency in yellow */
			static const float green[3] = {0.2f, 1, 0.2f}, yellow[3] = {1, 0.9f, 0.2f};
			float base = h * 0.7f - 4.0f, x0 = w - HISTORY * 2.0f - 4;
			for (int i = 0; i < HISTORY; i++) {
				int k = (histpos + i) % HISTORY;
				float ft = BW_MIN(frametimes[k] * 1000, 60), rt = BW_MIN(netrtt[k] * 1000 / 4, 60);
				R_DrawFill(x0 + i * 2, base - ft - size * 3, 2, ft, green, 0.8f, 0);
				R_DrawFill(x0 + i * 2, base - rt, 2, rt, yellow, 0.8f, 0);
			}
		}
	}
}

void R_DrawOverlays(int w, int h)
{
	float size = 8;
	double now = Sys_Seconds();
	if (lastframe > 0) {
		frametimes[histpos] = (float)(now - lastframe);
		netrtt[histpos] = cl.chan.rtt;
		histpos = (histpos + 1) % HISTORY;
	}
	lastframe = now;
	if (cl.realtime - lastorigin_time >= 0.1) {
		float d[3] = {r_scene.mainorigin[0] - lastorigin[0], r_scene.mainorigin[1] - lastorigin[1], 0};
		if (lastorigin_time > 0)
			speed = VectorLength(d) / (float)(cl.realtime - lastorigin_time);
		VectorCopy(r_scene.mainorigin, lastorigin);
		lastorigin_time = cl.realtime;
	}
	draw_screen_tints(w, h);
	draw_notify(w, h, size);
	draw_centerprint(w, h, size * 2);
	draw_chat(w, h, size);
	draw_readouts(w, h, size);
}
