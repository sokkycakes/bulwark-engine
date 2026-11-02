/* Bulwark engine - the model viewer and the map viewer.
 *
 * modelviewer <model> shows one model on its own, turning; the arrow keys
 * turn and tilt it, the mouse wheel or +/- zoom, PgUp/PgDn step through its
 * frames, [ and ] its skins, space pauses the animation and Escape closes.
 *
 * mapedit <map> drops any game and server, loads the map's geometry on the
 * client alone with no gamecode, and flies a free camera through it
 * (mouse to look, WASD to move, space and C up and down, shift faster).
 * Entities are drawn where they stand, brush entities with their models,
 * and the one in front of the camera has its keys listed. Escape leaves.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../client/keys.h"
#include "../client/console.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VIEW_NONE, VIEW_MODEL, VIEW_MAP };

#define MAX_VENTS 4096

typedef struct {
	float origin[3];
	int submodel; /* brush entities: their model, else 0 */
	char classname[64];
	int firstkv, numkv;
} vent_t;

static int mode;
static char modelname[128];
static float yaw = 30, pitch = 10, dist, spin = 1;
static int frame, skin;
static bool paused;
static double animtime;
static float seqtime; /* time into the animation shown, for models with named ones */

static float cam[3], camang[3];
static bspmap_t *mapworld; /* the world mapedit loaded, while it is still cl.world */
static bool held[K_MAX];
static vent_t *vents;
static int numvents;
static char (*kvs)[2][128];
static int numkvs;

bool V_Active(void) { return mode != VIEW_NONE; }

static void parse_entities(const char *s)
{
	char tok[256];
	Mem_Free(vents);
	Mem_Free(kvs);
	vents = Mem_Alloc(sizeof(vent_t) * MAX_VENTS);
	kvs = NULL;
	numvents = numkvs = 0;
	while (s && (s = COM_Parse(s, tok, sizeof(tok))) && !strcmp(tok, "{") && numvents < MAX_VENTS) {
		vent_t *v = &vents[numvents];
		memset(v, 0, sizeof(*v));
		v->firstkv = numkvs;
		for (;;) {
			char key[128];
			if (!(s = COM_Parse(s, tok, sizeof(tok))) || !strcmp(tok, "}"))
				break;
			Str_Copy(key, tok, sizeof(key));
			if (!(s = COM_Parse(s, tok, sizeof(tok))))
				break;
			if (!(numkvs & 255))
				kvs = Mem_Realloc(kvs, sizeof(*kvs) * (numkvs + 256));
			Str_Copy(kvs[numkvs][0], key, sizeof(kvs[0][0]));
			Str_Copy(kvs[numkvs][1], tok, sizeof(kvs[0][1]));
			numkvs++;
			if (!strcmp(key, "origin"))
				sscanf(tok, "%f %f %f", &v->origin[0], &v->origin[1], &v->origin[2]);
			else if (!strcmp(key, "classname"))
				Str_Copy(v->classname, tok, sizeof(v->classname));
			else if (!strcmp(key, "model") && tok[0] == '*')
				v->submodel = atoi(tok + 1);
		}
		v->numkv = numkvs - v->firstkv;
		numvents++;
	}
}

void V_Close(void)
{
	if (mode == VIEW_MAP) {
		if (cl.world == mapworld && cl.state == ca_disconnected) {
			R_ClearMap();
			BSP_Free(cl.world);
			cl.world = NULL;
			cl.worldpath[0] = 0;
		}
		mapworld = NULL;
		Mem_Free(vents);
		Mem_Free(kvs);
		vents = NULL;
		kvs = NULL;
		numvents = numkvs = 0;
	}
	mode = VIEW_NONE;
	memset(held, 0, sizeof(held));
}

static void ModelViewer_f(int argc, const char **argv)
{
	rmodel_t *m;
	if (argc < 2) {
		Con_Printf("modelviewer <MODELNAME> [SKINFILE] [ANIMATIONFILE]\n");
		return;
	}
	if (!R_Active()) {
		Con_Printf("modelviewer: there is no window to draw in\n");
		return;
	}
	m = R_LoadModel(argv[1]);
	if (!m || m->failed || !m->loaded) {
		Con_Printf("modelviewer: couldn't load %s\n", argv[1]);
		return;
	}
	if (argc > 2)
		Con_Printf("modelviewer: %s: separate skin files aren't supported, showing the model's own skins\n", argv[2]);
	if (argc > 3)
		Con_Printf("modelviewer: %s: separate animation files aren't supported, showing the model's own frames\n",
		           argv[3]);
	if (mode == VIEW_MAP)
		V_Close();
	Str_Copy(modelname, argv[1], sizeof(modelname));
	mode = VIEW_MODEL;
	frame = skin = 0;
	seqtime = 0;
	paused = false;
	animtime = cl.realtime;
	yaw = 30;
	pitch = 10;
	dist = BW_MAX(16.0f, m->radius * 2.2f);
	Con_SetActive(false);
}

static void MapEdit_f(int argc, const char **argv)
{
	char path[160];
	bspmap_t *b;
	if (argc < 2) {
		Con_Printf("%s <map>: loads the named map without any gamecode active\n", argv[0]);
		return;
	}
	if (!R_Active()) {
		Con_Printf("%s: there is no window to draw in\n", argv[0]);
		return;
	}
	Terrain_PrepareMap(argv[1]);
	snprintf(path, sizeof(path), "maps/%s.bsp", argv[1]);
	if (!FS_FileExists(path)) {
		Con_Printf("%s: no such map\n", path);
		return;
	}
	/* leave whatever game is running: nothing else may own the world */
	if (Cmd_Exists("killserver"))
		Cmd_ExecuteString("killserver");
	CL_Disconnect(NULL);
	if (mode != VIEW_NONE)
		V_Close();
	if (!(b = BSP_Load(path)))
		return;
	cl.world = mapworld = b;
	Str_Copy(cl.worldpath, path, sizeof(cl.worldpath));
	Str_Copy(cl.mapname, argv[1], sizeof(cl.mapname));
	R_NewMap();
	parse_entities(b->entities);
	mode = VIEW_MAP;
	VectorClear(cam);
	VectorClear(camang);
	for (int i = 0; i < numvents; i++)
		if (!Str_NICmp(vents[i].classname, "info_player", 11)) {
			VectorCopy(vents[i].origin, cam);
			cam[2] += 48;
			for (int k = vents[i].firstkv; k < vents[i].firstkv + vents[i].numkv; k++)
				if (!strcmp(kvs[k][0], "angle"))
					camang[1] = (float)atof(kvs[k][1]);
			break;
		}
	Con_SetActive(false);
	Con_Printf("mapedit %s: %d entities. Fly with WASD, space and C; Escape leaves.\n", argv[1], numvents);
}

/* --- input ----------------------------------------------------------------- */

bool V_KeyEvent(int key, bool down)
{
	rmodel_t *m;
	if (!mode || key < 0 || key >= K_MAX)
		return false;
	held[key] = down;
	if (!down)
		return true;
	if (key == K_ESCAPE) {
		V_Close();
		return true;
	}
	if (mode == VIEW_MAP)
		return true;
	m = R_LoadModel(modelname);
	switch (key) {
	case K_LEFTARROW: yaw -= 15; spin = 0; break;
	case K_RIGHTARROW: yaw += 15; spin = 0; break;
	case K_UPARROW: pitch = BW_MIN(pitch + 10, 89.0f); break;
	case K_DOWNARROW: pitch = BW_MAX(pitch - 10, -89.0f); break;
	case K_MWHEELUP: case '=': case '+': dist = BW_MAX(4.0f, dist * 0.85f); break;
	case K_MWHEELDOWN: case '-': dist *= 1.18f; break;
	case K_PGUP: frame++; seqtime = 0; paused = !R_FrameToName(R_LoadModel(modelname), 0); break;
	case K_PGDN: frame--; seqtime = 0; paused = !R_FrameToName(R_LoadModel(modelname), 0); break;
	case ']': skin++; break;
	case '[': skin = BW_MAX(0, skin - 1); break;
	case K_SPACE: paused = !paused; animtime = cl.realtime; break;
	case K_ENTER: spin = spin ? 0 : 1; break;
	}
	if (m && m->loaded) {
		int n = BW_MAX(1, R_ModelNumFrames(m));
		frame = ((frame % n) + n) % n;
	}
	return true;
}

bool V_MouseMove(float dx, float dy)
{
	if (mode == VIEW_MODEL) {
		yaw += dx * 0.3f;
		pitch = BW_MAX(-89.0f, BW_MIN(89.0f, pitch - dy * 0.3f));
		return true;
	}
	if (mode == VIEW_MAP) {
		camang[1] -= dx * 0.022f * 3;
		camang[0] = BW_MAX(-89.0f, BW_MIN(89.0f, camang[0] + dy * 0.022f * 3));
		return true;
	}
	return false;
}

/* --- drawing ----------------------------------------------------------------- */

static float ts = 10; /* text size for this frame */

static const float white[3] = {1, 1, 1}, yellow[3] = {1, 0.85f, 0.3f}, grey[3] = {0.6f, 0.6f, 0.6f};

static void draw_model(int w, int h)
{
	rmodel_t *m = R_LoadModel(modelname);
	rentity_t *e;
	float center[3], f[3], r[3], u[3], ang[3];
	int n;
	char line[256];
	if (!m || !m->loaded) {
		V_Close();
		return;
	}
	n = BW_MAX(1, R_ModelNumFrames(m));
	if (R_FrameToName(m, 0)) {
		/* named animations (sequences): play the one picked */
		if (!paused)
			seqtime += (float)cl.frametime;
	} else if (!paused && cl.realtime - animtime >= 0.1) {
		frame = (frame + 1) % n;
		animtime = cl.realtime;
	}
	yaw += spin * (float)cl.frametime * 20;
	R_ClearScene();
	r_scene.drawworld = false;
	r_scene.viewentity = -1;
	for (int k = 0; k < 3; k++)
		center[k] = (m->mins[k] + m->maxs[k]) * 0.5f;
	ang[0] = pitch;
	ang[1] = yaw + 180;
	ang[2] = 0;
	AngleVectors(ang, f, r, u);
	for (int k = 0; k < 3; k++)
		r_scene.origin[k] = center[k] - f[k] * dist;
	VectorCopy(ang, r_scene.angles);
	e = R_AllocEntity();
	if (e) {
		float a[3] = {0, 0, 0};
		e->model = m;
		e->frame = e->frame2 = frame;
		e->frame1time = e->frame2time = seqtime;
		e->skin = skin;
		AnglesToAxis(a, e->axis);
	}
	{
		/* a light over the viewer's shoulder */
		float lo[3], lc[3] = {1, 1, 1};
		for (int k = 0; k < 3; k++)
			lo[k] = r_scene.origin[k] + u[k] * dist * 0.5f;
		R_AddDynamicLight(lo, dist * 3, lc);
	}
	R_RenderScene();
	snprintf(line, sizeof(line), "%s  frame %d/%d %s  skin %d", m->name, frame, n, R_FrameToName(m, frame) ? R_FrameToName(m, frame) : "",
	         skin);
	R_Text(8, 8, line, ts, white, 1);
	snprintf(line, sizeof(line), "size %.0f x %.0f x %.0f, radius %.0f", m->maxs[0] - m->mins[0], m->maxs[1] - m->mins[1],
	         m->maxs[2] - m->mins[2], m->radius);
	R_Text(8, 8 + ts * 1.4f, line, ts, grey, 1);
	R_Text(8, (float)h - ts * 1.6f,
	       "arrows/mouse turn  wheel or +/- zoom  PgUp/PgDn frame  [ ] skin  space pause  enter spin  esc close", ts, grey,
	       1);
	(void)w;
}

static void draw_map(int w, int h)
{
	float f[3], r[3], u[3], speed;
	int best = -1;
	float bestd = 0.97f;
	char line[300];
	AngleVectors(camang, f, r, u);
	speed = (held[K_LSHIFT] ? 1200.0f : 400.0f) * (float)cl.frametime;
	for (int k = 0; k < 3; k++) {
		if (held['w'])
			cam[k] += f[k] * speed;
		if (held['s'])
			cam[k] -= f[k] * speed;
		if (held['d'])
			cam[k] += r[k] * speed;
		if (held['a'])
			cam[k] -= r[k] * speed;
	}
	if (held[K_SPACE])
		cam[2] += speed;
	if (held['c'])
		cam[2] -= speed;

	R_ClearScene();
	r_scene.viewentity = -1;
	VectorCopy(cam, r_scene.origin);
	VectorCopy(camang, r_scene.angles);
	for (int i = 0; i < numvents; i++) {
		rentity_t *e;
		if (vents[i].submodel <= 0 || vents[i].submodel >= r_world.numsubmodels)
			continue;
		if (!(e = R_AllocEntity()))
			break;
		float a[3] = {0, 0, 0};
		e->model = &r_world.submodels[vents[i].submodel];
		VectorCopy(vents[i].origin, e->origin);
		AnglesToAxis(a, e->axis);
	}
	R_RenderScene();

	/* markers and names of point entities in view, and the one looked at */
	for (int i = 0; i < numvents; i++) {
		float d[3], p[3], len, dot;
		if (!vents[i].classname[0] || !strcmp(vents[i].classname, "worldspawn"))
			continue;
		for (int k = 0; k < 3; k++)
			d[k] = vents[i].origin[k] - cam[k];
		len = VectorLength(d);
		if (len < 1 || len > 2048)
			continue;
		dot = DotProduct(d, f) / len;
		if (dot <= 0.2f)
			continue;
		if (dot > bestd) {
			bestd = dot;
			best = i;
		}
		R_Project(vents[i].origin, p);
		if (p[2] <= 0)
			continue;
		R_Rect(p[0] - 2, p[1] - 2, 4, 4, vents[i].submodel ? grey : yellow, 0.9f);
		if (len < 768)
			R_Text(p[0] + 4, p[1] - ts / 2, vents[i].classname, ts, white, 0.8f);
	}
	snprintf(line, sizeof(line), "mapedit %s  %.0f %.0f %.0f  (%d entities, no gamecode)", cl.mapname, cam[0], cam[1], cam[2],
	         numvents);
	R_Text(8, 8, line, ts, white, 1);
	if (best >= 0) {
		float y = 8 + ts * 2;
		for (int k = vents[best].firstkv; k < vents[best].firstkv + vents[best].numkv && y < h - ts * 3; k++, y += ts * 1.25f) {
			snprintf(line, sizeof(line), "%s: %s", kvs[k][0], kvs[k][1]);
			R_Text(8, y, line, ts, k == vents[best].firstkv ? yellow : grey, 1);
		}
	}
	R_Text(8, (float)h - ts * 1.6f, "mouse look  WASD move  space/C up/down  shift faster  esc leave", ts, grey, 1);
	(void)w;
}

void V_Draw(int w, int h)
{
	ts = BW_MAX(10.0f, floorf(h / 56.0f));
	/* a connection took the world over */
	if (mode == VIEW_MAP && (cl.world != mapworld || cl.state != ca_disconnected)) {
		mapworld = NULL;
		V_Close();
	}
	if (mode == VIEW_MODEL)
		draw_model(w, h);
	else if (mode == VIEW_MAP)
		draw_map(w, h);
}

/* viewerframe <number|name> [time]: picks the animation the model viewer
 * shows (and how far into it), for scripted checks and long lists */
static void ViewerFrame_f(int argc, const char **argv)
{
	rmodel_t *m = mode == VIEW_MODEL ? R_LoadModel(modelname) : NULL;
	int n;
	if (!m || argc < 2) {
		Con_Printf("viewerframe <number|name> [seconds]: in the model viewer, shows that animation\n");
		return;
	}
	n = BW_MAX(1, R_ModelNumFrames(m));
	frame = -1;
	for (int i = 0; i < n; i++)
		if (R_FrameToName(m, i) && !Str_ICmp(R_FrameToName(m, i), argv[1]))
			frame = i;
	if (frame < 0)
		frame = ((atoi(argv[1]) % n) + n) % n;
	seqtime = argc > 2 ? (float)atof(argv[2]) : 0;
	paused = argc > 2;
	spin = argc > 2 ? 0 : spin;
}

void V_Init(void)
{
	Cmd_AddD("viewerframe", ViewerFrame_f, "In the model viewer, shows an animation: viewerframe <number|name> [seconds] (with seconds, holds that moment)");
	Cmd_AddD("modelviewer", ModelViewer_f, "Shows a model on its own: modelviewer <MODELNAME> [SKINFILE] [ANIMATIONFILE]");
	Cmd_AddD("mapedit", MapEdit_f, "Loads the named map without any gamecode active.");
}
