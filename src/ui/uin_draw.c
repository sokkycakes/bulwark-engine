/* Bulwark engine - native UI: the 2D batch and its primitives. Every
 * primitive is quads into one static vertex buffer drawn with
 * R_Draw2DTriangles; the index pattern (0 1 2, 0 2 3 per quad) is built
 * once. No heap, no snprintf. See uin_local.h. ISC licence, see LICENSE. */
#include "uin_local.h"

#define MAX_QUADS 512
#define UIN_PI 3.14159265f
#define MAX_VERTS (MAX_QUADS * 4)

static r2dvert_t verts[MAX_VERTS];
static uint16_t idx[MAX_QUADS * 6];
static int nverts;
static int cur_image = -1, cur_blend = R2D_ALPHA, want_blend = R2D_ALPHA;
static uin_xf_t xf = {0, 0, 1, 1};
static int frame_verts, frame_calls;
static bool clipped;

#define XX(x) (xf.ox + (x) * xf.sx)
#define XY(y) (xf.oy + (y) * xf.sy)

uint32_t uin_col_alpha(uint32_t rgba, float mul)
{
	float a = (rgba & 0xff) * BW_CLAMP(mul, 0.0f, 1.0f);
	return (rgba & 0xffffff00u) | (uint32_t)(a + 0.5f);
}

/* --- transforms ---------------------------------------------------------------------- */

void uin_xf_stretch(uin_xf_t *t, float dw, float dh, float x, float y, float w, float h)
{
	t->ox = x;
	t->oy = y;
	t->sx = w / dw;
	t->sy = h / dh;
}

void uin_xf_fit(uin_xf_t *t, float dw, float dh, float x, float y, float w, float h)
{
	float s = BW_MIN(w / dw, h / dh);
	t->sx = t->sy = s;
	t->ox = x + (w - dw * s) * 0.5f;
	t->oy = y + (h - dh * s) * 0.5f;
}

void d_xf(const uin_xf_t *t)
{
	static const uin_xf_t identity = {0, 0, 1, 1};
	xf = t ? *t : identity;
}

const uin_xf_t *d_get_xf(void) { return &xf; }

/* --- the batch ---------------------------------------------------------------------- */

void d_flush(void)
{
	if (nverts > 0 && cur_image > 0) {
		R_Draw2DTriangles(cur_image, verts, nverts, idx, nverts / 4 * 6, cur_blend);
		frame_verts += nverts;
		frame_calls++;
	}
	nverts = 0;
}

void d_blend(int blend) { want_blend = blend; }

/* room for n quads drawn from image: flushes on a change or when full */
static r2dvert_t *reserve(int image, int nquads)
{
	r2dvert_t *v;
	if (image != cur_image || want_blend != cur_blend || nverts + nquads * 4 > MAX_VERTS) {
		d_flush();
		cur_image = image;
		cur_blend = want_blend;
		if (!idx[1]) /* first use: the shared index pattern */
			for (int q = 0; q < MAX_QUADS; q++) {
				uint16_t b = (uint16_t)(q * 4);
				uint16_t *p = idx + q * 6;
				p[0] = b, p[1] = (uint16_t)(b + 1), p[2] = (uint16_t)(b + 2);
				p[3] = b, p[4] = (uint16_t)(b + 2), p[5] = (uint16_t)(b + 3);
			}
	}
	v = verts + nverts;
	nverts += nquads * 4;
	return v;
}

static void put(r2dvert_t *v, float x, float y, float s, float t, uint32_t c)
{
	v->x = x;
	v->y = y;
	v->s = s;
	v->t = t;
	v->rgba[0] = (uint8_t)(c >> 24);
	v->rgba[1] = (uint8_t)(c >> 16);
	v->rgba[2] = (uint8_t)(c >> 8);
	v->rgba[3] = (uint8_t)c;
}

/* fills and shapes take the white block of the page already batched, so
 * they don't split a run of glyphs into two draws */
static int fill_page(void) { return cur_image > 0 && cur_image == uin_atlas_image(1) ? 1 : 0; }

static void white_quad(const float *x, const float *y, const uint32_t *c)
{
	int page = fill_page();
	float s, t;
	r2dvert_t *v;
	uin_white_uv(page, &s, &t);
	v = reserve(uin_atlas_image(page), 1);
	for (int i = 0; i < 4; i++)
		put(v + i, x[i], y[i], s, t, c[i]);
}

void d_quad4(const float *xy, const uint32_t *rgba4)
{
	float x[4], y[4];
	for (int i = 0; i < 4; i++) {
		x[i] = XX(xy[i * 2]);
		y[i] = XY(xy[i * 2 + 1]);
	}
	white_quad(x, y, rgba4);
}

static void rect4(float x, float y, float w, float h, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
	float x0 = XX(x), y0 = XY(y), x1 = XX(x + w), y1 = XY(y + h);
	float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
	uint32_t c[4] = {c0, c1, c2, c3};
	if (w <= 0 || h <= 0)
		return;
	white_quad(xs, ys, c);
}

void d_fill(float x, float y, float w, float h, uint32_t rgba) { rect4(x, y, w, h, rgba, rgba, rgba, rgba); }
void d_grad_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom) { rect4(x, y, w, h, top, top, bottom, bottom); }
void d_grad_h(float x, float y, float w, float h, uint32_t left, uint32_t right) { rect4(x, y, w, h, left, right, right, left); }

/* --- images ----------------------------------------------------------------------------- */

static void tex_quad(int image, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, uint32_t c)
{
	r2dvert_t *v = reserve(image, 1);
	put(v, x0, y0, s0, t0, c);
	put(v + 1, x1, y0, s1, t0, c);
	put(v + 2, x1, y1, s1, t1, c);
	put(v + 3, x0, y1, s0, t1, c);
}

void d_img_uv(int region, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba)
{
	const uin_region_t *r = uin_region(region);
	float ds, dt;
	if (!r || !r->ok || w <= 0 || h <= 0)
		return;
	ds = r->s1 - r->s0;
	dt = r->t1 - r->t0;
	tex_quad(uin_atlas_image(r->page), XX(x), XY(y), XX(x + w), XY(y + h), r->s0 + ds * u0, r->t0 + dt * v0,
	         r->s0 + ds * u1, r->t0 + dt * v1, rgba);
}

void d_img(int region, float x, float y, float w, float h, uint32_t rgba)
{
	d_img_uv(region, x, y, w, h, 0, 0, 1, 1, rgba);
}

void d_img_rot(int region, float cx, float cy, float w, float h, float deg, uint32_t rgba)
{
	const uin_region_t *r = uin_region(region);
	float a = deg * (UIN_PI / 180), c = cosf(a), s = sinf(a);
	float hx = w * 0.5f * xf.sx, hy = h * 0.5f * xf.sy, px = XX(cx), py = XY(cy);
	static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	r2dvert_t *v;
	if (!r || !r->ok)
		return;
	v = reserve(uin_atlas_image(r->page), 1);
	for (int i = 0; i < 4; i++) {
		float lx = corner[i][0] * hx, ly = corner[i][1] * hy;
		put(v + i, px + lx * c - ly * s, py + lx * s + ly * c, corner[i][0] < 0 ? r->s0 : r->s1,
		    corner[i][1] < 0 ? r->t0 : r->t1, rgba);
	}
}

void d_nine(int region, float x, float y, float w, float h, float l, float t, float rr, float b, uint32_t rgba)
{
	const uin_region_t *r = uin_region(region);
	float xs[4], ys[4], us[4], vs[4];
	if (!r || !r->ok || r->w <= 0 || r->h <= 0)
		return;
	xs[0] = x, xs[1] = x + l, xs[2] = x + w - rr, xs[3] = x + w;
	ys[0] = y, ys[1] = y + t, ys[2] = y + h - b, ys[3] = y + h;
	us[0] = 0, us[1] = l / r->w, us[2] = 1 - rr / r->w, us[3] = 1;
	vs[0] = 0, vs[1] = t / r->h, vs[2] = 1 - b / r->h, vs[3] = 1;
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			if (xs[i + 1] > xs[i] && ys[j + 1] > ys[j])
				d_img_uv(region, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], us[i], vs[j], us[i + 1], vs[j + 1],
				         rgba);
}

void d_pic(int handle, float x, float y, float w, float h, float s0, float t0, float s1, float t1, uint32_t rgba)
{
	if (handle <= 0 || w <= 0 || h <= 0)
		return;
	tex_quad(handle, XX(x), XY(y), XX(x + w), XY(y + h), s0, t0, s1, t1, rgba);
}

void d_annulus(float cx, float cy, float r0, float r1, int segs, uint32_t rgba)
{
	float px = XX(cx), py = XY(cy);
	uint32_t c[4] = {rgba, rgba, rgba, rgba};
	float pc = 1, ps = 0;
	segs = BW_CLAMP(segs, 3, MAX_QUADS);
	for (int i = 1; i <= segs; i++) {
		float a = (2 * UIN_PI) * i / segs, nc = cosf(a), ns = sinf(a);
		float x[4] = {px + pc * r0 * xf.sx, px + pc * r1 * xf.sx, px + nc * r1 * xf.sx, px + nc * r0 * xf.sx};
		float y[4] = {py + ps * r0 * xf.sy, py + ps * r1 * xf.sy, py + ns * r1 * xf.sy, py + ns * r0 * xf.sy};
		white_quad(x, y, c);
		pc = nc;
		ps = ns;
	}
}

/* --- text ---------------------------------------------------------------------------- */

/* the next code point of UTF-8 text; bytes that aren't UTF-8 stand for
 * themselves (Latin-1 names) */
static int next_cp(const char **sp)
{
	const unsigned char *s = (const unsigned char *)*sp;
	int cp = *s++;
	if (cp >= 0xc0 && cp < 0xe0 && (s[0] & 0xc0) == 0x80) {
		cp = ((cp & 0x1f) << 6) | (s[0] & 0x3f);
		s++;
	} else if (cp >= 0xe0 && cp < 0xf0 && (s[0] & 0xc0) == 0x80 && (s[1] & 0xc0) == 0x80) {
		cp = ((cp & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
		s += 2;
	}
	*sp = (const char *)s;
	return cp;
}

static float snap(float v)
{
	float sc = R_Get2DScale();
	if (sc <= 0)
		sc = 1;
	return floorf(v * sc + 0.5f) / sc;
}

/* the engine's built-in font, when a face has nothing baked at all (no
 * file, no fallback): a second batch, but text still shows */
static bool fallback_face(int face, float *size)
{
	const uin_face_t *f = uin_face(face);
	if (f && f->ok)
		return false;
	*size = f && f->px >= 8 ? floorf(f->px / 8) * 8 : 8;
	return true;
}

float d_text_width(int face, const char *s, float tracking)
{
	float w = 0, size;
	int prev = 0;
	if (!s)
		return 0;
	if (fallback_face(face, &size))
		return R_TextWidth(s, size);
	while (*s) {
		int cp = next_cp(&s);
		const uin_glyph_t *g = uin_glyph(face, cp);
		if (!g)
			continue;
		if (prev)
			w += uin_kern(face, prev, cp);
		w += g->adv64 / 64.0f + tracking;
		prev = cp;
	}
	return w;
}

float d_text_ex(int face, float x, float y, const char *s, int align, uint32_t rgba, float tracking, float rot)
{
	const uin_face_t *f = uin_face(face);
	float width, size, ax = XX(x), ay = XY(y), pen = 0, base = 0, c = 1, sn = 0;
	int prev = 0, img;
	if (!s || !*s)
		return 0;
	width = d_text_width(face, s, tracking);
	if (fallback_face(face, &size)) {
		float rgb[3] = {(rgba >> 24) / 255.0f, ((rgba >> 16) & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f};
		float top = ay - ((align & 12) == UIN_MIDDLE ? size / 2 : (align & 12) >= UIN_BASELINE ? size : 0);
		d_flush();
		R_Text(ax - ((align & 3) == UIN_CENTER ? width / 2 : (align & 3) == UIN_RIGHT ? width : 0), top, s, size, rgb,
		       (rgba & 255) / 255.0f);
		return width;
	}
	/* where the baseline starts, along the line (pen) and across it (base) */
	if ((align & 3) == UIN_CENTER)
		pen = -width / 2;
	else if ((align & 3) == UIN_RIGHT)
		pen = -width;
	switch (align & 12) {
	case UIN_TOP: base = f->ascent; break;
	case UIN_MIDDLE: base = (f->ascent - f->descent) / 2; break;
	case UIN_BOTTOM: base = -f->descent; break;
	default: break;
	}
	if (rot) {
		float a = rot * (UIN_PI / 180);
		c = cosf(a);
		sn = sinf(a);
	} else {
		/* unrotated text lands on whole pixels: the pixel faces stay crisp */
		ax = snap(ax + pen);
		ay = snap(ay + base);
		pen = base = 0;
	}
	img = uin_atlas_image(f->page);
	{
		int pw, ph;
		uin_atlas_page_size(f->page, &pw, &ph);
		while (*s) {
			int cp = next_cp(&s);
			const uin_glyph_t *g = uin_glyph(face, cp);
			if (!g)
				continue;
			if (prev)
				pen += uin_kern(face, prev, cp);
			prev = cp;
			if (g->w && g->h) {
				float lx0 = pen + g->xoff, ly0 = base + g->yoff, lx1 = lx0 + g->w, ly1 = ly0 + g->h;
				float s0 = (float)g->x / pw, t0 = (float)g->y / ph, s1 = (float)(g->x + g->w) / pw,
				      t1 = (float)(g->y + g->h) / ph;
				if (!rot) {
					/* the pen moves in fractions: each glyph's own corner snaps */
					float gx = snap(ax + lx0);
					tex_quad(img, gx, ay + ly0, gx + g->w, ay + ly1, s0, t0, s1, t1, rgba);
				} else {
					r2dvert_t *v = reserve(img, 1);
					float lx[4] = {lx0, lx1, lx1, lx0}, ly[4] = {ly0, ly0, ly1, ly1};
					float ss[4] = {s0, s1, s1, s0}, tt[4] = {t0, t0, t1, t1};
					for (int i = 0; i < 4; i++)
						put(v + i, ax + lx[i] * c - ly[i] * sn, ay + lx[i] * sn + ly[i] * c, ss[i], tt[i], rgba);
				}
			}
			pen += g->adv64 / 64.0f + tracking;
		}
	}
	return width;
}

float d_text(int face, float x, float y, const char *s, int align, uint32_t rgba)
{
	return d_text_ex(face, x, y, s, align, rgba, 0, 0);
}

const char *d_text_fit(int face, const char *s, float maxw, char *out, size_t outsize)
{
	size_t n;
	Str_Copy(out, s ? s : "", outsize);
	if (d_text_width(face, out, 0) <= maxw)
		return out;
	n = strlen(out);
	while (n > 0) {
		/* back off one code point, not one byte */
		do
			n--;
		while (n > 0 && ((unsigned char)out[n] & 0xc0) == 0x80);
		if (n + 4 > outsize)
			continue;
		memcpy(out + n, "...", 4);
		if (d_text_width(face, out, 0) <= maxw)
			return out;
	}
	Str_Copy(out, "...", outsize);
	return out;
}

/* --- clipping and counters ------------------------------------------------------ */

void d_clip(float x, float y, float w, float h)
{
	d_flush();
	R_Set2DClip(XX(x), XY(y), w * xf.sx, h * xf.sy);
	clipped = true;
}

void d_unclip(void)
{
	d_flush();
	if (clipped)
		R_Reset2DClip();
	clipped = false;
}

void d_counters(int *v, int *c)
{
	*v = frame_verts + nverts;
	*c = frame_calls + (nverts > 0);
}

void d_reset_counters(void) { frame_verts = frame_calls = 0; }
