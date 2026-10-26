/* Bulwark engine - renderer tools from FTE's console: listings of images,
 * materials, models and batches, shader dumps and remaps, pictures pinned
 * to the screen (showpic), water and skyroom fog, sky groups, fonts, leak
 * pointfiles, the waypoint graph, and screenshots that draw the main view
 * again off screen: bigger than the window, side by side for stereo,
 * as cubemap faces, or wrapped all the way round (360 and VR).
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../common/bsptool.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STBI_WRITE_NO_STDIO
#include "../thirdparty/stb_image_write.h"

/* server/sv_route.c: the waypoint graph, for route_visualise on a listen server */
int SV_RouteNumNodes(void);
bool SV_RouteNode(int i, float *origin, float *radius, int *numlinks);
int SV_RouteLink(int i, int k);

static const char *blend_name(int b)
{
	static const char *const n[] = {"opaque", "alpha", "add", "filter", "custom"};
	return b >= 0 && b < (int)BW_ARRAY_COUNT(n) ? n[b] : "?";
}

/* --- listings -------------------------------------------------------------------- */

typedef struct {
	const char *filter;
	int count;
	size_t bytes;
} listctx_t;

static bool want(const listctx_t *l, const char *name)
{
	return !l->filter || Str_WildMatch(l->filter, name, true) || Str_WildMatch(va("*%s*", l->filter), name, true);
}

static void list_image(image_t *img, void *ctx)
{
	listctx_t *l = ctx;
	size_t bytes = (size_t)img->width * img->height * 4 * (img->target == GL_TEXTURE_CUBE_MAP ? 6 : 1);
	if (!want(l, img->name))
		return;
	Con_Printf("%s%-48s %4dx%-4d %s%s\n", img->missing ? "^1" : "", img->name, img->width, img->height,
	           img->target == GL_TEXTURE_CUBE_MAP ? "cube " : "", img->alpha ? "alpha" : "");
	l->count++;
	l->bytes += img->missing ? 0 : bytes;
}

static void Imagelist_f(int argc, const char **argv)
{
	listctx_t l = {argc > 1 ? argv[1] : NULL, 0, 0};
	R_ForEachImage(list_image, &l);
	Con_Printf("%d images, about %.1f MB (missing ones in red)\n", l.count, l.bytes / 1048576.0);
}

static void list_wad(const char *wad, const char *name, int type, int size, void *ctx)
{
	listctx_t *l = ctx;
	if (!want(l, name))
		return;
	Con_Printf("%-8s %-16s type %c %7d bytes%s\n", wad, name, type, size, R_ImageExists(name) ? "" : " (not loaded)");
	l->count++;
}

static void ImagelistWad_f(int argc, const char **argv)
{
	listctx_t l = {argc > 1 ? argv[1] : NULL, 0, 0};
	if (!R_NumWads()) {
		Con_Printf("no WAD files are open (the map names them in worldspawn's \"wad\" key)\n");
		return;
	}
	R_ForEachWadTexture(list_wad, &l);
	Con_Printf("%d WAD images\n", l.count);
}

static void list_material(material_t *m, void *ctx)
{
	listctx_t *l = ctx;
	if (!want(l, m->name))
		return;
	Con_Printf("%s%-48s %-6s %d stage%s%s%s\n", m->fromfile ? "^2" : "", m->name, blend_name(m->blend), m->numstages,
	           m->numstages == 1 ? "" : "s", m->sky ? " sky" : "", m->nodraw ? " nodraw" : "");
	l->count++;
}

static void Shaderlist_f(int argc, const char **argv)
{
	listctx_t l = {argc > 1 ? argv[1] : NULL, 0, 0};
	R_ForEachMaterial(list_material, &l);
	Con_Printf("%d materials (green ones come from scripts)\n", l.count);
}

static void Imageformats_f(int argc, const char **argv)
{
	GLint maxtex = 0, maxrb = 0;
	Con_Printf("Image files read: tga png jpg bmp psd gif hdr pic pnm lmp, WAD2/WAD3 mips, BSP-embedded mips\n");
	Con_Printf("Image files written: png jpg tga bmp\n");
	if (!R_Active()) {
		Con_Printf("no renderer running\n");
		return;
	}
	qglGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
	qglGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
	Con_Printf("OpenGL %s, %s\n", (const char *)qglGetString(GL_VERSION), (const char *)qglGetString(GL_RENDERER));
	Con_Printf("Texture formats used: RGBA8 (2D and cube), RGB8 render targets; largest texture %d, render target %d\n",
	           maxtex, maxrb);
}

/* which materials the world uses, and in how many surfaces */
static void Texturelist_f(int argc, const char **argv)
{
	material_t **mats;
	int *counts, n = 0;
	if (!r_world.loaded) {
		Con_Printf("no map loaded\n");
		return;
	}
	mats = Mem_Alloc(sizeof(*mats) * (size_t)(r_world.numsurfaces + 1));
	counts = Mem_Alloc(sizeof(int) * (size_t)(r_world.numsurfaces + 1));
	for (int i = 0; i < r_world.numsurfaces; i++) {
		material_t *m = r_world.surfaces[i].material;
		int k;
		for (k = 0; k < n && mats[k] != m; k++)
			;
		if (k == n)
			mats[n++] = m;
		counts[k]++;
	}
	Con_Printf("%s\n", r_world.name);
	for (int k = 0; k < n; k++)
		if (mats[k] && (argc < 2 || Str_WildMatch(va("*%s*", argv[1]), mats[k]->name, true)))
			Con_Printf("  %s%s%s (%d surfaces)\n", mats[k]->diffuse && mats[k]->diffuse->missing ? "^1" : "", mats[k]->name,
			           mats[k]->diffuse && mats[k]->diffuse->missing ? "^7" : "", counts[k]);
	Con_Printf("%d materials\n", n);
	Mem_Free(mats);
	Mem_Free(counts);
}

static void Memlist_f(int argc, const char **argv)
{
	size_t total = 0;
	for (int i = 0; i < r_world.numsubmodels; i++) {
		int v, t;
		size_t b = R_ModelMemory(&r_world.submodels[i], &v, &t);
		if (i == 0)
			b += (size_t)r_world.numlightmaps * r_world.lightmapsize * r_world.lightmapsize * 4;
		Con_Printf("%s%s: %zu bytes (%d triangles)\n", i ? va("*%d:", i) : "", r_world.name, b, t);
		total += b;
	}
	for (int i = 0; i < R_NumModels(); i++) {
		rmodel_t *m = R_ModelAt(i);
		int v, t;
		size_t b = R_ModelMemory(m, &v, &t);
		Con_Printf("%s: %zu bytes%s\n", m->name, b, m->failed ? " (failed to load)" : va(" (%d vertices, %d triangles, %d frames)", v, t, m->numframes));
		total += b;
	}
	Con_Printf("about %.1f MB in models\n", total / 1048576.0);
}

/* the world's draw batches: surfaces by material and lightmap page */
static void Batchlist_f(int argc, const char **argv)
{
	int batches = 0;
	if (!r_world.loaded || !r_world.numsubmodels) {
		Con_Printf("no map loaded\n");
		return;
	}
	Con_Printf("%s:\n", r_world.name);
	for (int i = 0; i < r_world.numsurfaces;) {
		const rsurface_t *s = &r_world.surfaces[i];
		int n = 0, tris = 0;
		while (i < r_world.numsurfaces && r_world.surfaces[i].material == s->material &&
		       r_world.surfaces[i].lightmap == s->lightmap) {
			tris += r_world.surfaces[i].numindexes / 3;
			n++;
			i++;
		}
		Con_Printf("  %s lm=%d surfs=%d tris=%d%s\n", s->material ? s->material->name : "?", s->lightmap, n, tris,
		           s->material && s->material->sky ? " sky" : "");
		batches++;
	}
	Con_Printf("%d batches for %d surfaces\n", batches, r_world.numsurfaces);
}

static void Modelformats_f(int argc, const char **argv)
{
	static const char *const formats[] = {
		"Quake1 BSP (bsp29)", "Quake1 BSP2 (bsp2, 2psb)", "Half-Life BSP (bsp30)", "Quake3 BSP (ibsp46)",
		"Raven BSP (rbsp)", "QFusion BSP (fbsp)", "BSPX extension lumps", "Quake1 Model (mdl)",
		"Inter-Quake Model (iqm)", "Quake1 Sprite (spr)",
	};
	for (size_t i = 0; i < BW_ARRAY_COUNT(formats); i++)
		Con_Printf("%s\n", formats[i]);
}

/* r_dumpshaders: the GLSL programs as glsl/<name>.glsl */
static void Dumpshaders_f(int argc, const char **argv)
{
	const char *name, *vs, *fs;
	for (int p = 0; R_ProgramSource(p, &name, &vs, &fs); p++) {
		char rel[64], path[1024];
		FILE *f;
		snprintf(rel, sizeof(rel), "glsl/%s.glsl", name);
		if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
			Con_Printf("can't write %s\n", rel);
			continue;
		}
		fprintf(f, "%s// vertex program\n#ifdef VERTEX_SHADER\n%s#endif\n// fragment program\n#ifdef FRAGMENT_SHADER\n%s#endif\n",
		        R_GLSLHeader(), vs, fs);
		fclose(f);
		Con_Printf("Writing %s\n", rel);
	}
}

/* the world surface the main view looks at */
static const rsurface_t *surface_in_view(void)
{
	bsptrace_t tr;
	float end[3], zero[3] = {0};
	int s;
	if (!r_world.loaded || !cl.world || !r_scene.havemain)
		return NULL;
	for (int k = 0; k < 3; k++)
		end[k] = r_scene.mainorigin[k] + r_scene.mainforward[k] * 8192;
	BSP_TraceModel(cl.world, 0, zero, r_scene.mainorigin, zero, zero, end, -1, &tr);
	if (tr.fraction >= 1)
		return NULL;
	s = R_SurfaceNearPoint(&r_world.submodels[0], tr.endpos);
	return s >= 0 ? R_ModelSurface(&r_world.submodels[0], s) : NULL;
}

static void Showshader_f(int argc, const char **argv)
{
	material_t *m = NULL;
	if (argc > 1) {
		m = R_FindMaterial(argv[1], 0);
		if (!m->fromfile && !R_MaterialFileExists(argv[1]) && m->diffuse && m->diffuse->missing) {
			Con_Printf("Shader \"%s\" is not loaded\n", argv[1]);
			return;
		}
	} else {
		const rsurface_t *s = surface_in_view();
		if (!s) {
			Con_Printf("Shader \"\" is not loaded\n");
			return;
		}
		m = s->material;
	}
	Con_Printf("%s%s\n{\n", m->name, m->fromfile ? " (from a script)" : " (made from its picture)");
	Con_Printf("\tprogram %d, blend %s%s%s%s%s\n", m->program, blend_name(m->blend), m->alphatest ? ", alphatest" : "",
	           m->twosided ? ", twosided" : "", m->sky ? ", sky" : "", m->nodraw ? ", nodraw" : "");
	if (m->diffuse)
		Con_Printf("\tdiffuse %s%s\n", m->diffuse->name, m->diffuse->missing ? " (missing)" : "");
	if (m->normal)
		Con_Printf("\tnormalmap %s\n", m->normal->name);
	if (m->fullbright)
		Con_Printf("\tfullbright %s\n", m->fullbright->name);
	if (m->specular)
		Con_Printf("\tspecular %s\n", m->specular->name);
	for (int i = 0; i < m->numstages; i++) {
		const mstage_t *st = &m->stages[i];
		Con_Printf("\t{\n\t\tmap %s\n\t\tblend %s%s%s\n\t}\n", st->lightmap ? "$lightmap" : st->image ? st->image->name : "?",
		           blend_name(st->blend), st->alphatest ? ", alphatest" : "", st->vertexcolor ? ", vertex colours" : "");
	}
	Con_Printf("}\n");
}

/* r_remapshader <from> <to>: draws one material as another. Like FTE, it is
 * for game code (run at cmd_gamecodelevel) or with cheats on. */
typedef struct {
	material_t *m;
	material_t orig;
} remap_t;
static remap_t *remaps;
static int numremaps;

static void Remapshader_f(int argc, const char **argv)
{
	material_t *from, *to;
	remap_t *r = NULL;
	if (!Cvar_Value("sv_cheats") && Cmd_ExecLevel() != (int)Cvar_Value("cmd_gamecodelevel")) {
		Con_Printf("r_remapshader may only be used from gamecode, or when cheats are enabled\n");
		return;
	}
	if (argc < 3) {
		Con_Printf("r_remapshader <from> <to>: draw one material as another (the same name twice undoes it)\n");
		return;
	}
	from = R_FindMaterial(argv[1], 0);
	for (int i = 0; i < numremaps; i++)
		if (remaps[i].m == from)
			r = &remaps[i];
	if (!r) {
		remaps = Mem_Realloc(remaps, sizeof(remap_t) * (size_t)(numremaps + 1));
		r = &remaps[numremaps++];
		r->m = from;
		r->orig = *from;
	}
	to = !Str_ICmp(argv[1], argv[2]) ? &r->orig : R_FindMaterial(argv[2], 0);
	{
		/* the contents, keeping the name and the hash chain */
		char name[sizeof(from->name)];
		material_t *next = from->next;
		memcpy(name, from->name, sizeof(name));
		*from = *to;
		memcpy(from->name, name, sizeof(name));
		from->next = next;
	}
}

/* mod_usetexture <material>: puts a material on the world surface in view
 * (until the map is loaded again), for trying textures out */
static void Usetexture_f(int argc, const char **argv)
{
	const rsurface_t *s;
	if (argc < 2) {
		s = surface_in_view();
		Con_Printf("mod_usetexture <material>: the surface in view %s\n", s ? va("uses %s", s->material->name) : "isn't a world surface");
		return;
	}
	if (!(s = surface_in_view())) {
		Con_Printf("no world surface in view\n");
		return;
	}
	((rsurface_t *)s)->material = R_FindMaterial(argv[1], 0);
}

/* --- showpic ------------------------------------------------------------------------ */

typedef struct {
	char image[128], place[64], zone[8], command[256];
	float x, y, w, h;
} showpic_t;
static showpic_t showpics[32];
static int numshowpics;

static void pic_rect(const showpic_t *p, int sw, int sh, float *x, float *y, float *w, float *h)
{
	const char *z = p->zone;
	int iw = 0, ih = 0;
	R_ImageSize(R_ImageHandle(p->image), &iw, &ih);
	*w = p->w > 0 ? p->w : iw;
	*h = p->h > 0 ? p->h : ih;
	/* zones: t/b/l/r/c combinations, as tl or mid */
	*x = strchr(z, 'r') ? sw - *w - p->x : strchr(z, 'l') ? p->x : (sw - *w) / 2 + p->x;
	*y = strchr(z, 'b') ? sh - *h - p->y : strchr(z, 't') ? p->y : (sh - *h) / 2 + p->y;
}

void R_DrawToolsOverlay(int w, int h)
{
	static const float white[3] = {1, 1, 1};
	for (int i = 0; i < numshowpics; i++) {
		float x, y, pw, ph;
		pic_rect(&showpics[i], w, h, &x, &y, &pw, &ph);
		R_DrawPic(x, y, pw, ph, R_FindMaterial(showpics[i].image, MATF_2D), 0, 0, 1, 1, white, 1, 0);
	}
}

/* a tap or click on a picture with a command runs it; true if one did */
bool R_ShowpicTouch(float x, float y)
{
	for (int i = numshowpics - 1; i >= 0; i--) {
		float px, py, pw, ph;
		pic_rect(&showpics[i], r_vwidth, r_vheight, &px, &py, &pw, &ph);
		if (showpics[i].command[0] && x >= px && y >= py && x < px + pw && y < py + ph) {
			Cbuf_AddText(va("%s\n", showpics[i].command));
			return true;
		}
	}
	return false;
}

static void Showpic_f(int argc, const char **argv)
{
	showpic_t *p = NULL;
	if (argc < 6) {
		Con_Printf("showpic <imagename> <placename> <x> <y> <zone> [width] [height] [touchcommand]\n");
		for (int i = 0; i < numshowpics; i++)
			Con_Printf("  %s: %s at %g %g %s\n", showpics[i].place, showpics[i].image, showpics[i].x, showpics[i].y, showpics[i].zone);
		return;
	}
	for (int i = 0; i < numshowpics; i++)
		if (!strcmp(showpics[i].place, argv[2]))
			p = &showpics[i];
	if (!argv[1][0] || !strcmp(argv[1], "-")) {
		if (p)
			*p = showpics[--numshowpics];
		return;
	}
	if (!p) {
		if (numshowpics == (int)BW_ARRAY_COUNT(showpics)) {
			Con_Printf("too many pictures\n");
			return;
		}
		p = &showpics[numshowpics++];
	}
	memset(p, 0, sizeof(*p));
	Str_Copy(p->image, argv[1], sizeof(p->image));
	Str_Copy(p->place, argv[2], sizeof(p->place));
	p->x = (float)atof(argv[3]);
	p->y = (float)atof(argv[4]);
	Str_Copy(p->zone, argv[5], sizeof(p->zone));
	p->w = argc > 6 ? (float)atof(argv[6]) : 0;
	p->h = argc > 7 ? (float)atof(argv[7]) : 0;
	Str_Copy(p->command, argc > 8 ? argv[8] : "", sizeof(p->command));
}

static void ShowpicRemoveall_f(int argc, const char **argv) { numshowpics = 0; }

/* --- fog and skies ---------------------------------------------------------------------- */

static void Fogcmd_f(int argc, const char **argv)
{
	bool sky = strstr(argv[0], "sky") != NULL;
	float *f = sky ? r_world.skyroomfog : r_world.waterfog;
	if (argc < 2) {
		Con_Printf("Current %s %f (r:%f g:%f b:%f, a:%f bias:%f)\n", argv[0], f[0], f[1], f[2], f[3], f[4], f[5]);
		return;
	}
	for (int i = 0; i < 6 && i + 1 < argc; i++)
		f[i] = (float)atof(argv[i + 1]);
}

static void Loadsky_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Current per-map skybox is %s\n", r_world.skyname[0] ? r_world.skyname : "(none)");
		return;
	}
	Cmd_ExecuteString(va("sky \"%s\"", argv[1]));
}

typedef struct {
	char sky[64];
	char maps[512]; /* space separated */
} skygroup_t;
static skygroup_t skygroups[32];
static int numskygroups;

/* skygroup <skybox> <map> [map...]: those maps use that skybox */
static void Skygroup_f(int argc, const char **argv)
{
	skygroup_t *g = NULL;
	if (argc < 2) {
		if (!numskygroups)
			Con_Printf("No skygroups defined\n");
		for (int i = 0; i < numskygroups; i++)
			Con_Printf("%s: %s\n", skygroups[i].sky, skygroups[i].maps);
		return;
	}
	for (int i = 0; i < numskygroups; i++)
		if (!Str_ICmp(skygroups[i].sky, argv[1]))
			g = &skygroups[i];
	if (argc < 3) {
		if (g)
			*g = skygroups[--numskygroups];
		return;
	}
	if (!g) {
		if (numskygroups == (int)BW_ARRAY_COUNT(skygroups))
			return;
		g = &skygroups[numskygroups++];
	}
	Str_Copy(g->sky, argv[1], sizeof(g->sky));
	g->maps[0] = 0;
	for (int i = 2; i < argc; i++) {
		Str_Cat(g->maps, argv[i], sizeof(g->maps));
		Str_Cat(g->maps, " ", sizeof(g->maps));
	}
}

/* a skygroup naming the map just loaded overrides worldspawn's sky */
void R_ToolsMapLoaded(void)
{
	char base[64];
	const char *slash = strrchr(r_world.name, '/');
	char *dot;
	Str_Copy(base, slash ? slash + 1 : r_world.name, sizeof(base));
	if ((dot = strrchr(base, '.')))
		*dot = 0;
	for (int i = 0; i < numskygroups; i++) {
		char padded[80];
		const char *maps = va(" %s", skygroups[i].maps);
		bool found = false;
		snprintf(padded, sizeof(padded), " %s ", base);
		for (const char *p = maps; *p && !found; p++)
			found = !Str_NICmp(p, padded, strlen(padded));
		if (found) {
			Str_Copy(r_world.skyname, skygroups[i].sky, sizeof(r_world.skyname));
			r_world.skybox = NULL;
		}
	}
}

typedef struct {
	int count;
	char seen[64][64];
} skylist_t;

static void sky_file(const char *name, const char *searchpath, void *ctx)
{
	char base[128], *end;
	skylist_t *l = ctx;
	const char *slash = strrchr(name, '/');
	Str_Copy(base, slash ? slash + 1 : name, sizeof(base));
	if ((end = strrchr(base, '.')))
		*end = 0;
	end = base + strlen(base) - 2;
	if (end <= base || Str_ICmp(end, "rt"))
		return;
	if (end > base && end[-1] == '_')
		end--;
	*end = 0;
	for (int i = 0; i < BW_MIN(l->count, 64); i++)
		if (!Str_ICmp(l->seen[i], base))
			return;
	if (l->count < 64)
		Str_Copy(l->seen[l->count], base, sizeof(l->seen[0]));
	Con_Printf("  %s\n", base);
	l->count++;
}

static void Listskyboxes_f(int argc, const char **argv)
{
	static skylist_t l;
	l.count = 0;
	Con_Printf("Skybox Options:\n");
	FS_List("env/*rt.*", true, sky_file, &l);
	FS_List("gfx/env/*rt.*", true, sky_file, &l);
	Con_Printf("%d skyboxes (use sky <name>)\n", l.count);
}

/* --- fonts ----------------------------------------------------------------------------- */

static void font_file(const char *name, const char *searchpath, void *ctx)
{
	Con_Printf("  %s\n", name);
	(*(int *)ctx)++;
}

static void Listfonts_f(int argc, const char **argv)
{
	int count = 0;
	Con_Printf("Font files:\n");
	FS_List("fonts/*.ttf", true, font_file, &count);
	FS_List("fonts/*.otf", true, font_file, &count);
	FS_List("fonts/*.font", true, font_file, &count);
	Con_Printf("%d font files\nLoaded:\n", count);
	for (int i = 0; i < 64; i++) {
		const char *name;
		bool ttf;
		int sizes[16], n;
		if (!R_FontInfo(i, &name, &ttf, sizes, &n))
			continue;
		Con_Printf("  %2d %s%s", i, name, ttf ? "" : " (built-in bitmap font)");
		for (int k = 0; k < n; k++)
			Con_Printf("%s%d", k ? "," : " sizes ", sizes[k]);
		Con_Printf("\n");
	}
}

/* loadfont [slot] <file> [sizes] */
static void Loadfont_f(int argc, const char **argv)
{
	int slot = -1, first = 1;
	if (argc < 2) {
		Con_Printf("loadfont [slot] <file> [sizes]: load a TrueType font for drawfont\n");
		return;
	}
	if (argc > 2 && argv[1][0] >= '0' && argv[1][0] <= '9') {
		slot = atoi(argv[1]);
		first = 2;
	}
	slot = R_LoadFont(NULL, argv[first], argc > first + 1 ? argv[first + 1] : "", slot);
	Con_Printf(slot ? "font %s in slot %d\n" : "couldn't load %s\n", argv[first], slot);
}

/* --- pointfiles and the waypoint graph ------------------------------------------------------ */

static float *points;
static int numpoints;
static char pointmap[128];
static bool route_visible;

static void Pointfile_f(int argc, const char **argv)
{
	char path[160], base[128];
	char *text, *dot;
	size_t len;
	const char *slash = strrchr(r_world.name, '/');
	Mem_Free(points);
	points = NULL;
	numpoints = 0;
	if (argc > 1 && (!strcmp(argv[1], "0") || !Str_ICmp(argv[1], "clear")))
		return;
	Str_Copy(base, slash ? slash + 1 : r_world.name, sizeof(base));
	if ((dot = strrchr(base, '.')))
		*dot = 0;
	snprintf(path, sizeof(path), "maps/%s.pts", argc > 1 ? argv[1] : base);
	if (!(text = FS_LoadFile(path, &len))) {
		snprintf(path, sizeof(path), "maps/%s.lin", argc > 1 ? argv[1] : base);
		text = FS_LoadFile(path, &len);
	}
	if (!text) {
		Con_Printf("couldn't open maps/%s.pts\n", argc > 1 ? argv[1] : base);
		return;
	}
	for (char *p = text; *p;) {
		float v[3];
		int used = 0;
		if (sscanf(p, "%f %f %f%n", &v[0], &v[1], &v[2], &used) == 3) {
			points = Mem_Realloc(points, sizeof(float) * 3 * (size_t)(numpoints + 1));
			memcpy(points + numpoints * 3, v, sizeof(v));
			numpoints++;
		}
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	Mem_Free(text);
	Str_Copy(pointmap, r_world.name, sizeof(pointmap));
	Con_Printf("%s: %d points\n", path, numpoints);
}

static void RouteVisualise_f(int argc, const char **argv)
{
	route_visible = argc > 1 ? atoi(argv[1]) != 0 : !route_visible;
	Con_Printf("waypoint graph %s%s\n", route_visible ? "shown" : "hidden",
	           route_visible && !SV_RouteNumNodes() ? " (no graph loaded on a local server)" : "");
}

static void draw_lines(const float *xyz, int n, GLenum mode, const float *rgba)
{
	static rvert_t v[4096];
	n = BW_MIN(n, (int)BW_ARRAY_COUNT(v));
	for (int i = 0; i < n; i++) {
		memset(&v[i], 0, sizeof(v[i]));
		memcpy(v[i].xyz, xyz + i * 3, sizeof(float) * 3);
		v[i].color[0] = v[i].color[1] = v[i].color[2] = v[i].color[3] = 255;
	}
	qglUniform4f(r_progs[PROG_UNLIT].u_color, rgba[0], rgba[1], rgba[2], rgba[3]);
	R_StreamDraw(mode, v, n, NULL, 0);
}

void R_DrawToolsWorld(void)
{
	static const float red[4] = {1, 0.2f, 0.2f, 1}, green[4] = {0.2f, 1, 0.3f, 1}, yellow[4] = {1, 1, 0.2f, 1};
	if ((!numpoints || strcmp(pointmap, r_world.name)) && !route_visible)
		return;
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(r_progs[PROG_UNLIT].u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(r_progs[PROG_UNLIT].u_tcmod, 0, 0, 1, 1);
	qglUniform1f(r_progs[PROG_UNLIT].u_alphatest, 0);
	GL_BindTexture(0, GL_TEXTURE_2D, r_whiteimage->tex);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
	GL_SetDepth(true, false);
	if (numpoints && !strcmp(pointmap, r_world.name))
		for (int i = 0; i < numpoints; i += 4000)
			draw_lines(points + i * 3, BW_MIN(numpoints - i, 4001), GL_LINE_STRIP, red);
	if (route_visible) {
		int n = SV_RouteNumNodes();
		float *lines = Mem_Alloc(sizeof(float) * 6 * 4096);
		int nl = 0;
		for (int i = 0; i < n; i++) {
			float org[3], radius, cross[6 * 3];
			int links;
			if (!SV_RouteNode(i, org, &radius, &links))
				continue;
			for (int a = 0; a < 3; a++)
				for (int k = 0; k < 3; k++) {
					cross[a * 6 + k] = org[k] - (a == k ? 8 : 0);
					cross[a * 6 + 3 + k] = org[k] + (a == k ? 8 : 0);
				}
			draw_lines(cross, 6, GL_LINES, yellow);
			for (int l = 0; l < links && nl < 4094; l++) {
				float to[3], r2;
				int dummy;
				if (!SV_RouteNode(SV_RouteLink(i, l), to, &r2, &dummy))
					continue;
				memcpy(lines + nl * 3, org, sizeof(org));
				memcpy(lines + (nl + 1) * 3, to, sizeof(to));
				nl += 2;
			}
		}
		if (nl)
			draw_lines(lines, nl, GL_LINES, green);
		Mem_Free(lines);
	}
	GL_SetDepth(true, true);
}

/* --- renderer choice and presets ---------------------------------------------------------------- */

static void Setrenderer_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("\nValid setrenderer parameters are:\ngl: OpenGL 3.3%s\nheadless: no window (start with -headless)\n",
		           R_Active() ? " (current)" : "");
		return;
	}
	if (!Str_ICmp(argv[1], "gl") || !Str_ICmp(argv[1], "opengl"))
		Cbuf_AddText("vid_restart\n");
	else
		Con_Printf("%s can't be switched to while running; gl is the only windowed renderer\n", argv[1]);
}

typedef struct {
	const char *name, *desc, *cvars;
} preset_t;
static const preset_t presets[] = {
	{"fast", "Fewest effects", "r_drawparticles 1; r_particles_max 2048; r_part_density 0.5; r_drawdecals 0; r_decal_max 128; gl_texturemode nearest; r_lightmapscale 2"},
	{"normal", "The defaults", "r_drawparticles 1; r_particles_max 16384; r_part_density 1; r_drawdecals 1; r_decal_max 1024; gl_texturemode linear; r_lightmapscale 2"},
	{"nice", "More of everything", "r_drawparticles 1; r_particles_max 32768; r_part_density 1.5; r_drawdecals 1; r_decal_max 4096; gl_texturemode linear; r_lightmapscale 2"},
};

static void FpsPreset_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("fps_preset <name>:\n");
		for (size_t i = 0; i < BW_ARRAY_COUNT(presets); i++)
			Con_Printf("  %-8s %s\n", presets[i].name, presets[i].desc);
		return;
	}
	for (size_t i = 0; i < BW_ARRAY_COUNT(presets); i++)
		if (!Str_ICmp(argv[1], presets[i].name)) {
			Cbuf_InsertText(va("%s\n", presets[i].cvars));
			return;
		}
	Con_Printf("no preset %s\n", argv[1]);
}

/* --- screenshots that draw the view again -------------------------------------------------------- */

static bool redrawing;
bool R_ToolsRedrawing(void) { return redrawing; }

static void write_func(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ctx); }

/* RGB rows from the top */
static bool write_image(const char *rel, int w, int h, const uint8_t *rgb)
{
	char path[1024];
	FILE *f;
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", rel);
		return false;
	}
	stbi_flip_vertically_on_write(0);
	if (strstr(rel, ".jpg"))
		stbi_write_jpg_to_func(write_func, f, w, h, 3, rgb, 90);
	else if (strstr(rel, ".tga"))
		stbi_write_tga_to_func(write_func, f, w, h, 3, rgb);
	else
		stbi_write_png_to_func(write_func, f, w, h, 3, rgb, w * 3);
	fclose(f);
	Con_Printf("Wrote %s/%s\n", FS_GameDir(), rel);
	return true;
}

/* draws the scene from a viewpoint into an off-screen target; RGB rows
 * from the top (Mem_Free it) */
static uint8_t *render_view(const float *org, const float *ang, float fovx, float fovy, int w, int h)
{
	GLuint fb, rb[2];
	uint8_t *px, *row;
	rscene_t saved;
	int pw = r_pwidth, ph = r_pheight;
	qglGenFramebuffers(1, &fb);
	qglGenRenderbuffers(2, rb);
	qglBindFramebuffer(GL_FRAMEBUFFER, fb);
	qglBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	qglBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	px = NULL;
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		memcpy(&saved.vx, &r_scene.vx, sizeof(float) * 4);
		saved.fov_x = r_scene.fov_x, saved.fov_y = r_scene.fov_y, saved.afov = r_scene.afov;
		VectorCopy(r_scene.origin, saved.origin);
		VectorCopy(r_scene.angles, saved.angles);
		saved.drawworld = r_scene.drawworld, saved.perspective = r_scene.perspective;
		r_pwidth = w, r_pheight = h;
		r_scene.vx = r_scene.vy = 0;
		r_scene.vw = (float)r_vwidth, r_scene.vh = (float)r_vheight;
		r_scene.fov_x = fovx, r_scene.fov_y = fovy;
		VectorCopy(org, r_scene.origin);
		VectorCopy(ang, r_scene.angles);
		r_scene.drawworld = r_scene.maindrawworld;
		r_scene.perspective = true;
		qglDisable(GL_SCISSOR_TEST);
		qglViewport(0, 0, w, h);
		qglClearColor(0, 0, 0, 1);
		qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		redrawing = true;
		R_RenderScene();
		redrawing = false;
		px = Mem_Alloc((size_t)w * h * 3);
		row = Mem_Alloc((size_t)w * 3);
		qglPixelStorei(GL_PACK_ALIGNMENT, 1);
		qglReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
		for (int y = 0; y < h / 2; y++) {
			memcpy(row, px + (size_t)y * w * 3, (size_t)w * 3);
			memcpy(px + (size_t)y * w * 3, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
			memcpy(px + (size_t)(h - 1 - y) * w * 3, row, (size_t)w * 3);
		}
		Mem_Free(row);
		memcpy(&r_scene.vx, &saved.vx, sizeof(float) * 4);
		r_scene.fov_x = saved.fov_x, r_scene.fov_y = saved.fov_y, r_scene.afov = saved.afov;
		VectorCopy(saved.origin, r_scene.origin);
		VectorCopy(saved.angles, r_scene.angles);
		r_scene.drawworld = saved.drawworld, r_scene.perspective = saved.perspective;
		r_pwidth = pw, r_pheight = ph;
	} else {
		Con_Printf("couldn't make a %dx%d render target\n", w, h);
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	qglDeleteRenderbuffers(2, rb);
	qglDeleteFramebuffers(1, &fb);
	qglDisable(GL_SCISSOR_TEST);
	qglViewport(0, 0, r_pwidth, r_pheight);
	return px;
}

/* skybox faces as R_LoadCubemap and the sky shader read them: the view
 * angles that draw each. Written out, the sides are mirrored and the top
 * and bottom flipped, which is how those files are stored (checked by
 * drawing env/dune back into the same faces). */
static const char *const face_suffix[6] = {"lf", "bk", "rt", "ft", "up", "dn"};
static const float face_angles[6][3] = {{0, 0, 0}, {0, 90, 0}, {0, 180, 0}, {0, 270, 0}, {-90, 0, 0}, {90, 0, 0}};

static uint8_t *faces[6];
static float face_axes[6][3][3]; /* forward, right, up of each face */

static bool render_faces(const float *org, int size, float eyesep)
{
	for (int f = 0; f < 6; f++) {
		float o[3];
		AngleVectors(face_angles[f], face_axes[f][0], face_axes[f][1], face_axes[f][2]);
		for (int k = 0; k < 3; k++)
			o[k] = org[k] + face_axes[f][1][k] * (f < 4 ? eyesep : 0);
		Mem_Free(faces[f]);
		if (!(faces[f] = render_view(o, face_angles[f], 90, 90, size, size)))
			return false;
	}
	return true;
}

static void free_faces(void)
{
	for (int f = 0; f < 6; f++) {
		Mem_Free(faces[f]);
		faces[f] = NULL;
	}
}

/* equirectangular: longitude across, latitude down, from the six faces */
static void faces_to_equirect(int size, uint8_t *out, int w, int h, float yaw0)
{
	for (int y = 0; y < h; y++) {
		float lat = (0.5f - (y + 0.5f) / h) * (float)M_PI;
		for (int x = 0; x < w; x++) {
			float lon = yaw0 * (float)M_PI / 180 + (0.5f - (x + 0.5f) / w) * 2 * (float)M_PI;
			float d[3] = {cosf(lat) * cosf(lon), cosf(lat) * sinf(lon), sinf(lat)};
			int best = 0;
			float bestdot = -2, u, v;
			for (int f = 0; f < 6; f++) {
				float dot = DotProduct(d, face_axes[f][0]);
				if (dot > bestdot)
					bestdot = dot, best = f;
			}
			u = DotProduct(d, face_axes[best][1]) / bestdot;
			v = DotProduct(d, face_axes[best][2]) / bestdot;
			{
				int px = BW_CLAMP((int)((u * 0.5f + 0.5f) * size), 0, size - 1);
				int py = BW_CLAMP((int)((0.5f - v * 0.5f) * size), 0, size - 1);
				memcpy(out + ((size_t)y * w + x) * 3, faces[best] + ((size_t)py * size + px) * 3, 3);
			}
		}
	}
}

enum { JOB_NONE, JOB_MEGA, JOB_STEREO, JOB_CUBEMAP, JOB_360, JOB_VR, JOB_BUILDCUBEMAPS };
static struct {
	int kind;
	char name[128];
	int w, h;
	float origin[3];
	bool haveorigin;
} job;

static cvar_t *r_stereo_separation;

static void shot_name(char *out, size_t size, const char *given)
{
	if (given && *given)
		snprintf(out, size, "screenshots/%s%s", given, strchr(given, '.') ? "" : ".png");
	else
		for (int i = 0; i < 10000; i++) {
			snprintf(out, size, "screenshots/bulwark-%04d.png", i);
			if (!FS_FileExists(out))
				break;
		}
}

static const char *map_base(void)
{
	static char base[64];
	const char *slash = strrchr(r_world.name, '/');
	char *dot;
	Str_Copy(base, slash ? slash + 1 : r_world.name, sizeof(base));
	if ((dot = strrchr(base, '.')))
		*dot = 0;
	return base;
}

static void write_cubemap(const char *base, const float *org, int size)
{
	if (!render_faces(org, size, 0)) {
		free_faces();
		return;
	}
	for (int f = 0; f < 6; f++) {
		uint8_t *img = faces[f], *out = Mem_Alloc((size_t)size * size * 3);
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++)
				memcpy(out + ((size_t)y * size + x) * 3,
				       f < 4 ? img + ((size_t)y * size + (size - 1 - x)) * 3 : img + ((size_t)(size - 1 - y) * size + x) * 3, 3);
		write_image(va("%s_%s.png", base, face_suffix[f]), size, size, out);
		Mem_Free(out);
	}
	free_faces();
}

/* the env_cubemap entities of the map: origin and size */
static int find_envmaps(float (*org)[3], int *sizes, int max)
{
	const char *p = cl.world ? cl.world->entities : NULL;
	int n = 0;
	while (p && (p = strchr(p, '{')) && n < max) {
		const char *end = strchr(p, '}');
		char block[2048];
		const char *cls, *o, *s;
		if (!end)
			break;
		snprintf(block, sizeof(block), "%.*s", (int)BW_MIN((size_t)(end - p), sizeof(block) - 1), p);
		p = end + 1;
		cls = strstr(block, "\"classname\" \"env_cubemap\"");
		if (!cls || !(o = strstr(block, "\"origin\" \"")))
			continue;
		if (sscanf(o + 10, "%f %f %f", &org[n][0], &org[n][1], &org[n][2]) != 3)
			continue;
		sizes[n] = (s = strstr(block, "\"cubemapsize\" \"")) ? atoi(s + 15) : 0;
		if (sizes[n] <= 0)
			sizes[n] = 256;
		n++;
	}
	return n;
}

void R_ToolsEndFrame(void)
{
	int kind = job.kind;
	char rel[256];
	GLint maxrb = 4096;
	job.kind = JOB_NONE;
	if (kind == JOB_NONE || !R_Active())
		return;
	if (!r_scene.havemain || !r_world.loaded) {
		Con_Printf("Please start a map first\n");
		return;
	}
	qglGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
	job.w = BW_CLAMP(job.w, 16, maxrb);
	job.h = BW_CLAMP(job.h, 16, maxrb);
	R_Flush2D();
	if (kind == JOB_MEGA || kind == JOB_STEREO) {
		/* the main view's vertical field of view, widened to the new shape */
		float fy = r_scene.mainfov[1];
		float fx = atanf(tanf(fy * (float)M_PI / 360) * job.w / job.h) * 360 / (float)M_PI;
		uint8_t *eye[2] = {NULL, NULL};
		float sep = r_stereo_separation->value / 2;
		float fwd[3], right[3], up[3];
		AngleVectors(r_scene.mainangles, fwd, right, up);
		for (int e = 0; e < (kind == JOB_STEREO ? 2 : 1); e++) {
			float o[3];
			for (int k = 0; k < 3; k++)
				o[k] = r_scene.mainorigin[k] + (kind == JOB_STEREO ? right[k] * (e ? sep : -sep) : 0);
			eye[e] = render_view(o, r_scene.mainangles, fx, fy, job.w, job.h);
		}
		shot_name(rel, sizeof(rel), job.name);
		if (kind == JOB_MEGA && eye[0]) {
			write_image(rel, job.w, job.h, eye[0]);
		} else if (eye[0] && eye[1]) {
			uint8_t *both = Mem_Alloc((size_t)job.w * 2 * job.h * 3);
			for (int y = 0; y < job.h; y++)
				for (int e = 0; e < 2; e++)
					memcpy(both + ((size_t)y * job.w * 2 + (size_t)e * job.w) * 3, eye[e] + (size_t)y * job.w * 3, (size_t)job.w * 3);
			write_image(rel, job.w * 2, job.h, both);
			Mem_Free(both);
		}
		Mem_Free(eye[0]);
		Mem_Free(eye[1]);
	} else if (kind == JOB_CUBEMAP) {
		const float *o = job.haveorigin ? job.origin : r_scene.mainorigin;
		if (job.name[0])
			snprintf(rel, sizeof(rel), "textures/%s", job.name);
		else
			snprintf(rel, sizeof(rel), "textures/%s/%d_%d_%d", map_base(), (int)floorf(o[0] + 0.5f), (int)floorf(o[1] + 0.5f),
			         (int)floorf(o[2] + 0.5f));
		write_cubemap(rel, o, job.w);
	} else if (kind == JOB_360 || kind == JOB_VR) {
		int eyes = kind == JOB_VR ? 2 : 1, w = job.w, h = kind == JOB_VR ? job.w / 2 : job.h;
		int size = BW_CLAMP(w / 4, 64, maxrb);
		uint8_t *out = Mem_Alloc((size_t)w * h * eyes * 3);
		bool ok = true;
		for (int e = 0; e < eyes && ok; e++) {
			ok = render_faces(r_scene.mainorigin, size, eyes > 1 ? (e ? 1 : -1) * r_stereo_separation->value / 2 : 0);
			if (ok)
				faces_to_equirect(size, out + (size_t)e * w * h * 3, w, h, r_scene.mainangles[1]);
		}
		free_faces();
		shot_name(rel, sizeof(rel), job.name);
		if (ok)
			write_image(rel, w, h * eyes, out);
		Mem_Free(out);
	} else if (kind == JOB_BUILDCUBEMAPS) {
		float org[256][3];
		int sizes[256], n = find_envmaps(org, sizes, 256);
		for (int i = 0; i < n; i++) {
			snprintf(rel, sizeof(rel), "textures/env/%s_%d_%d_%d", map_base(), (int)floorf(org[i][0] + 0.5f),
			         (int)floorf(org[i][1] + 0.5f), (int)floorf(org[i][2] + 0.5f));
			write_cubemap(rel, org[i], BW_MIN(sizes[i], maxrb));
		}
		Con_Printf("%d env_cubemap sites\n", n);
	}
}

static void queue(int kind, int argc, const char **argv, int defw, int defh)
{
	memset(&job, 0, sizeof(job));
	job.kind = kind;
	Str_Copy(job.name, argc > 1 ? argv[1] : "", sizeof(job.name));
	job.w = argc > 2 ? atoi(argv[2]) : defw;
	job.h = argc > 3 ? atoi(argv[3]) : defh;
	if (!R_Active()) {
		Con_Printf("%s needs a renderer\n", argv[0]);
		job.kind = JOB_NONE;
	}
}

static void ScreenshotMega_f(int argc, const char **argv) { queue(JOB_MEGA, argc, argv, r_pwidth * 2, r_pheight * 2); }
static void ScreenshotStereo_f(int argc, const char **argv) { queue(JOB_STEREO, argc, argv, r_pwidth, r_pheight); }
static void Screenshot360_f(int argc, const char **argv) { queue(JOB_360, argc, argv, 2048, 1024); }
static void ScreenshotVR_f(int argc, const char **argv) { queue(JOB_VR, argc, argv, 2048, 2048); }
static void ScreenshotCubemap_f(int argc, const char **argv)
{
	queue(JOB_CUBEMAP, argc, argv, 256, 256);
	job.h = job.w;
}
static void Buildcubemaps_f(int argc, const char **argv) { queue(JOB_BUILDCUBEMAPS, 1, argv, 256, 256); }

/* mod_findcubemaps: the map's env_cubemap sites and each surface's nearest
 * one, written into the map as the ENVMAP and SURFENVMAP BSPX lumps */
static void Findcubemaps_f(int argc, const char **argv)
{
	float org[256][3];
	int sizes[256], n;
	uint8_t *envmap;
	uint32_t *surf;
	int nsurf;
	const char *names[2] = {"ENVMAP", "SURFENVMAP"};
	const void *datas[2];
	size_t lens[2];
	if (!r_world.loaded) {
		Con_Printf("Please start a map first\n");
		return;
	}
	n = find_envmaps(org, sizes, 256);
	Con_Printf("%d env_cubemap sites\n", n);
	if (!n)
		return;
	envmap = Mem_Alloc((size_t)n * 16);
	for (int i = 0; i < n; i++) {
		memcpy(envmap + i * 16, org[i], 12);
		memcpy(envmap + i * 16 + 12, &sizes[i], 4);
	}
	/* in the file's surface order */
	nsurf = r_world.numsurfaces;
	surf = Mem_Alloc(sizeof(uint32_t) * (size_t)(nsurf + 1));
	for (int i = 0; i < r_world.numsurfaces; i++) {
		const rsurface_t *s = &r_world.surfaces[i];
		float c[3], best = 1e30f;
		uint32_t pick = ~0u;
		if (s->orig < 0 || s->orig >= nsurf)
			continue;
		for (int k = 0; k < 3; k++)
			c[k] = (s->mins[k] + s->maxs[k]) * 0.5f;
		for (int e = 0; e < n; e++) {
			float d[3] = {org[e][0] - c[0], org[e][1] - c[1], org[e][2] - c[2]};
			if (DotProduct(d, d) < best) {
				best = DotProduct(d, d);
				pick = (uint32_t)e;
			}
		}
		surf[s->orig] = pick;
	}
	datas[0] = envmap, lens[0] = (size_t)n * 16;
	datas[1] = surf, lens[1] = sizeof(uint32_t) * (size_t)nsurf;
	BSPX_SetLumps(r_world.name, names, datas, lens, 2);
	Mem_Free(envmap);
	Mem_Free(surf);
}

void R_InitTools(void)
{
	r_stereo_separation = Cvar_Get("r_stereo_separation", "4", CVAR_ARCHIVE, "Distance between the eyes for stereo and VR screenshots");
	Cmd_AddD("r_imagelist", Imagelist_f, "r_imagelist [filter]: the loaded images");
	Cmd_AddD("r_imagelist_wad", ImagelistWad_f, "r_imagelist_wad [filter]: the images in the map's WAD files");
	Cmd_AddD("r_shaderlist", Shaderlist_f, "r_shaderlist [filter]: the loaded materials");
	Cmd_AddD("r_imageformats", Imageformats_f, "The image files and GPU formats the renderer uses");
	Cmd_AddD("mod_texturelist", Texturelist_f, "mod_texturelist [filter]: the world's materials and surface counts");
	Cmd_AddD("mod_memlist", Memlist_f, "The loaded models and roughly what they take");
	Cmd_AddD("mod_batchlist", Batchlist_f, "The world's draw batches");
	Cmd_AddD("version_modelformats", Modelformats_f, "The model and map formats the engine reads");
	Cmd_AddD("r_dumpshaders", Dumpshaders_f, "Write the GLSL programs to glsl/");
	Cmd_AddD("r_showshader", Showshader_f, "r_showshader [material]: a material's definition (default: the surface in view)");
	Cmd_AddD("r_remapshader", Remapshader_f, "r_remapshader <from> <to>: draw one material as another (game code or cheats)");
	Cmd_AddD("mod_usetexture", Usetexture_f, "mod_usetexture <material>: put a material on the world surface in view");
	Cmd_AddD("showpic", Showpic_f, "showpic <image> <place> <x> <y> <zone> [width] [height] [touchcommand]: pin a picture to the screen");
	Cmd_AddD("showpic_removeall", ShowpicRemoveall_f, "Remove the showpic pictures");
	Cmd_AddD("waterfog", Fogcmd_f, "waterfog <density> <red> <green> <blue> <alpha> <depthbias>: fog under water");
	Cmd_AddD("skyroomfog", Fogcmd_f, "skyroomfog <density> <red> <green> <blue> <alpha> <depthbias>: fog in the skyroom");
	Cmd_AddD("loadsky", Loadsky_f, "loadsky [name]: the map's skybox (DarkPlaces' name for sky)");
	Cmd_AddD("skygroup", Skygroup_f, "skygroup <skybox> <map...>: maps that use a skybox");
	Cmd_AddD("listskyboxes", Listskyboxes_f, "The skyboxes in env/ and gfx/env/");
	Cmd_AddD("listfonts", Listfonts_f, "The font files and the loaded fonts");
	Cmd_AddD("loadfont", Loadfont_f, "loadfont [slot] <file> [sizes]: load a font");
	Cmd_AddD("pointfile", Pointfile_f, "pointfile [map|clear]: show a map compiler's leak trail (maps/<map>.pts or .lin)");
	Cmd_AddD("route_visualise", RouteVisualise_f, "route_visualise [0|1]: show the waypoint graph of a local server");
	Cmd_AddD("setrenderer", Setrenderer_f, "setrenderer [gl]: the renderers there are; gl restarts video");
	Cmd_AddD("fps_preset", FpsPreset_f, "fps_preset <fast|normal|nice>: set the effect cvars for speed or looks");
	Cmd_AddD("screenshot_mega", ScreenshotMega_f, "screenshot_mega [name] [width] [height]: the 3D view drawn bigger than the window");
	Cmd_AddD("screenshot_stereo", ScreenshotStereo_f, "screenshot_stereo [name] [width] [height]: left and right eye side by side");
	Cmd_AddD("screenshot_360", Screenshot360_f, "screenshot_360 [name] [width] [height]: all the way round, equirectangular");
	Cmd_AddD("screenshot_vr", ScreenshotVR_f, "screenshot_vr [name] [width]: stereo 360, left eye over right");
	Cmd_AddD("screenshot_cubemap", ScreenshotCubemap_f, "screenshot_cubemap [name] [size]: six faces from here, as a skybox set");
	Cmd_AddD("envmap", ScreenshotCubemap_f, "envmap [name] [size]: screenshot_cubemap");
	Cmd_AddD("mod_buildcubemaps", Buildcubemaps_f, "Draw a cubemap at each env_cubemap into textures/env/");
	Cmd_AddD("mod_findcubemaps", Findcubemaps_f, "Find each surface's nearest env_cubemap and store it in the map's BSPX lumps");
}

void R_MainView(float origin[3], float forward[3])
{
	VectorCopy(r_scene.mainorigin, origin);
	VectorCopy(r_scene.mainforward, forward);
}
