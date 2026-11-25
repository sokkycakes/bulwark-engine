/* Bulwark engine - model shots: one model drawn on its own into an image.
 *
 * A shot is a named image (shown in 2D and by the UI as "$rt:name") that
 * holds a model posed in one of its animations, lit by a fixed studio
 * light on a transparent background. Live shots keep their texture as the
 * render target and are drawn again every frame they are asked for; still
 * shots are drawn once and read back (portraits, which can also be saved).
 *
 * Framing is found, not configured: the model is drawn small, the box its
 * pixels cover is measured, and the camera moves onto it. That copes with
 * models whose bounds say little about where the body is (Half-Life models
 * whose sequences move them off their origin, Quake ones standing on it).
 * Portraits frame the top of that box, centred on what is in it.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMINGS 32
#define MAX_TARGETS 32
#define FOV_Y 24.0f /* narrow, close to a long lens: little perspective */

typedef struct {
	char key[200]; /* model|anim|portrait|aspect */
	float center[3], dist;
	bool ok;
} framing_t;

typedef struct {
	char name[128];
	image_t *img;
	GLuint fb, depth;
	int w, h;
} target_t;

static framing_t framings[MAX_FRAMINGS];
static int numframings, nextframing;
static target_t targets[MAX_TARGETS];

/* an animation by name or number; "$MACRO" values (QC frame macros) and
 * names a model doesn't have fall back to the usual idle names, then 0 */
static int find_anim(rmodel_t *m, const char *anim)
{
	static const char *const idles[] = {"idle1", "idle", "stand1", "stand", "a00wait1.nuanmb"};
	int n = BW_MAX(1, R_ModelNumFrames(m)), f;
	if (anim && *anim && anim[0] != '$') {
		char *end;
		long v = strtol(anim, &end, 10);
		if (!*end)
			return (int)(((v % n) + n) % n);
		if ((f = R_FrameForName(m, anim)) >= 0)
			return f;
	}
	for (size_t i = 0; i < sizeof(idles) / sizeof(idles[0]); i++)
		if ((f = R_FrameForName(m, idles[i])) >= 0)
			return f;
	return 0;
}

/* Quake models have no sequences: their stand1, stand2 ... frames play at
 * ten a second */
static int alias_frame(rmodel_t *m, int frame, float time)
{
	const char *name = R_FrameToName(m, frame);
	char base[64];
	size_t len;
	int count = 1, n = R_ModelNumFrames(m);
	if (!m->alias || !name || !*name || R_FrameDuration(m, frame) > 0)
		return frame;
	Str_Copy(base, name, sizeof(base));
	len = strlen(base);
	while (len && base[len - 1] >= '0' && base[len - 1] <= '9')
		base[--len] = 0;
	if (!len || len == strlen(name))
		return frame;
	while (frame + count < n && !Str_NICmp(R_FrameToName(m, frame + count), base, (int)len))
		count++;
	return frame + (int)(time * 10) % count;
}

static target_t *get_target(const char *name, int w, int h, bool make)
{
	target_t *t = NULL, *free_slot = NULL;
	for (int i = 0; i < MAX_TARGETS; i++) {
		if (!Str_ICmp(targets[i].name, name)) {
			t = &targets[i];
			break;
		}
		if (!targets[i].name[0] && !free_slot)
			free_slot = &targets[i];
	}
	if (!t) {
		if (!make || !free_slot)
			return NULL;
		t = free_slot;
		Str_Copy(t->name, name, sizeof(t->name));
	}
	if (t->img && t->w == w && t->h == h)
		return t;
	{
		/* the image exists before it is first drawn, so a UI resolving
		 * "$rt:name" finds it: blank until then */
		uint8_t *clear = Mem_Alloc((size_t)w * h * 4);
		t->img = R_ImageFromPixels(name, clear, w, h, IMG_NOMIP | IMG_CLAMP);
		Mem_Free(clear);
	}
	t->img->flipy = true;
	t->img->alpha = true;
	t->w = w;
	t->h = h;
	if (!R_Active())
		return t;
	if (!t->fb)
		qglGenFramebuffers(1, &t->fb);
	if (!t->depth)
		qglGenRenderbuffers(1, &t->depth);
	qglBindRenderbuffer(GL_RENDERBUFFER, t->depth);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	return t;
}

/* draws m into the bound w x h target */
static void draw(rmodel_t *m, int frame, float time, float yaw, const float *center, float dist, int w, int h)
{
	float ang[3], f[3], r[3], u[3], zero[3] = {0, 0, 0};
	rentity_t *e;
	int pw = r_pwidth, ph = r_pheight;
	R_ClearScene();
	r_scene.isolated = true;
	r_scene.drawworld = false;
	r_scene.viewentity = -1;
	r_scene.vx = r_scene.vy = 0;
	r_scene.vw = (float)r_vwidth;
	r_scene.vh = (float)r_vheight;
	r_scene.fov_y = FOV_Y;
	r_scene.fov_x = atanf(tanf(FOV_Y * (float)M_PI / 360) * w / h) * 360 / (float)M_PI;
	r_scene.nearclip = BW_MAX(1.0f, dist * 0.05f);
	/* the camera looks at the model's front (models face +x) */
	ang[0] = 4;
	ang[1] = 180;
	ang[2] = 0;
	AngleVectors(ang, f, r, u);
	for (int k = 0; k < 3; k++)
		r_scene.origin[k] = center[k] - f[k] * dist;
	VectorCopy(ang, r_scene.angles);
	/* a key light from the upper left of the camera, and fill */
	r_scene.isolight[0][0] = 0.55f, r_scene.isolight[0][1] = 0.55f, r_scene.isolight[0][2] = 0.58f;
	r_scene.isolight[1][0] = 0.75f, r_scene.isolight[1][1] = 0.72f, r_scene.isolight[1][2] = 0.68f;
	for (int k = 0; k < 3; k++)
		r_scene.isolight[2][k] = -f[k] * 0.6f - r[k] * 0.5f + u[k] * 0.7f;
	VectorNormalize(r_scene.isolight[2]);
	if ((e = R_AllocEntity())) {
		float a[3] = {0, yaw, 0};
		e->model = m;
		e->frame = e->frame2 = alias_frame(m, frame, time);
		e->frame1time = e->frame2time = time;
		AnglesToAxis(a, e->axis);
		VectorCopy(zero, e->origin);
	}
	r_pwidth = w, r_pheight = h;
	qglDisable(GL_SCISSOR_TEST);
	qglViewport(0, 0, w, h);
	R_RenderScene();
	r_pwidth = pw, r_pheight = ph;
	R_ClearScene();
}

/* the box (pixels from the top left) the drawn model covers, from the
 * bound target's alpha; false when nothing was drawn */
static bool measure(int w, int h, int box[4], int *topcx)
{
	uint8_t *px = Mem_Alloc((size_t)w * h * 4);
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	qglPixelStorei(GL_PACK_ALIGNMENT, 1);
	qglReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (px[((size_t)y * w + x) * 4 + 3] > 8) {
				int ty = h - 1 - y;
				x0 = BW_MIN(x0, x), x1 = BW_MAX(x1, x);
				y0 = BW_MIN(y0, ty), y1 = BW_MAX(y1, ty);
			}
	if (x1 >= 0 && topcx) {
		/* the middle of what is in the top fifth: the head, not the
		 * centre of a box an outstretched arm widens */
		int lim = y0 + (y1 - y0) / 5, cx0 = w, cx1 = -1;
		for (int y = 0; y < h; y++) {
			int ty = h - 1 - y;
			if (ty < y0 || ty > lim)
				continue;
			for (int x = 0; x < w; x++)
				if (px[((size_t)y * w + x) * 4 + 3] > 8)
					cx0 = BW_MIN(cx0, x), cx1 = BW_MAX(cx1, x);
		}
		*topcx = cx1 >= 0 ? (cx0 + cx1) / 2 : (x0 + x1) / 2;
	}
	Mem_Free(px);
	box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
	return x1 >= 0;
}

/* moves the camera so pixels [x0..x1] x [y0..y1] fill fill of the view */
static void reframe(float *center, float *dist, int w, int h, float x0, float y0, float x1, float y1, float fill)
{
	float ang[3] = {4, 180, 0}, f[3], r[3], u[3];
	float ty = tanf(FOV_Y * (float)M_PI / 360), tx = ty * w / h;
	float cx = (x0 + x1 + 1) / w - 1, cy = 1 - (y0 + y1 + 1) / h;
	float sh = (y1 - y0 + 1) / (float)h, sw = (x1 - x0 + 1) / (float)w;
	AngleVectors(ang, f, r, u);
	for (int k = 0; k < 3; k++)
		center[k] += r[k] * cx * tx * *dist + u[k] * cy * ty * *dist;
	*dist *= BW_MAX(sh, sw) / fill;
}

static framing_t *find_framing(rmodel_t *m, const char *anim, int frame, float yaw, bool portrait, int w, int h)
{
	char key[200];
	framing_t *fr;
	snprintf(key, sizeof(key), "%s|%s|%d|%.0f|%.3f", m->name, anim ? anim : "", portrait, yaw, (float)w / h);
	for (int i = 0; i < numframings; i++)
		if (!strcmp(framings[i].key, key))
			return &framings[i];
	if (numframings < MAX_FRAMINGS)
		fr = &framings[numframings++];
	else
		fr = &framings[nextframing++ % MAX_FRAMINGS];
	memset(fr, 0, sizeof(*fr));
	Str_Copy(fr->key, key, sizeof(fr->key));
	return fr;
}

/* measures where the model is, in a scratch target of the shot's shape */
static void frame_model(framing_t *fr, rmodel_t *m, int frame, float yaw, bool portrait, int w, int h)
{
	int mw = 160, mh = BW_MAX(16, 160 * h / w), box[4], topcx = 0;
	GLuint fb, rb[2];
	float center[3], dist;
	for (int k = 0; k < 3; k++)
		center[k] = (m->mins[k] + m->maxs[k]) * 0.5f;
	dist = BW_MAX(32.0f, m->radius) * 8;
	qglGenFramebuffers(1, &fb);
	qglGenRenderbuffers(2, rb);
	qglBindFramebuffer(GL_FRAMEBUFFER, fb);
	qglBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, mw, mh);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	qglBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, mw, mh);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	fr->ok = false;
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		/* far away first, then closer as the box is found; a box touching
		 * the edge means part of the model is outside: step back */
		for (int pass = 0; pass < 6; pass++) {
			draw(m, frame, 0, yaw, center, dist, mw, mh);
			if (!measure(mw, mh, box, &topcx)) {
				dist *= 2;
				continue;
			}
			if (box[0] <= 0 || box[1] <= 0 || box[2] >= mw - 1 || box[3] >= mh - 1) {
				reframe(center, &dist, mw, mh, (float)box[0], (float)box[1], (float)box[2], (float)box[3], 0.25f);
				continue;
			}
			fr->ok = true;
			if (pass >= 2)
				break;
			reframe(center, &dist, mw, mh, (float)box[0], (float)box[1], (float)box[2], (float)box[3], 0.6f);
		}
		if (fr->ok) {
			if (portrait) {
				/* head and shoulders: the top of the body, square on the head */
				float bh = (float)(box[3] - box[1] + 1), half = bh * 0.21f * w / h;
				reframe(center, &dist, mw, mh, topcx - half, box[1] - bh * 0.04f, topcx + half, box[1] + bh * 0.38f, 1.0f);
			} else {
				reframe(center, &dist, mw, mh, (float)box[0], (float)box[1], (float)box[2], (float)box[3], 0.9f);
			}
		}
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	qglDeleteRenderbuffers(2, rb);
	qglDeleteFramebuffers(1, &fb);
	VectorCopy(center, fr->center);
	fr->dist = dist;
}

bool R_ModelShot(const char *name, const r_modelshot_t *s)
{
	rmodel_t *m;
	target_t *t;
	framing_t *fr;
	int frame;
	bool ok = false;
	if (!Str_NICmp(name, "$rt:", 4))
		name += 4;
	if (!name[0] || s->width < 8 || s->height < 8 || s->width > 2048 || s->height > 2048)
		return false;
	if (!(t = get_target(name, s->width, s->height, true)))
		return false;
	if (!R_Active() || !s->model || !s->model[0])
		return false;
	m = R_LoadModel(s->model);
	if (!m || m->failed || !m->loaded)
		return false;
	frame = find_anim(m, s->anim);
	R_Flush2D();
	fr = find_framing(m, s->anim, frame, s->yaw, s->portrait, s->width, s->height);
	if (!fr->dist)
		frame_model(fr, m, frame, s->yaw, s->portrait, s->width, s->height);
	qglBindFramebuffer(GL_FRAMEBUFFER, t->fb);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->img->tex, 0);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
	if (fr->ok && qglCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		draw(m, frame, s->time, s->yaw, fr->center, fr->dist, s->width, s->height);
		ok = true;
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	qglDisable(GL_SCISSOR_TEST);
	qglViewport(0, 0, r_pwidth, r_pheight);
	R_Begin2D(r_vwidth, r_vheight);
	return ok;
}

bool R_ModelShotExists(const char *name)
{
	if (!Str_NICmp(name, "$rt:", 4))
		name += 4;
	return get_target(name, 1, 1, false) != NULL;
}

/* modelshot <model> <file.png> [width] [height] [portrait] [anim] [yaw]:
 * writes a still to a file, for art and for checking framing */
static void ModelShot_f(int argc, const char **argv)
{
	r_modelshot_t s = {0};
	target_t *t;
	char path[1024];
	uint8_t *px, *rgb;
	strbuf_t out = {0};
	FILE *f;
	if (argc < 3) {
		Con_Printf("modelshot <model> <file.png> [width] [height] [portrait 0/1] [anim] [yaw]\n");
		return;
	}
	s.model = argv[1];
	s.width = argc > 3 ? atoi(argv[3]) : 512;
	s.height = argc > 4 ? atoi(argv[4]) : 512;
	s.portrait = argc > 5 && atoi(argv[5]);
	s.anim = argc > 6 ? argv[6] : NULL;
	s.yaw = argc > 7 ? (float)atof(argv[7]) : 0;
	if (!R_ModelShot("_modelshot", &s) || !(t = get_target("_modelshot", s.width, s.height, false))) {
		Con_Printf("modelshot: couldn't draw %s\n", argv[1]);
		return;
	}
	px = Mem_Alloc((size_t)s.width * s.height * 4);
	rgb = Mem_Alloc((size_t)s.width * s.height * 3);
	qglBindFramebuffer(GL_FRAMEBUFFER, t->fb);
	qglPixelStorei(GL_PACK_ALIGNMENT, 1);
	qglReadPixels(0, 0, s.width, s.height, GL_RGBA, GL_UNSIGNED_BYTE, px);
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* the file has no alpha: the model over the Stiletto grey */
	for (int i = 0; i < s.width * s.height; i++) {
		int a = px[i * 4 + 3];
		for (int k = 0; k < 3; k++)
			rgb[i * 3 + k] = (uint8_t)((px[i * 4 + k] * a + 0x33 * (255 - a)) / 255);
	}
	if (!R_EncodeImage(strrchr(argv[2], '.') ? strrchr(argv[2], '.') + 1 : "png", rgb, s.width, s.height, 92, &out) ||
	    !FS_WritePath(argv[2], path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("modelshot: can't write %s\n", argv[2]);
	} else {
		fwrite(out.data, 1, out.len, f);
		fclose(f);
		Con_Printf("Wrote %s\n", path);
	}
	SB_Free(&out);
	Mem_Free(px);
	Mem_Free(rgb);
}

void R_InitModelShots(void)
{
	Cmd_AddD("modelshot", ModelShot_f,
	         "Draws a model on its own into an image file: modelshot <model> <file.png> [width] [height] [portrait] [anim] [yaw]");
}
