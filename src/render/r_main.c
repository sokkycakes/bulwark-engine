/* Bulwark engine - renderer front end: start-up, frames, the client's model
 * list, and the drawing and scene builtins of the client game.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../client/console.h"
#include "../client/vid.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool r_active;
double r_time;
cvar_t *r_lightmapscale, *r_fullbright, *r_drawentities, *r_drawworld, *r_novis, *r_clearcolor;
cvar_t *gl_texturemode, *r_wireframe, *r_speeds;
static cvar_t *scr_scale, *r_screenshot_format;
static bool cursor_mode;
static char screenshot_pending[256];

bool R_Active(void) { return r_active; }

void R_GetSize(int *w, int *h)
{
	*w = r_vwidth;
	*h = r_vheight;
}

/* --- models of the client's precache list ----------------------------------- */

rmodel_t *R_ModelForIndex(int index)
{
	clmodel_t *cm;
	if (index <= 0 || index >= cl.nummodels)
		return NULL;
	cm = &cl.models[index];
	if (!cm->name)
		return NULL;
	if (cm->name[0] == '*') /* world submodels exist once the map is loaded */
		return R_LoadModel(cm->name);
	if (!cm->render)
		cm->render = R_LoadModel(cm->name);
	return cm->render;
}

void R_PrecacheModel(int index)
{
	if (index > 0 && index < cl.nummodels && cl.models[index].name && cl.models[index].name[0] != '*')
		cl.models[index].render = R_LoadModel(cl.models[index].name);
}

bool R_ModelBounds(int index, float *mins, float *maxs)
{
	rmodel_t *m = R_ModelForIndex(index);
	if (!m || !m->loaded)
		return false;
	memcpy(mins, m->mins, sizeof(float) * 3);
	memcpy(maxs, m->maxs, sizeof(float) * 3);
	return true;
}

void R_ClearMap(void)
{
	R_FreeWorld();
	R_FreeModels();
	for (int i = 0; i < cl.nummodels; i++)
		cl.models[i].render = NULL;
}

void R_NewMap(void)
{
	if (cl.world && cl.worldpath[0])
		R_LoadWorld(cl.world, cl.worldpath);
	for (int i = 1; i < cl.nummodels; i++)
		R_PrecacheModel(i);
}

/* --- frames ------------------------------------------------------------------ */

static void update_size(void)
{
	float scale;
	VID_GetSize(&r_pwidth, &r_pheight);
	if (r_pwidth < 1)
		r_pwidth = 1;
	if (r_pheight < 1)
		r_pheight = 1;
	/* the 2D layer works in virtual pixels: scale 0 picks a whole-number
	 * scale keeping at least 480 lines, as menus and HUDs expect */
	scale = scr_scale->value;
	if (scale <= 0)
		scale = BW_MAX(1.0f, floorf(r_pheight / 480.0f));
	r_2dscale = scale;
	r_vwidth = (int)(r_pwidth / scale);
	r_vheight = (int)(r_pheight / scale);
	if (!r_active) {
		r_vwidth = 640;
		r_vheight = 480;
	}
}

static void take_screenshot(const char *name);

void R_BeginFrame(void)
{
	r_time = cl.realtime;
	if (!r_active)
		return;
	update_size();
	qglViewport(0, 0, r_pwidth, r_pheight);
	qglDisable(GL_SCISSOR_TEST);
	{
		float c[3] = {0, 0, 0};
		sscanf(r_clearcolor->string, "%f %f %f", &c[0], &c[1], &c[2]);
		qglClearColor(c[0], c[1], c[2], 1);
	}
	GL_SetDepth(true, true);
	qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	R_Begin2D(r_vwidth, r_vheight);
	R_ClearScene();
}

void R_EndFrame(void)
{
	bool game = cl.state == ca_active && cl.csqc;
	if (!r_active)
		return;
	if (!game && !Con_IsActive() && !UI_WantsInput()) {
		/* nothing else on screen: show the console */
		static const float black[3] = {0, 0, 0};
		R_DrawFill(0, 0, (float)r_vwidth, (float)r_vheight, black, 1, 0);
		Con_SetActive(true);
	}
	R_DrawConsole(r_vwidth, r_vheight);
	R_End2D();
	VID_SetMouseGrab(game && !Con_IsActive() && !cursor_mode && !UI_WantsInput());
	if (screenshot_pending[0]) {
		take_screenshot(screenshot_pending);
		screenshot_pending[0] = 0;
	}
	VID_Swap();
}

/* --- screenshots ------------------------------------------------------------- */

#define STBIW_MALLOC(sz) Mem_Alloc(sz)
#define STBIW_REALLOC(p, sz) Mem_Realloc(p, sz)
#define STBIW_FREE(p) Mem_Free(p)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../thirdparty/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void write_func(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ctx); }

static void take_screenshot(const char *name)
{
	char rel[300], path[1024];
	uint8_t *px = Mem_Alloc((size_t)r_pwidth * r_pheight * 3);
	FILE *f;
	qglPixelStorei(GL_PACK_ALIGNMENT, 1);
	qglReadPixels(0, 0, r_pwidth, r_pheight, GL_RGB, GL_UNSIGNED_BYTE, px);
	if (name[0] == '*') {
		for (int i = 0; i < 10000; i++) {
			snprintf(rel, sizeof(rel), "screenshots/shot%04d.png", i);
			if (!FS_FileExists(rel))
				break;
		}
	} else {
		snprintf(rel, sizeof(rel), "screenshots/%s%s", name, strchr(name, '.') ? "" : ".png");
	}
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("screenshot: can't write %s\n", rel);
		Mem_Free(px);
		return;
	}
	stbi_flip_vertically_on_write(1);
	if (strstr(rel, ".jpg"))
		stbi_write_jpg_to_func(write_func, f, r_pwidth, r_pheight, 3, px, 90);
	else
		stbi_write_png_to_func(write_func, f, r_pwidth, r_pheight, 3, px, r_pwidth * 3);
	fclose(f);
	Con_Printf("wrote %s\n", rel);
	Mem_Free(px);
}

static void Screenshot_f(int argc, const char **argv)
{
	Str_Copy(screenshot_pending, argc > 1 ? argv[1] : "*", sizeof(screenshot_pending));
}

/* "sky <name>": a skybox from env/<name>_rt and friends, as sent by games */
static void Sky_f(int argc, const char **argv)
{
	char base[96];
	if (argc < 2) {
		Con_Printf("sky is \"%s\"\n", r_world.skyname);
		return;
	}
	Str_Copy(r_world.skyname, argv[1], sizeof(r_world.skyname));
	snprintf(base, sizeof(base), strchr(argv[1], '/') ? "%s" : "env/%s", argv[1]);
	r_world.skybox = r_active ? R_LoadCubemap(base) : NULL;
}

bool R_Init(bool headless)
{
	r_lightmapscale = Cvar_Get("r_lightmapscale", "2", CVAR_ARCHIVE, "Brightness multiplier for lightmaps (Q3 maps are stored at half brightness)");
	r_fullbright = Cvar_Get("r_fullbright", "0", 0, "Draw everything without lighting");
	r_drawentities = Cvar_Get("r_drawentities", "1", 0, "Draw models and brush entities");
	r_drawworld = Cvar_Get("r_drawworld", "1", 0, "Draw the world");
	r_novis = Cvar_Get("r_novis", "0", 0, "Ignore visibility data");
	r_clearcolor = Cvar_Get("r_clearcolor", "0 0 0", CVAR_ARCHIVE, "Colour behind everything");
	gl_texturemode = Cvar_Get("gl_texturemode", "linear", CVAR_ARCHIVE, "Texture filtering: linear or nearest");
	r_wireframe = Cvar_Get("r_wireframe", "0", 0, NULL);
	r_speeds = Cvar_Get("r_speeds", "0", 0, NULL);
	scr_scale = Cvar_Get("scr_scale", "0", CVAR_ARCHIVE, "2D scale: virtual pixels per screen pixel, 0 for automatic");
	r_screenshot_format = Cvar_Get("r_screenshot_format", "png", CVAR_ARCHIVE, NULL);
	Cmd_Add("screenshot", Screenshot_f);
	Cmd_Add("sky", Sky_f);
	R_InitScene();
	r_active = false;
	if (!headless && VID_Active()) {
		if (!GL_LoadFunctions(VID_GetProcAddress)) {
			Con_Printf("OpenGL 3.3 entry points missing, running without a renderer\n");
		} else {
			Con_Printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n", qglGetString(GL_VENDOR),
			           qglGetString(GL_RENDERER), qglGetString(GL_VERSION));
			if (R_InitPrograms())
				r_active = true;
		}
	}
	R_InitImages();
	R_InitMaterials();
	R_InitDraw(); /* after the images: it uploads the built-in font */
	update_size();
	return r_active;
}

void R_Shutdown(void)
{
	R_ClearMap();
	R_ShutdownMaterials();
	R_ShutdownDraw();
	R_ShutdownImages();
	if (r_active) {
		R_ShutdownPrograms();
		R_ShutdownStream();
	}
	r_active = false;
}

/* --- 2D builtins ------------------------------------------------------------- */

static int drawfont_ofs(qcvm_t *vm)
{
	static qcvm_t *last;
	static int ofs = -1;
	if (vm != last) {
		last = vm;
		ofs = QC_GlobalOfs(vm, "drawfont");
	}
	return ofs;
}

static void use_drawfont(qcvm_t *vm) { R_SetDrawFont((int)QC_GFloat(vm, drawfont_ofs(vm))); }

static int argflags(qcvm_t *vm, int n) { return vm->argc > n ? (int)QC_PFLOAT(vm, n) : 0; }

static void r_drawpic(qcvm_t *vm)
{
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 2);
	static const float white[3] = {1, 1, 1};
	const float *rgb = vm->argc > 3 ? QC_PVEC(vm, 3) : white;
	float alpha = vm->argc > 4 ? QC_PFLOAT(vm, 4) : 1;
	R_DrawPic(pos[0], pos[1], size[0], size[1], R_FindMaterial(QC_PSTR(vm, 1), MATF_2D), 0, 0, 1, 1, rgb, alpha,
	          argflags(vm, 5));
	QC_ReturnFloat(vm, 1);
}

static void r_drawsubpic(qcvm_t *vm)
{
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 1), *src = QC_PVEC(vm, 3), *srcsz = QC_PVEC(vm, 4);
	R_DrawPic(pos[0], pos[1], size[0], size[1], R_FindMaterial(QC_PSTR(vm, 2), MATF_2D), src[0], src[1],
	          src[0] + srcsz[0], src[1] + srcsz[1], QC_PVEC(vm, 5), QC_PFLOAT(vm, 6), argflags(vm, 7));
}

static void r_drawfill(qcvm_t *vm)
{
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 1);
	R_DrawFill(pos[0], pos[1], size[0], size[1], QC_PVEC(vm, 2), QC_PFLOAT(vm, 3), argflags(vm, 4));
	QC_ReturnFloat(vm, 1);
}

static void text_builtin(qcvm_t *vm, bool usemarkup)
{
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 2);
	static const float white[3] = {1, 1, 1};
	float sx = vm->argc > 2 ? size[0] : 8, sy = vm->argc > 2 ? size[1] : 8;
	const float *rgb = vm->argc > 3 ? QC_PVEC(vm, 3) : white;
	float alpha = vm->argc > 4 ? QC_PFLOAT(vm, 4) : 1;
	use_drawfont(vm);
	QC_ReturnFloat(vm, R_DrawString(pos[0], pos[1], QC_PSTR(vm, 1), sx, sy, rgb, alpha, argflags(vm, 5), usemarkup));
}

static void r_drawstring(qcvm_t *vm) { text_builtin(vm, true); }
static void r_drawrawstring(qcvm_t *vm) { text_builtin(vm, false); }

static void r_drawcharacter(qcvm_t *vm)
{
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 2);
	int c = (int)QC_PFLOAT(vm, 1);
	char s[8] = {0};
	if (c < 0x80)
		s[0] = (char)c;
	else if (c < 0x800)
		s[0] = (char)(0xc0 | (c >> 6)), s[1] = (char)(0x80 | (c & 0x3f));
	else
		s[0] = (char)(0xe0 | (c >> 12)), s[1] = (char)(0x80 | ((c >> 6) & 0x3f)), s[2] = (char)(0x80 | (c & 0x3f));
	use_drawfont(vm);
	R_DrawString(pos[0], pos[1], s, size[0], size[1], QC_PVEC(vm, 3), QC_PFLOAT(vm, 4), argflags(vm, 5), false);
	QC_ReturnFloat(vm, 1);
}

static void r_stringwidth(qcvm_t *vm)
{
	float sx = 8, sy = 8;
	if (vm->argc > 2) {
		sx = QC_PVEC(vm, 2)[0];
		sy = QC_PVEC(vm, 2)[1];
		if (sy <= 0)
			sy = sx;
	}
	use_drawfont(vm);
	QC_ReturnFloat(vm, R_StringWidth(QC_PSTR(vm, 0), sx, sy, QC_PFLOAT(vm, 1) != 0));
}

static void r_drawtextfield(qcvm_t *vm)
{
	/* word wrapped text in a box; alignment bits are right, top, left, bottom */
	float *pos = QC_PVEC(vm, 0), *size = QC_PVEC(vm, 1);
	int align = (int)QC_PFLOAT(vm, 2);
	const char *text = QC_PSTR(vm, 3);
	static const float white[3] = {1, 1, 1};
	float lineh = 8, y = pos[1];
	int lines = 0;
	char line[1024];
	size_t n = 0;
	const char *p = text, *lastspace = NULL;
	(void)align;
	use_drawfont(vm);
	while (*p || n) {
		bool flush = !*p || *p == '\n';
		if (!flush) {
			line[n] = *p;
			line[n + 1] = 0;
			if (*p == ' ')
				lastspace = p;
			if (R_StringWidth(line, lineh, lineh, true) > size[0] && lastspace) {
				/* break at the last space */
				n -= (size_t)(p - lastspace);
				p = lastspace;
				flush = true;
			} else if (n < sizeof(line) - 2) {
				n++;
			}
		}
		if (flush) {
			line[n] = 0;
			if (y + lineh <= pos[1] + size[1] + 0.5f)
				R_DrawString(pos[0], y, line, lineh, lineh, white, 1, 0, true);
			y += lineh;
			lines++;
			n = 0;
			lastspace = NULL;
			if (!*p)
				break;
		}
		p++;
	}
	QC_ReturnFloat(vm, (float)lines);
}

static void r_drawsetcliparea(qcvm_t *vm)
{
	R_SetClip(QC_PFLOAT(vm, 0), QC_PFLOAT(vm, 1), QC_PFLOAT(vm, 2), QC_PFLOAT(vm, 3));
}

static void r_drawresetcliparea(qcvm_t *vm) { R_ResetClip(); }

static void r_drawgetimagesize(qcvm_t *vm)
{
	material_t *m = R_FindMaterial(QC_PSTR(vm, 0), MATF_2D);
	float v[3] = {0, 0, 0};
	if (m && !(m->diffuse && m->diffuse->missing && !m->fromfile)) {
		v[0] = (float)m->width;
		v[1] = (float)m->height;
	}
	QC_ReturnVector(vm, v);
}

static void r_precache_pic(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	material_t *m = R_FindMaterial(name, MATF_2D);
	int flags = argflags(vm, 1);
	/* PRECACHE_PIC_TEST: an empty result says it failed */
	if ((flags & 512) && m->diffuse->missing && !m->fromfile)
		QC_ReturnString(vm, "");
	else
		QC_ReturnString(vm, name);
}

static void r_iscachedpic(qcvm_t *vm)
{
	material_t *m = R_FindMaterial(QC_PSTR(vm, 0), MATF_2D);
	QC_ReturnFloat(vm, m && (m->fromfile || !m->diffuse->missing) ? 1 : 0);
}

static void r_freepic(qcvm_t *vm) { (void)vm; }

static void r_loadfont(qcvm_t *vm)
{
	QC_ReturnFloat(vm, (float)R_LoadFont(QC_PSTR(vm, 0), QC_PSTR(vm, 1), QC_PSTR(vm, 2),
	                                     vm->argc > 3 ? (int)QC_PFLOAT(vm, 3) : -1));
}

static void r_findfont(qcvm_t *vm) { QC_ReturnFloat(vm, (float)R_FindFont(QC_PSTR(vm, 0))); }

static void r_drawline(qcvm_t *vm)
{
	float w = QC_PFLOAT(vm, 0), *a = QC_PVEC(vm, 1), *b = QC_PVEC(vm, 2);
	static const float white[3] = {1, 1, 1};
	float dx = b[0] - a[0], dy = b[1] - a[1], len = sqrtf(dx * dx + dy * dy);
	float nx, ny, xy[8];
	static const float st[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	const float *rgb = vm->argc > 3 ? QC_PVEC(vm, 3) : white;
	float alpha = vm->argc > 4 ? QC_PFLOAT(vm, 4) : 1;
	float rgba[3] = {rgb[0], rgb[1], rgb[2]};
	if (len <= 0)
		return;
	if (w < 1)
		w = 1;
	nx = -dy / len * w * 0.5f;
	ny = dx / len * w * 0.5f;
	xy[0] = a[0] + nx, xy[1] = a[1] + ny;
	xy[2] = b[0] + nx, xy[3] = b[1] + ny;
	xy[4] = b[0] - nx, xy[5] = b[1] - ny;
	xy[6] = a[0] - nx, xy[7] = a[1] - ny;
	R_BeginPolygon("$white", argflags(vm, 5), true);
	for (int i = 0; i < 4; i++) {
		float p[3] = {xy[i * 2], xy[i * 2 + 1], 0}, tc[3] = {st[i * 2], st[i * 2 + 1], 0};
		R_PolygonVertex(p, tc, rgba, alpha);
	}
	R_EndPolygon();
}

static void rotpic(qcvm_t *vm, const float *pivot, const float *mins, const float *maxs, const char *pic,
                   const float *tc0, const float *tcsz, const float *rgb, float alpha, float angle)
{
	float s = sinf(angle * (float)M_PI / 180), c = cosf(angle * (float)M_PI / 180);
	float corners[4][2] = {{mins[0], mins[1]}, {maxs[0], mins[1]}, {maxs[0], maxs[1]}, {mins[0], maxs[1]}};
	float st[4][2] = {{tc0[0], tc0[1]}, {tc0[0] + tcsz[0], tc0[1]}, {tc0[0] + tcsz[0], tc0[1] + tcsz[1]}, {tc0[0], tc0[1] + tcsz[1]}};
	R_BeginPolygon(pic, 0, true);
	for (int i = 0; i < 4; i++) {
		float p[3] = {pivot[0] + corners[i][0] * c - corners[i][1] * s, pivot[1] + corners[i][0] * s + corners[i][1] * c, 0};
		float t[3] = {st[i][0], st[i][1], 0};
		R_PolygonVertex(p, t, rgb, alpha);
	}
	R_EndPolygon();
	(void)vm;
}

static void r_drawrotpic(qcvm_t *vm)
{
	static const float tc0[2] = {0, 0}, tcsz[2] = {1, 1};
	rotpic(vm, QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PSTR(vm, 3), tc0, tcsz, QC_PVEC(vm, 4),
	       QC_PFLOAT(vm, 5), QC_PFLOAT(vm, 6));
}

static void r_drawrotsubpic(qcvm_t *vm)
{
	float *aa = QC_PVEC(vm, 7);
	rotpic(vm, QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PSTR(vm, 3), QC_PVEC(vm, 4), QC_PVEC(vm, 5),
	       QC_PVEC(vm, 6), aa[0], aa[1]);
}

static void r_getresolution(qcvm_t *vm)
{
	static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 900}, {1920, 1080}, {2560, 1440}};
	int m = (int)QC_PFLOAT(vm, 0);
	float v[3] = {0, 0, 0};
	if (m == -1) {
		v[0] = (float)r_pwidth;
		v[1] = (float)r_pheight;
	} else if (m >= 0 && m < (int)BW_ARRAY_COUNT(modes)) {
		v[0] = (float)modes[m][0];
		v[1] = (float)modes[m][1];
	}
	QC_ReturnVector(vm, v);
}

/* --- scene builtins ------------------------------------------------------------ */

enum {
	VF_MIN = 1, VF_MIN_X, VF_MIN_Y, VF_SIZE, VF_SIZE_X, VF_SIZE_Y, VF_VIEWPORT, VF_FOV, VF_FOV_X, VF_FOV_Y,
	VF_ORIGIN, VF_ORIGIN_X, VF_ORIGIN_Y, VF_ORIGIN_Z, VF_ANGLES, VF_ANGLES_X, VF_ANGLES_Y, VF_ANGLES_Z,
	VF_DRAWWORLD, VF_DRAWENGINESBAR, VF_DRAWCROSSHAIR, VF_MINDIST = 23, VF_MAXDIST = 24,
	VF_CL_VIEWANGLES = 33, VF_CL_VIEWANGLES_X, VF_CL_VIEWANGLES_Y, VF_CL_VIEWANGLES_Z,
	VF_PERSPECTIVE = 200, VF_ACTIVESEAT = 202, VF_AFOV = 203, VF_SCREENVSIZE = 204, VF_SCREENPSIZE = 205,
	VF_VIEWENTITY = 206
};

static void r_clearscene(qcvm_t *vm) { R_ClearScene(); }

static void r_addentities(qcvm_t *vm) { R_AddEntities(vm, (int)QC_PFLOAT(vm, 0)); }

static void r_addentity(qcvm_t *vm) { R_AddEntityFromQC(vm, QC_PENT(vm, 0)); }

static void r_removeentity(qcvm_t *vm)
{
	int ent = QC_PENT(vm, 0), out = 0;
	for (int i = 0; i < r_scene.numents; i++)
		if (r_scene.ents[i].qcent != ent || r_scene.ents[i].vm != vm)
			r_scene.ents[out++] = r_scene.ents[i];
	r_scene.numents = out;
}

static void r_setproperty(qcvm_t *vm)
{
	int prop = (int)QC_PFLOAT(vm, 0);
	float *v = QC_PVEC(vm, 1), f = v[0];
	switch (prop) {
	case VF_MIN: r_scene.vx = v[0], r_scene.vy = v[1]; break;
	case VF_MIN_X: r_scene.vx = f; break;
	case VF_MIN_Y: r_scene.vy = f; break;
	case VF_SIZE: r_scene.vw = v[0], r_scene.vh = v[1]; break;
	case VF_SIZE_X: r_scene.vw = f; break;
	case VF_SIZE_Y: r_scene.vh = f; break;
	case VF_VIEWPORT: {
		float *s = QC_PVEC(vm, 2);
		r_scene.vx = v[0], r_scene.vy = v[1], r_scene.vw = s[0], r_scene.vh = s[1];
		break;
	}
	case VF_FOV: r_scene.fov_x = v[0], r_scene.fov_y = v[1]; break;
	case VF_FOV_X: r_scene.fov_x = f; break;
	case VF_FOV_Y: r_scene.fov_y = f; break;
	case VF_AFOV: r_scene.afov = f, r_scene.fov_x = r_scene.fov_y = 0; break;
	case VF_ORIGIN: memcpy(r_scene.origin, v, sizeof(float) * 3); break;
	case VF_ORIGIN_X: case VF_ORIGIN_Y: case VF_ORIGIN_Z: r_scene.origin[prop - VF_ORIGIN_X] = f; break;
	case VF_ANGLES: memcpy(r_scene.angles, v, sizeof(float) * 3); break;
	case VF_ANGLES_X: case VF_ANGLES_Y: case VF_ANGLES_Z: r_scene.angles[prop - VF_ANGLES_X] = f; break;
	case VF_DRAWWORLD: r_scene.drawworld = f != 0; break;
	case VF_MINDIST: r_scene.nearclip = f; break;
	case VF_MAXDIST: r_scene.farclip = f; break;
	case VF_CL_VIEWANGLES: memcpy(cl.viewangles, v, sizeof(float) * 3); break;
	case VF_CL_VIEWANGLES_X: case VF_CL_VIEWANGLES_Y: case VF_CL_VIEWANGLES_Z:
		cl.viewangles[prop - VF_CL_VIEWANGLES_X] = f;
		break;
	case VF_PERSPECTIVE: r_scene.perspective = f != 0; break;
	case VF_VIEWENTITY: r_scene.viewentity = (int)f; break;
	default: break;
	}
	QC_ReturnFloat(vm, 1);
}

static void r_getproperty(qcvm_t *vm)
{
	int prop = (int)QC_PFLOAT(vm, 0);
	float v[3] = {0, 0, 0};
	switch (prop) {
	case VF_MIN: v[0] = r_scene.vx, v[1] = r_scene.vy; break;
	case VF_MIN_X: v[0] = r_scene.vx; break;
	case VF_MIN_Y: v[0] = r_scene.vy; break;
	case VF_SIZE: v[0] = r_scene.vw, v[1] = r_scene.vh; break;
	case VF_SIZE_X: v[0] = r_scene.vw; break;
	case VF_SIZE_Y: v[0] = r_scene.vh; break;
	case VF_FOV: case VF_FOV_X: case VF_FOV_Y: {
		float fx = r_scene.fov_x, fy = r_scene.fov_y;
		if (fx <= 0) {
			float aspect = r_scene.vh > 0 ? r_scene.vw / r_scene.vh : 4.0f / 3;
			float ty = tanf((r_scene.afov > 0 ? r_scene.afov : 90) * (float)M_PI / 360.0f) * 0.75f;
			fy = atanf(ty) * 360.0f / (float)M_PI;
			fx = atanf(ty * aspect) * 360.0f / (float)M_PI;
		}
		if (prop == VF_FOV)
			v[0] = fx, v[1] = fy;
		else
			v[0] = prop == VF_FOV_X ? fx : fy;
		break;
	}
	case VF_AFOV: v[0] = r_scene.afov; break;
	case VF_ORIGIN: memcpy(v, r_scene.origin, sizeof(v)); break;
	case VF_ORIGIN_X: case VF_ORIGIN_Y: case VF_ORIGIN_Z: v[0] = r_scene.origin[prop - VF_ORIGIN_X]; break;
	case VF_ANGLES: memcpy(v, r_scene.angles, sizeof(v)); break;
	case VF_ANGLES_X: case VF_ANGLES_Y: case VF_ANGLES_Z: v[0] = r_scene.angles[prop - VF_ANGLES_X]; break;
	case VF_DRAWWORLD: v[0] = r_scene.drawworld; break;
	case VF_MINDIST: v[0] = r_scene.nearclip; break;
	case VF_MAXDIST: v[0] = r_scene.farclip; break;
	case VF_CL_VIEWANGLES: memcpy(v, cl.viewangles, sizeof(v)); break;
	case VF_CL_VIEWANGLES_X: case VF_CL_VIEWANGLES_Y: case VF_CL_VIEWANGLES_Z:
		v[0] = cl.viewangles[prop - VF_CL_VIEWANGLES_X];
		break;
	case VF_PERSPECTIVE: v[0] = r_scene.perspective; break;
	case VF_ACTIVESEAT: v[0] = 0; break;
	case VF_SCREENVSIZE: v[0] = (float)r_vwidth, v[1] = (float)r_vheight; break;
	case VF_SCREENPSIZE: v[0] = (float)r_pwidth, v[1] = (float)r_pheight; break;
	case VF_VIEWENTITY: v[0] = (float)r_scene.viewentity; break;
	default: break;
	}
	QC_ReturnVector(vm, v);
}

static void r_renderscene(qcvm_t *vm) { R_RenderScene(); }

static void r_dynamiclight_add(qcvm_t *vm)
{
	QC_ReturnFloat(vm, (float)R_AddDynamicLight(QC_PVEC(vm, 0), QC_PFLOAT(vm, 1), QC_PVEC(vm, 2)));
}

static void r_beginpolygon(qcvm_t *vm)
{
	R_BeginPolygon(QC_PSTR(vm, 0), argflags(vm, 1), vm->argc > 2 && QC_PFLOAT(vm, 2) != 0);
}

static void r_polygonvertex(qcvm_t *vm)
{
	R_PolygonVertex(QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PFLOAT(vm, 3));
}

static void r_endpolygon(qcvm_t *vm) { R_EndPolygon(); }

static void r_project(qcvm_t *vm)
{
	float out[3];
	R_Project(QC_PVEC(vm, 0), out);
	QC_ReturnVector(vm, out);
}

static void r_unproject(qcvm_t *vm)
{
	float out[3];
	R_Unproject(QC_PVEC(vm, 0), out);
	QC_ReturnVector(vm, out);
}

static void r_shaderforname(qcvm_t *vm) { QC_ReturnFloat(vm, (float)R_MaterialHandle(R_FindMaterial(QC_PSTR(vm, 0), 0))); }

static void r_getlight(qcvm_t *vm)
{
	float amb[3], dif[3], dir[3], v[3];
	R_LightPoint(QC_PVEC(vm, 0), amb, dif, dir);
	for (int k = 0; k < 3; k++)
		v[k] = (amb[k] + dif[k]) * 0.5f;
	QC_ReturnVector(vm, v);
}

static rmodel_t *model_arg(qcvm_t *vm, int n) { return R_ModelForIndex((int)QC_PFLOAT(vm, n)); }

static void r_modelframecount(qcvm_t *vm) { QC_ReturnFloat(vm, (float)R_ModelNumFrames(model_arg(vm, 0))); }
static void r_frameforname(qcvm_t *vm) { QC_ReturnFloat(vm, (float)R_FrameForName(model_arg(vm, 0), QC_PSTR(vm, 1))); }
static void r_frametoname(qcvm_t *vm) { QC_ReturnString(vm, R_FrameToName(model_arg(vm, 0), (int)QC_PFLOAT(vm, 1))); }
static void r_frameduration(qcvm_t *vm) { QC_ReturnFloat(vm, R_FrameDuration(model_arg(vm, 0), (int)QC_PFLOAT(vm, 1))); }
static void r_minus1(qcvm_t *vm) { QC_ReturnFloat(vm, -1); }
static void r_zero(qcvm_t *vm) { QC_ReturnInt(vm, 0); }
static void r_nop(qcvm_t *vm) { (void)vm; }

static void r_gettaginfo(qcvm_t *vm)
{
	/* no tags yet: the entity's own origin */
	int e = QC_PENT(vm, 0);
	float *o = vm->fofs.origin >= 0 ? &QC_EdictFields(vm, e)[vm->fofs.origin].f : NULL;
	float zero[3] = {0, 0, 0};
	QC_ReturnVector(vm, o ? o : zero);
}

static void r_setcursormode(qcvm_t *vm) { cursor_mode = QC_PFLOAT(vm, 0) != 0; }
static void r_getcursormode(qcvm_t *vm) { QC_ReturnFloat(vm, cursor_mode ? 1 : 0); }

static void r_getmousepos(qcvm_t *vm)
{
	float v[3] = {in_mouse_x / r_2dscale, in_mouse_y / r_2dscale, 0};
	QC_ReturnVector(vm, v);
}

static void r_setwindowcaption(qcvm_t *vm) { VID_SetCaption(QC_PSTR(vm, 0)); }

static void r_spriteframe(qcvm_t *vm) { QC_ReturnString(vm, QC_PSTR(vm, 0)); }

const qc_builtin_t r_builtins[] = {
	{"drawpic", r_drawpic}, {"drawsubpic", r_drawsubpic}, {"drawfill", r_drawfill}, {"drawstring", r_drawstring},
	{"drawrawstring", r_drawrawstring}, {"drawcharacter", r_drawcharacter}, {"stringwidth", r_stringwidth},
	{"drawtextfield", r_drawtextfield}, {"drawsetcliparea", r_drawsetcliparea},
	{"drawresetcliparea", r_drawresetcliparea}, {"drawgetimagesize", r_drawgetimagesize},
	{"precache_pic", r_precache_pic}, {"iscachedpic", r_iscachedpic}, {"freepic", r_freepic},
	{"loadfont", r_loadfont}, {"findfont", r_findfont}, {"drawline", r_drawline}, {"drawrotpic", r_drawrotpic},
	{"drawrotsubpic", r_drawrotsubpic}, {"getresolution", r_getresolution},
	{"clearscene", r_clearscene}, {"addentities", r_addentities}, {"addentity", r_addentity},
	{"addentity_lighting", r_addentity}, {"removeentity", r_removeentity}, {"setproperty", r_setproperty},
	{"getproperty", r_getproperty}, {"renderscene", r_renderscene}, {"dynamiclight_add", r_dynamiclight_add},
	{"R_BeginPolygon", r_beginpolygon}, {"R_PolygonVertex", r_polygonvertex}, {"R_EndPolygon", r_endpolygon},
	{"project", r_project}, {"unproject", r_unproject}, {"shaderforname", r_shaderforname},
	{"getlight", r_getlight}, {"modelframecount", r_modelframecount}, {"frameforname", r_frameforname},
	{"frametoname", r_frametoname}, {"frameduration", r_frameduration}, {"frameforaction", r_minus1},
	{"gettagindex", r_zero}, {"gettaginfo", r_gettaginfo}, {"setcursormode", r_setcursormode},
	{"getcursormode", r_getcursormode}, {"getmousepos", r_getmousepos}, {"setwindowcaption", r_setwindowcaption},
	{"spriteframe", r_spriteframe},
	/* not done yet: accepted so games carry on */
	{"adddecal", r_nop}, {"remapshader", r_nop}, {"setcustomskin", r_nop}, {"loadcustomskin", r_zero},
	{"applycustomskin", r_nop}, {"releasecustomskin", r_nop}, {"dynamiclight_get", r_zero},
	{"dynamiclight_set", r_nop}, {"dynamiclight_spawnstatic", r_zero}, {"pointparticles", r_nop},
	{"trailparticles", r_nop}, {"boxparticles", r_nop}, {"particleeffectquery", r_zero},
	{"getmodeleventidx", r_zero}, {"getnextmodelevent", r_zero}, {"processmodelevents", r_nop},
	{"skel_create", r_zero}, {"skel_build", r_zero}, {"skel_delete", r_nop}, {"skel_get_numbones", r_zero},
	{"skel_get_bonename", r_zero}, {"skel_get_boneparent", r_zero}, {"skel_find_bone", r_zero},
	{"skel_get_bonerel", r_zero}, {"skel_get_boneabs", r_zero}, {"skel_set_bone", r_nop},
	{"skel_premul_bone", r_nop}, {"skel_premul_bones", r_nop}, {"skel_postmul_bone", r_nop},
	{"skel_copybones", r_nop}, {"skel_ragupdate", r_zero}, {"skel_mmap", r_zero}, {"skel_set_bone_world", r_nop},
	{"skinforname", r_zero}, {"cin_open", r_zero}, {"cin_close", r_nop}, {"cin_setstate", r_nop},
	{"cin_getstate", r_zero}, {"cin_restart", r_nop}, {"clipboard_set", r_nop}, {"setmousepos", r_nop},
	{"r_uploadimage", r_nop}, {"r_readimage", r_zero}, {"getsurfacenumpoints", r_zero},
	{"getsurfacenearpoint", r_minus1}, {"getsurfacetexture", r_zero},
	{NULL, NULL}};
