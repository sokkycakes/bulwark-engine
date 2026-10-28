/* Bulwark engine - a map's static realtime lights and their editor.
 *
 * The lights come from maps/<map>.rtlights (DarkPlaces' text format, one
 * light a line, with FTE's spin, cone and near clip after) or from the
 * map's light entities. With r_shadow_realtime_world on, or while editing,
 * the ones near the view light the scene as dynamic lights do, with their
 * light style, and draw coronas where they have one.
 *
 * r_editlights 1 shows them; the one under the crosshair is selected
 * (unless locked) and the r_editlights_* commands change it. Its
 * properties are also the $r_editlights_current_* macros.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RTLIGHTS 4096

typedef struct {
	float origin[3], angles[3], color[3];
	float radius, corona, coronasize;
	float ambient, diffuse, specular;
	int style;
	bool shadows, normalmode, realtimemode;
	char cubemap[64];
	float spin[3], cone, nearclip;
} rtlight_t;

static rtlight_t *lights;
static int numlights;
static int selected = -1;
static bool locked;
static rtlight_t clipboard;
static bool haveclipboard;

static cvar_t *r_editlights, *r_editlights_cursordistance, *r_editlights_cursorpushback, *r_editlights_cursorpushoff;
static cvar_t *r_editlights_cursorgrid, *r_shadow_realtime_world, *r_shadow_realtime_world_import;
static cvar_t *r_editlights_import_radius, *r_editlights_import_ambient, *r_editlights_import_diffuse;
static cvar_t *r_editlights_import_specular, *r_coronas, *r_coronas_intensity;

static void defaults(rtlight_t *l)
{
	memset(l, 0, sizeof(*l));
	l->color[0] = l->color[1] = l->color[2] = 1;
	l->radius = 200;
	l->coronasize = 0.35f;
	l->diffuse = 1;
	l->specular = 1;
	l->shadows = true;
	l->realtimemode = true;
}

static rtlight_t *add_light(void)
{
	if (numlights >= MAX_RTLIGHTS)
		return NULL;
	if (!lights)
		lights = Mem_Alloc(sizeof(*lights) * MAX_RTLIGHTS);
	defaults(&lights[numlights]);
	return &lights[numlights++];
}

static void clear_lights(void)
{
	numlights = 0;
	selected = -1;
	locked = false;
}

/* --- files ------------------------------------------------------------------------------- */

static void rtlights_path(char *out, size_t size)
{
	snprintf(out, size, "maps/%s.rtlights", cl.mapname[0] ? cl.mapname : "unnamed");
}

/* [!]x y z radius r g b style ["cubemap" [corona [pitch yaw roll [coronasize ambient diffuse specular [flags
 * [spinx spiny spinz cone nearclip]]]]]] */
static bool load_rtlights(void)
{
	char path[160], line[1024], tok[128];
	rtlights_path(path, sizeof(path));
	char *data = FS_LoadFile(path, NULL);
	if (!data)
		return false;
	for (char *p = data; *p;) {
		size_t n = strcspn(p, "\n");
		Str_Copy(line, p, BW_MIN(sizeof(line), n + 1));
		p += n + (p[n] == '\n');
		const char *s = line;
		while (*s == ' ' || *s == '\t' || *s == '\r')
			s++;
		if (!*s || (s[0] == '/' && s[1] == '/'))
			continue;
		bool noshadow = *s == '!';
		if (noshadow)
			s++;
		float v[24];
		int nv = 0, flags = 2;
		char cubemap[64] = "";
		while ((s = COM_Parse(s, tok, sizeof(tok))) && nv < 24) {
			if (nv == 8) { /* the cubemap name, quoted */
				Str_Copy(cubemap, tok, sizeof(cubemap));
				v[nv++] = 0;
				continue;
			}
			v[nv++] = (float)atof(tok);
		}
		if (nv < 8)
			continue;
		rtlight_t *l = add_light();
		if (!l)
			break;
		for (int k = 0; k < 3; k++)
			l->origin[k] = v[k];
		l->radius = v[3];
		for (int k = 0; k < 3; k++)
			l->color[k] = v[4 + k];
		l->style = (int)v[7];
		Str_Copy(l->cubemap, cubemap, sizeof(l->cubemap));
		if (nv > 9)
			l->corona = v[9];
		if (nv > 12)
			for (int k = 0; k < 3; k++)
				l->angles[k] = v[10 + k];
		if (nv > 16) {
			l->coronasize = v[13];
			l->ambient = v[14];
			l->diffuse = v[15];
			l->specular = v[16];
		}
		if (nv > 17)
			flags = (int)v[17];
		if (nv > 22) {
			for (int k = 0; k < 3; k++)
				l->spin[k] = v[18 + k];
			l->cone = v[21];
			l->nearclip = v[22];
		}
		l->shadows = !noshadow;
		l->normalmode = (flags & 1) != 0;
		l->realtimemode = (flags & 2) != 0;
	}
	Mem_Free(data);
	return true;
}

static bool save_rtlights(void)
{
	char rel[160], path[1024];
	strbuf_t sb = {0};
	rtlights_path(rel, sizeof(rel));
	for (int i = 0; i < numlights; i++) {
		const rtlight_t *l = &lights[i];
		SB_Printf(&sb, "%s%g %g %g %g %g %g %g %d \"%s\" %g %g %g %g %g %g %g %g %d", l->shadows ? "" : "!", l->origin[0],
		          l->origin[1], l->origin[2], l->radius, l->color[0], l->color[1], l->color[2], l->style, l->cubemap,
		          l->corona, l->angles[0], l->angles[1], l->angles[2], l->coronasize, l->ambient, l->diffuse, l->specular,
		          (l->normalmode ? 1 : 0) | (l->realtimemode ? 2 : 0));
		if (l->spin[0] || l->spin[1] || l->spin[2] || l->cone || l->nearclip)
			SB_Printf(&sb, " %g %g %g %g %g", l->spin[0], l->spin[1], l->spin[2], l->cone, l->nearclip);
		SB_AppendStr(&sb, "\n");
	}
	FILE *f = NULL;
	if (FS_WritePath(rel, path, sizeof(path)) && FS_CreatePath(path))
		f = fopen(path, "wb");
	bool ok = f && (!sb.len || fwrite(sb.data, 1, sb.len, f) == sb.len);
	if (f)
		fclose(f);
	SB_Free(&sb);
	Con_Printf(ok ? "Saved %s\n" : "Couldn't write %s\n", rel);
	return ok;
}

/* light, light_fluoro and the rest of the map's light entities */
static void import_bsp(void)
{
	char key[128], value[256];
	const char *s = cl.world ? cl.world->entities : NULL;
	while (s && (s = COM_Parse(s, key, sizeof(key))) && key[0] == '{') {
		float origin[3] = {0, 0, 0}, color[3] = {1, 1, 1}, level = 300;
		int style = 0;
		bool islight = false;
		while ((s = COM_Parse(s, key, sizeof(key))) && key[0] != '}') {
			if (!(s = COM_Parse(s, value, sizeof(value))))
				break;
			if (!strcmp(key, "classname"))
				islight = !strncmp(value, "light", 5) && strcmp(value, "light_environment");
			else if (!strcmp(key, "origin"))
				sscanf(value, "%f %f %f", &origin[0], &origin[1], &origin[2]);
			else if (!strcmp(key, "light") || !strcmp(key, "_light")) {
				/* Quake's single level, or Half-Life's "r g b brightness" */
				float v[4];
				int n = sscanf(value, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
				if (n >= 3) {
					for (int k = 0; k < 3; k++)
						color[k] = v[k] / 255.0f;
					level = n == 4 ? v[3] : 300;
				} else if (n == 1)
					level = v[0];
			} else if (!strcmp(key, "_color") || !strcmp(key, "color")) {
				sscanf(value, "%f %f %f", &color[0], &color[1], &color[2]);
				if (color[0] > 1 || color[1] > 1 || color[2] > 1)
					for (int k = 0; k < 3; k++)
						color[k] /= 255.0f;
			} else if (!strcmp(key, "style"))
				style = atoi(value);
		}
		if (!s || !islight)
			continue;
		rtlight_t *l = add_light();
		if (!l)
			break;
		memcpy(l->origin, origin, sizeof(origin));
		memcpy(l->color, color, sizeof(color));
		l->radius = level * r_editlights_import_radius->value;
		l->style = style;
		l->ambient = r_editlights_import_ambient->value;
		l->diffuse = r_editlights_import_diffuse->value;
		l->specular = r_editlights_import_specular->value;
	}
}

/* the lights for a map: from its .rtlights, or its entities */
static void reload(const char *source)
{
	clear_lights();
	if (!Str_ICmp(source, "none"))
		return;
	if (!Str_ICmp(source, "bsp")) {
		import_bsp();
		return;
	}
	/* rtlights, statics and the default all prefer the file */
	if (!load_rtlights() && (Str_ICmp(source, "rtlights") || r_shadow_realtime_world_import->ivalue))
		import_bsp();
}

void R_LoadRTLights(void)
{
	reload(r_shadow_realtime_world_import->ivalue ? "bsp" : "");
}

static void clear_qclights(void);
void R_ClearRTLights(void)
{
	clear_lights();
	clear_qclights();
}

/* --- the scene ---------------------------------------------------------------------------- */

static float style_value(int style)
{
	const char *s;
	if (style < 0 || style >= 256 || !(s = cl.lightstyles[style]) || !*s)
		return 1;
	size_t n = strlen(s);
	int c = s[(size_t)(cl.time * 10) % n];
	return (float)(c - 'a') / ('m' - 'a');
}

static float score(const rtlight_t *l)
{
	float d = 0;
	for (int k = 0; k < 3; k++)
		d += (l->origin[k] - r_scene.origin[k]) * (l->origin[k] - r_scene.origin[k]);
	return sqrtf(d) - l->radius;
}

/* the lights nearest the view join the dynamic lights for this scene */
void R_AddRTLights(void)
{
	static int order[MAX_RTLIGHTS];
	static float scores[MAX_RTLIGHTS];
	int n = 0;
	if (!numlights || !r_scene.drawworld || (!r_shadow_realtime_world->ivalue && !r_editlights->ivalue))
		return;
	for (int i = 0; i < numlights; i++) {
		if (!lights[i].realtimemode || lights[i].radius <= 0)
			continue;
		float s = score(&lights[i]);
		if (s > 0)
			continue; /* the view is outside its reach */
		int k = n++;
		while (k > 0 && scores[k - 1] > s) {
			scores[k] = scores[k - 1];
			order[k] = order[k - 1];
			k--;
		}
		scores[k] = s;
		order[k] = i;
	}
	for (int i = 0; i < n && r_scene.numdlights < R_MAX_DLIGHTS; i++) {
		const rtlight_t *l = &lights[order[i]];
		float c[3], v = style_value(l->style) * l->diffuse;
		for (int k = 0; k < 3; k++)
			c[k] = l->color[k] * v;
		R_AddDynamicLight(l->origin, l->radius, c);
	}
}

/* --- the editor's view -------------------------------------------------------------------- */

static bool to_screen(const float *p, float *sx, float *sy)
{
	const float *m = r_scene.mainviewproj;
	float clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
	if (clip[3] <= 0.01f)
		return false;
	*sx = r_scene.mainvp[0] + (clip[0] / clip[3] + 1) * 0.5f * r_scene.mainvp[2];
	*sy = r_scene.mainvp[1] + (1 - clip[1] / clip[3]) * 0.5f * r_scene.mainvp[3];
	return true;
}

static bool visible_from_view(const float *p)
{
	bsptrace_t tr;
	static const float zero[3] = {0, 0, 0};
	if (!cl.world)
		return true;
	BSP_TraceModel(cl.world, 0, zero, r_scene.mainorigin, zero, zero, p, CONTENTS_SOLID, &tr);
	return tr.fraction > 0.999f;
}

/* where the crosshair meets the world, pulled back off the surface */
static void cursor_point(float *out)
{
	float end[3];
	bsptrace_t tr;
	static const float zero[3] = {0, 0, 0};
	float dist = r_editlights_cursordistance->value;
	for (int k = 0; k < 3; k++)
		end[k] = r_scene.mainorigin[k] + r_scene.mainforward[k] * dist;
	if (cl.world)
		BSP_TraceModel(cl.world, 0, zero, r_scene.mainorigin, zero, zero, end, CONTENTS_SOLID, &tr);
	else
		tr.fraction = 1, VectorCopy(end, tr.endpos), tr.normal[0] = tr.normal[1] = tr.normal[2] = 0;
	for (int k = 0; k < 3; k++) {
		out[k] = tr.endpos[k] - r_scene.mainforward[k] * r_editlights_cursorpushback->value;
		if (tr.fraction < 1)
			out[k] += tr.normal[k] * r_editlights_cursorpushoff->value;
		float g = r_editlights_cursorgrid->value;
		if (g > 0)
			out[k] = floorf(out[k] / g + 0.5f) * g;
	}
}

/* each frame while editing: the light nearest the crosshair's line */
static void select_light(void)
{
	float best = 0.97f; /* about 14 degrees off the crosshair at most */
	int found = -1;
	if (locked)
		return;
	for (int i = 0; i < numlights; i++) {
		float d[3], len = 0, dot = 0;
		for (int k = 0; k < 3; k++) {
			d[k] = lights[i].origin[k] - r_scene.mainorigin[k];
			len += d[k] * d[k];
		}
		len = sqrtf(len);
		if (len < 1 || len > r_editlights_cursordistance->value) {
			if (len < 1)
				found = i, best = 2;
			continue;
		}
		for (int k = 0; k < 3; k++)
			dot += d[k] / len * r_scene.mainforward[k];
		if (dot > best && visible_from_view(lights[i].origin)) {
			best = dot;
			found = i;
		}
	}
	selected = found;
}

static void draw_coronas(void)
{
	if (!r_coronas->value && !r_editlights->ivalue)
		return;
	for (int i = 0; i < numlights; i++) {
		const rtlight_t *l = &lights[i];
		float sx, sy, d = 0;
		if (l->corona <= 0 || !to_screen(l->origin, &sx, &sy))
			continue;
		for (int k = 0; k < 3; k++)
			d += (l->origin[k] - r_scene.mainorigin[k]) * (l->origin[k] - r_scene.mainorigin[k]);
		d = sqrtf(d);
		if (d > l->radius * 4 || !visible_from_view(l->origin))
			continue;
		float size = BW_MAX(4.0f, l->radius * l->coronasize * 64 / BW_MAX(d, 1.0f));
		float a = BW_MIN(1.0f, l->corona * r_coronas_intensity->value * style_value(l->style)) * (r_coronas->value ? r_coronas->value : 1);
		R_DrawFill(sx - size / 2, sy - size / 2, size, size, l->color, BW_MIN(a, 1.0f) * 0.5f, 1);
	}
}

void R_DrawRTLightsOverlay(int w, int h)
{
	static const float yellow[3] = {1, 1, 0.2f}, white[3] = {1, 1, 1}, back[3] = {0, 0, 0};
	(void)w, (void)h;
	if (!r_scene.havemain || !r_scene.maindrawworld)
		return;
	draw_coronas();
	if (!r_editlights->ivalue)
		return;
	select_light();
	for (int i = 0; i < numlights; i++) {
		float sx, sy, d = 0;
		for (int k = 0; k < 3; k++)
			d += (lights[i].origin[k] - r_scene.mainorigin[k]) * (lights[i].origin[k] - r_scene.mainorigin[k]);
		if (d > r_editlights_cursordistance->value * r_editlights_cursordistance->value || !to_screen(lights[i].origin, &sx, &sy))
			continue;
		float s = i == selected ? 10 : 6;
		if (i == selected)
			R_DrawFill(sx - s / 2 - 2, sy - s / 2 - 2, s + 4, s + 4, yellow, 1, 0);
		R_DrawFill(sx - s / 2, sy - s / 2, s, s, lights[i].color, 1, 0);
	}
	{
		float c[3], sx, sy;
		cursor_point(c);
		if (to_screen(c, &sx, &sy)) {
			R_DrawFill(sx - 4, sy, 9, 1, white, 0.8f, 0);
			R_DrawFill(sx, sy - 4, 1, 9, white, 0.8f, 0);
		}
	}
	if (selected >= 0) {
		const rtlight_t *l = &lights[selected];
		char text[512];
		snprintf(text, sizeof(text),
		         "light %d of %d%s\norigin %g %g %g\nradius %g\ncolour %g %g %g\nstyle %d\nshadows %s  corona %g\n"
		         "ambient %g diffuse %g specular %g",
		         selected + 1, numlights, locked ? " (locked)" : "", l->origin[0], l->origin[1], l->origin[2], l->radius,
		         l->color[0], l->color[1], l->color[2], l->style, l->shadows ? "yes" : "no", l->corona, l->ambient,
		         l->diffuse, l->specular);
		R_DrawFill(4, 40, 220, 64, back, 0.5f, 0);
		float y = 42;
		for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n"), y += 8)
			R_DrawString(6, y, line, 8, 8, white, 1, 0, false);
	}
}

/* --- commands ------------------------------------------------------------------------------- */

static rtlight_t *current(void)
{
	if (selected < 0 || selected >= numlights) {
		Con_Printf("No selected light\n");
		return NULL;
	}
	return &lights[selected];
}

static bool editing(void)
{
	if (!r_editlights->ivalue) {
		Con_Printf("Cannot edit lights when not in editing mode (r_editlights 1)\n");
		return false;
	}
	return true;
}

static void Spawn_f(int argc, const char **argv)
{
	rtlight_t *l;
	if (!editing())
		return;
	if (!(l = add_light())) {
		Con_Printf("Too many lights\n");
		return;
	}
	cursor_point(l->origin);
	selected = numlights - 1;
}

static void Clone_f(int argc, const char **argv)
{
	rtlight_t *src = current(), *l;
	if (!src || !editing())
		return;
	rtlight_t copy = *src;
	if (!(l = add_light())) {
		Con_Printf("Too many lights\n");
		return;
	}
	*l = copy;
	cursor_point(l->origin);
	selected = numlights - 1;
}

static void Remove_f(int argc, const char **argv)
{
	if (!current())
		return;
	lights[selected] = lights[--numlights];
	selected = -1;
	locked = false;
}

static void print_light(const rtlight_t *l)
{
	const char *ss = l->style >= 0 && l->style < 256 && cl.lightstyles[l->style] ? cl.lightstyles[l->style] : "---";
	Con_Printf("Selected light's properties:\n");
	Con_Printf("Origin       : %f %f %f\n", l->origin[0], l->origin[1], l->origin[2]);
	Con_Printf("Angles       : %f %f %f\n", l->angles[0], l->angles[1], l->angles[2]);
	Con_Printf("Colour       : %f %f %f\n", l->color[0], l->color[1], l->color[2]);
	Con_Printf("Radius       : %f\n", l->radius);
	Con_Printf("Corona       : %f\n", l->corona);
	Con_Printf("Style        : %d\n", l->style);
	Con_Printf("Style String : %s\n", *ss ? ss : "---");
	Con_Printf("Shadows      : %s\n", l->shadows ? "yes" : "no");
	Con_Printf("Cubemap      : \"%s\"\n", l->cubemap);
	Con_Printf("CoronaSize   : %f\n", l->coronasize);
	Con_Printf("Ambient      : %f\n", l->ambient);
	Con_Printf("Diffuse      : %f\n", l->diffuse);
	Con_Printf("Specular     : %f\n", l->specular);
	Con_Printf("NormalMode   : %s\n", l->normalmode ? "yes" : "no");
	Con_Printf("RealTimeMode : %s\n", l->realtimemode ? "yes" : "no");
	Con_Printf("Spin         : %f %f %f\n", l->spin[0], l->spin[1], l->spin[2]);
	Con_Printf("Cone         : %f\n", l->cone);
	Con_Printf("NearClip     : %f\n", l->nearclip);
}

static bool flag_arg(const char *s) { return !Str_ICmp(s, "yes") || !Str_ICmp(s, "on") || !Str_ICmp(s, "true") || atoi(s); }

/* one property from argv[1] on; false (and says why) if it isn't one */
static bool edit_light(rtlight_t *l, int argc, const char **argv)
{
	const char *p = argc > 1 ? argv[1] : "";
	float v[3] = {0, 0, 0};
	for (int k = 0; k < 3 && k + 2 < argc; k++)
		v[k] = (float)atof(argv[k + 2]);
	bool three = argc >= 5, one = argc >= 3;
#define VEC(name, field)                                         \
	if (!Str_ICmp(p, name)) {                                    \
		if (!three)                                              \
			goto usage;                                          \
		memcpy(l->field, v, sizeof(v));                          \
		return true;                                             \
	}
#define AXIS(name, field, k, add)                                \
	if (!Str_ICmp(p, name)) {                                    \
		if (!one)                                                \
			goto usage;                                          \
		l->field[k] = (add) ? l->field[k] + v[0] : v[0];          \
		return true;                                             \
	}
#define NUM(name, field)                                         \
	if (!Str_ICmp(p, name)) {                                    \
		if (!one)                                                \
			goto usage;                                          \
		l->field = v[0];                                         \
		return true;                                             \
	}
#define FLAG(name, field)                                        \
	if (!Str_ICmp(p, name)) {                                    \
		if (!one)                                                \
			goto usage;                                          \
		l->field = flag_arg(argv[2]);                            \
		return true;                                             \
	}
	VEC("origin", origin) VEC("angles", angles) VEC("color", color) VEC("colour", color) VEC("spin", spin)
	AXIS("originx", origin, 0, 0) AXIS("originy", origin, 1, 0) AXIS("originz", origin, 2, 0)
	AXIS("movex", origin, 0, 1) AXIS("movey", origin, 1, 1) AXIS("movez", origin, 2, 1)
	AXIS("anglesx", angles, 0, 0) AXIS("anglesy", angles, 1, 0) AXIS("anglesz", angles, 2, 0)
	NUM("radius", radius) NUM("corona", corona) NUM("coronasize", coronasize) NUM("ambient", ambient)
	NUM("diffuse", diffuse) NUM("specular", specular) NUM("cone", cone) NUM("nearclip", nearclip)
	FLAG("shadows", shadows) FLAG("normalmode", normalmode) FLAG("realtimemode", realtimemode)
#undef VEC
#undef AXIS
#undef NUM
#undef FLAG
	if (!Str_ICmp(p, "move")) {
		if (!three)
			goto usage;
		for (int k = 0; k < 3; k++)
			l->origin[k] += v[k];
		return true;
	}
	if (!Str_ICmp(p, "colorscale") || !Str_ICmp(p, "radiusscale")) {
		if (!one)
			goto usage;
		if (p[0] == 'c' || p[0] == 'C')
			for (int k = 0; k < 3; k++)
				l->color[k] *= v[0];
		else
			l->radius *= v[0];
		return true;
	}
	if (!Str_ICmp(p, "style")) {
		if (!one)
			goto usage;
		l->style = atoi(argv[2]);
		return true;
	}
	if (!Str_ICmp(p, "cubemap")) {
		Str_Copy(l->cubemap, argc > 2 ? argv[2] : "", sizeof(l->cubemap));
		return true;
	}
	Con_Printf("Argument not known: %s\n", p);
	return false;
usage:
	Con_Printf("%s %s needs %s\n", argv[0], p, three ? "a value" : "values");
	return false;
}

static void Edit_f(int argc, const char **argv)
{
	rtlight_t *l = current();
	if (!l)
		return;
	if (argc < 2) {
		print_light(l);
		return;
	}
	edit_light(l, argc, argv);
}

static void EditAll_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Argument not known: \n");
		return;
	}
	for (int i = 0; i < numlights; i++) {
		float origin[3];
		memcpy(origin, lights[i].origin, sizeof(origin));
		if (!edit_light(&lights[i], argc, argv))
			return;
		/* moving them all to one place would be no use */
		if (!Str_ICmp(argv[1], "origin"))
			memcpy(lights[i].origin, origin, sizeof(origin));
	}
}

static void ToggleShadow_f(int argc, const char **argv)
{
	rtlight_t *l = current();
	if (l)
		l->shadows = !l->shadows;
}

static void ToggleCorona_f(int argc, const char **argv)
{
	rtlight_t *l = current();
	if (l)
		l->corona = l->corona > 0 ? 0 : 1;
}

static void CopyInfo_f(int argc, const char **argv)
{
	rtlight_t *l = current();
	if (!l)
		return;
	clipboard = *l;
	haveclipboard = true;
}

static void PasteInfo_f(int argc, const char **argv)
{
	rtlight_t *l = current();
	if (!l)
		return;
	if (!haveclipboard) {
		Con_Printf("Nothing copied (r_editlights_copyinfo)\n");
		return;
	}
	float origin[3];
	memcpy(origin, l->origin, sizeof(origin));
	*l = clipboard;
	memcpy(l->origin, origin, sizeof(origin));
}

static void Lock_f(int argc, const char **argv)
{
	if (locked)
		locked = false;
	else if (current())
		locked = true;
}

static void Save_f(int argc, const char **argv)
{
	if (!cl.world) {
		Con_Printf("No map loaded\n");
		return;
	}
	save_rtlights();
}

static void Reload_f(int argc, const char **argv)
{
	const char *src = argc > 1 ? argv[1] : "";
	if (!cl.world) {
		Con_Printf("Cannot reload lights at this time\n");
		return;
	}
	if (src[0] && Str_ICmp(src, "rtlights") && Str_ICmp(src, "statics") && Str_ICmp(src, "bsp") && Str_ICmp(src, "none")) {
		Con_Printf("r_editlights_reload [rtlights|statics|bsp|none]\n");
		return;
	}
	reload(src);
	Con_Printf("%d lights\n", numlights);
}

/* the $r_editlights_current_* macros: the selected light's properties */
const char *R_EditLightsMacro(const char *name)
{
	static char buf[128];
	const char *p = !strncmp(name, "r_editlights_current_", 21) ? name + 21 : NULL;
	if (!p)
		return NULL;
	if (selected < 0 || selected >= numlights)
		return "";
	const rtlight_t *l = &lights[selected];
	if (!strcmp(p, "origin"))
		snprintf(buf, sizeof(buf), "%g %g %g", l->origin[0], l->origin[1], l->origin[2]);
	else if (!strcmp(p, "angles"))
		snprintf(buf, sizeof(buf), "%g %g %g", l->angles[0], l->angles[1], l->angles[2]);
	else if (!strcmp(p, "color"))
		snprintf(buf, sizeof(buf), "%g %g %g", l->color[0], l->color[1], l->color[2]);
	else if (!strcmp(p, "radius"))
		snprintf(buf, sizeof(buf), "%g", l->radius);
	else if (!strcmp(p, "corona"))
		snprintf(buf, sizeof(buf), "%g", l->corona);
	else if (!strcmp(p, "coronasize"))
		snprintf(buf, sizeof(buf), "%g", l->coronasize);
	else if (!strcmp(p, "style"))
		snprintf(buf, sizeof(buf), "%d", l->style);
	else if (!strcmp(p, "shadows"))
		snprintf(buf, sizeof(buf), "%d", l->shadows ? 1 : 0);
	else if (!strcmp(p, "cubemap"))
		snprintf(buf, sizeof(buf), "%s", l->cubemap);
	else if (!strcmp(p, "ambient"))
		snprintf(buf, sizeof(buf), "%g", l->ambient);
	else if (!strcmp(p, "diffuse"))
		snprintf(buf, sizeof(buf), "%g", l->diffuse);
	else if (!strcmp(p, "specular"))
		snprintf(buf, sizeof(buf), "%g", l->specular);
	else if (!strcmp(p, "normalmode"))
		snprintf(buf, sizeof(buf), "%d", l->normalmode ? 1 : 0);
	else if (!strcmp(p, "realtimemode"))
		snprintf(buf, sizeof(buf), "%d", l->realtimemode ? 1 : 0);
	else
		return NULL;
	return buf;
}

void R_InitRTLights(void)
{
	r_editlights = Cvar_Get("r_editlights", "0", 0, "Show the map's realtime lights and select the one under the crosshair for the r_editlights_* commands");
	r_editlights_cursordistance = Cvar_Get("r_editlights_cursordistance", "1024", 0, "How far the editing cursor reaches");
	r_editlights_cursorpushback = Cvar_Get("r_editlights_cursorpushback", "0", 0, "How far the cursor is pulled back towards the view");
	r_editlights_cursorpushoff = Cvar_Get("r_editlights_cursorpushoff", "4", 0, "How far the cursor is pushed off the surface it hits");
	r_editlights_cursorgrid = Cvar_Get("r_editlights_cursorgrid", "1", 0, "Snaps the cursor to a grid this size");
	r_editlights_import_radius = Cvar_Get("r_editlights_import_radius", "1", 0, "Scales the radius of lights imported from the map's entities");
	r_editlights_import_ambient = Cvar_Get("r_editlights_import_ambient", "0", 0, "The ambient level of lights imported from the map");
	r_editlights_import_diffuse = Cvar_Get("r_editlights_import_diffuse", "1", 0, "The diffuse level of lights imported from the map");
	r_editlights_import_specular = Cvar_Get("r_editlights_import_specular", "1", 0, "The specular level of lights imported from the map");
	r_shadow_realtime_world = Cvar_Get("r_shadow_realtime_world", "0", CVAR_ARCHIVE, "Light the scene with the map's static realtime lights");
	/* Bulwark always lights per pixel; this is here for the games that read
	 * it, such as FreeHL's flashlight, which picks its light by it */
	Cvar_Get("r_shadow_realtime_dlight", "1", CVAR_ARCHIVE, "Light the scene with dynamic lights per pixel (games read this; Bulwark always does)");
	r_shadow_realtime_world_import = Cvar_Get("r_shadow_realtime_world_importlightentitiesfrommap", "0", 0,
	                                          "1: take the static lights from the map's light entities, not its .rtlights");
	r_coronas = Cvar_Get("r_coronas", "0", CVAR_ARCHIVE, "Draw the glows of the lights that have a corona");
	r_coronas_intensity = Cvar_Get("r_coronas_intensity", "1", CVAR_ARCHIVE, "Scales the coronas' brightness");
	Cmd_AddD("r_editlights_spawn", Spawn_f, "Spawn a new light with default properties");
	Cmd_AddD("r_editlights_clone", Clone_f, "Duplicate the current light (with a new origin)");
	Cmd_AddD("r_editlights_remove", Remove_f, "Removes the current light.");
	Cmd_AddD("r_editlights_edit", Edit_f, "Changes named properties on the current light.");
	Cmd_AddD("r_editlights_editall", EditAll_f, "Like r_editlights_edit, but affects all lights instead of just the selected one.");
	Cmd_AddD("r_editlights_toggleshadow", ToggleShadow_f, "Toggles the shadow flag on the current light.");
	Cmd_AddD("r_editlights_togglecorona", ToggleCorona_f, "Toggles the current light's corona field.");
	Cmd_AddD("r_editlights_copyinfo", CopyInfo_f, "store a copy of all properties (except origin) of the selected light");
	Cmd_AddD("r_editlights_pasteinfo", PasteInfo_f, "apply the stored properties onto the selected light (making it exactly identical except for origin)");
	Cmd_AddD("r_editlights_lock", Lock_f, "Blocks changing the current light according the crosshair.");
	Cmd_AddD("r_editlights_save", Save_f, "Saves rtlights to maps/FOO.rtlights");
	Cmd_AddD("r_editlights_reload", Reload_f, "Reload static rtlights. Argument can be rtlights|statics|bsp|none to override the source.");
}

/* --- the game's own lights (dynamiclight_spawnstatic) -------------------------------------
 * Lights the client game makes and changes as it likes (Nuclide's
 * light_dynamic, point_spotlight, beams). They light the scene like
 * dynamic lights, with their style, until the map changes. Fields are
 * fteextensions.qc's LFIELD_* numbers; the ones the renderer has no use for
 * yet (cubemaps, cones, specular) are kept so dynamiclight_get returns them. */

#define MAX_QCLIGHTS 256

enum {
	LF_ORIGIN, LF_COLOUR, LF_RADIUS, LF_FLAGS, LF_STYLE, LF_ANGLES, LF_FOV, LF_CORONA, LF_CORONASCALE,
	LF_CUBEMAPNAME, LF_AMBIENTSCALE, LF_DIFFUSESCALE, LF_SPECULARSCALE, LF_ROTATION, LF_DIETIME, LF_RGBDECAY,
	LF_RADIUSDECAY, LF_STYLESTRING, LF_NEARCLIP, LF_OWNER
};

typedef struct {
	bool used;
	float origin[3], color[3], angles[3], rotation[3], rgbdecay[3];
	float radius, fov, corona, coronascale, ambient, diffuse, specular, dietime, radiusdecay, nearclip;
	int flags, style, owner;
	char cubemap[64], stylestring[64];
} qclight_t;

static qclight_t qclights[MAX_QCLIGHTS];

static void clear_qclights(void) { memset(qclights, 0, sizeof(qclights)); }

static float qclight_style(const qclight_t *l)
{
	const char *s = l->stylestring[0] ? l->stylestring : NULL;
	size_t n;
	if (!s)
		return style_value(l->style);
	n = strlen(s);
	return (float)(s[(size_t)(cl.time * 10) % n] - 'a') / ('m' - 'a');
}

void R_AddQCLights(void)
{
	for (int i = 0; i < MAX_QCLIGHTS && r_scene.numdlights < R_MAX_DLIGHTS; i++) {
		qclight_t *l = &qclights[i];
		float c[3], v;
		if (!l->used || l->radius <= 0)
			continue;
		if (l->dietime > 0 && cl.time >= l->dietime) {
			l->used = false;
			continue;
		}
		v = qclight_style(l) * (l->diffuse > 0 ? l->diffuse : 1);
		for (int k = 0; k < 3; k++)
			c[k] = l->color[k] * v;
		R_AddDynamicLight(l->origin, l->radius, c);
	}
}

static void bi_dynamiclight_spawnstatic(qcvm_t *vm)
{
	for (int i = 0; i < MAX_QCLIGHTS; i++) {
		qclight_t *l = &qclights[i];
		if (l->used)
			continue;
		memset(l, 0, sizeof(*l));
		l->used = true;
		VectorCopy(QC_PVEC(vm, 0), l->origin);
		l->radius = QC_PFLOAT(vm, 1);
		VectorCopy(QC_PVEC(vm, 2), l->color);
		l->diffuse = 1;
		l->coronascale = 1;
		QC_ReturnFloat(vm, (float)i);
		return;
	}
	QC_ReturnFloat(vm, -1);
}

static qclight_t *qclight_arg(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	return i >= 0 && i < MAX_QCLIGHTS && qclights[i].used ? &qclights[i] : NULL;
}

static void bi_dynamiclight_set(qcvm_t *vm)
{
	qclight_t *l = qclight_arg(vm);
	const float *v = QC_PVEC(vm, 2);
	float f = v[0];
	if (!l)
		return;
	switch ((int)QC_PFLOAT(vm, 1)) {
	case LF_ORIGIN: VectorCopy(v, l->origin); break;
	case LF_COLOUR: VectorCopy(v, l->color); break;
	case LF_RADIUS: l->radius = f; break;
	case LF_FLAGS: l->flags = (int)f; break;
	case LF_STYLE: l->style = (int)f; break;
	case LF_ANGLES: VectorCopy(v, l->angles); break;
	case LF_FOV: l->fov = f; break;
	case LF_CORONA: l->corona = f; break;
	case LF_CORONASCALE: l->coronascale = f; break;
	case LF_CUBEMAPNAME: Str_Copy(l->cubemap, QC_PSTR(vm, 2), sizeof(l->cubemap)); break;
	case LF_AMBIENTSCALE: l->ambient = f; break;
	case LF_DIFFUSESCALE: l->diffuse = f; break;
	case LF_SPECULARSCALE: l->specular = f; break;
	case LF_ROTATION: VectorCopy(v, l->rotation); break;
	case LF_DIETIME: l->dietime = f; break;
	case LF_RGBDECAY: VectorCopy(v, l->rgbdecay); break;
	case LF_RADIUSDECAY: l->radiusdecay = f; break;
	case LF_STYLESTRING: Str_Copy(l->stylestring, QC_PSTR(vm, 2), sizeof(l->stylestring)); break;
	case LF_NEARCLIP: l->nearclip = f; break;
	case LF_OWNER: l->owner = QC_PINT(vm, 2); break;
	default: break;
	}
}

static void bi_dynamiclight_get(qcvm_t *vm)
{
	qclight_t *l = qclight_arg(vm);
	float zero[3] = {0, 0, 0};
	if (!l) {
		QC_ReturnVector(vm, zero);
		return;
	}
	switch ((int)QC_PFLOAT(vm, 1)) {
	case LF_ORIGIN: QC_ReturnVector(vm, l->origin); break;
	case LF_COLOUR: QC_ReturnVector(vm, l->color); break;
	case LF_RADIUS: QC_ReturnFloat(vm, l->radius); break;
	case LF_FLAGS: QC_ReturnFloat(vm, (float)l->flags); break;
	case LF_STYLE: QC_ReturnFloat(vm, (float)l->style); break;
	case LF_ANGLES: QC_ReturnVector(vm, l->angles); break;
	case LF_FOV: QC_ReturnFloat(vm, l->fov); break;
	case LF_CORONA: QC_ReturnFloat(vm, l->corona); break;
	case LF_CORONASCALE: QC_ReturnFloat(vm, l->coronascale); break;
	case LF_CUBEMAPNAME: QC_ReturnString(vm, l->cubemap); break;
	case LF_AMBIENTSCALE: QC_ReturnFloat(vm, l->ambient); break;
	case LF_DIFFUSESCALE: QC_ReturnFloat(vm, l->diffuse); break;
	case LF_SPECULARSCALE: QC_ReturnFloat(vm, l->specular); break;
	case LF_ROTATION: QC_ReturnVector(vm, l->rotation); break;
	case LF_DIETIME: QC_ReturnFloat(vm, l->dietime); break;
	case LF_RGBDECAY: QC_ReturnVector(vm, l->rgbdecay); break;
	case LF_RADIUSDECAY: QC_ReturnFloat(vm, l->radiusdecay); break;
	case LF_STYLESTRING: QC_ReturnString(vm, l->stylestring); break;
	case LF_NEARCLIP: QC_ReturnFloat(vm, l->nearclip); break;
	case LF_OWNER: QC_ReturnInt(vm, l->owner); break;
	default: QC_ReturnVector(vm, zero); break;
	}
}

const qc_builtin_t r_qclight_builtins[] = {
	{"dynamiclight_spawnstatic", bi_dynamiclight_spawnstatic}, {"dynamiclight_set", bi_dynamiclight_set},
	{"dynamiclight_get", bi_dynamiclight_get}, {NULL, NULL}};
