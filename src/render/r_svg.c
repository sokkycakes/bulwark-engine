/* Bulwark engine - SVG images, drawn to pixels by nanosvg (zlib licence,
 * third_party/nanosvg). UI pages use SVG art; it is rasterised at twice its
 * own size, and small icons at 512 pixels across, so it stays sharp when a
 * 1080-line layout scales it up.
 * ISC licence, see LICENSE. */
#include "r_local.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

/* --- masks -------------------------------------------------------------------------
 * nanosvg has no <mask>. Bulwark's copy marks a mask's content with the mask's
 * id (NSVGshape.maskDef) and masked shapes with the id they use
 * (NSVGshape.mask). Each mask is rasterised on its own; shapes are drawn in
 * runs that share a mask, each run's alpha is multiplied by its mask, and the
 * runs are composited in document order. */

/* rasterises the shapes list[0..n) alone (nsvgRasterize clears the buffer) */
static void raster_run(NSVGrasterizer *rast, NSVGimage *img, NSVGshape **list, int n, float scale, uint8_t *dst,
                       int w, int h)
{
	for (int i = 0; i < n; i++)
		list[i]->next = i + 1 < n ? list[i + 1] : NULL;
	img->shapes = n ? list[0] : NULL;
	nsvgRasterize(rast, img, 0, 0, scale, dst, w, h, w * 4);
}

/* straight-alpha source-over of src onto dst, src alpha times cover (0..255) */
static void composite(uint8_t *dst, const uint8_t *src, const uint8_t *cover, size_t npix)
{
	for (size_t i = 0; i < npix; i++, dst += 4, src += 4) {
		float sa = src[3] / 255.0f * (cover ? cover[i] / 255.0f : 1);
		float da = dst[3] / 255.0f;
		float oa = sa + da * (1 - sa);
		if (sa <= 0)
			continue;
		for (int c = 0; c < 3; c++)
			dst[c] = (uint8_t)((src[c] * sa + dst[c] * da * (1 - sa)) / oa + 0.5f);
		dst[3] = (uint8_t)(oa * 255 + 0.5f);
	}
}

/* the shapes in document order, then drawn with their masks applied */
static void raster_masked(NSVGrasterizer *rast, NSVGimage *img, float scale, uint8_t *px, int w, int h)
{
	int n = 0, ndraw = 0;
	size_t npix = (size_t)w * h;
	NSVGshape **all, **draw, **run;
	uint8_t *layer, *cover;
	for (NSVGshape *sh = img->shapes; sh; sh = sh->next)
		n++;
	all = Mem_Alloc(sizeof(*all) * (size_t)n * 3);
	draw = all + n;
	run = draw + n;
	n = 0;
	for (NSVGshape *sh = img->shapes; sh; sh = sh->next) {
		all[n++] = sh;
		if (!sh->maskDef[0])
			draw[ndraw++] = sh;
	}
	layer = Mem_Alloc(npix * 4);
	cover = Mem_Alloc(npix);
	memset(px, 0, npix * 4);
	for (int i = 0; i < ndraw;) {
		const char *mask = draw[i]->mask;
		int j = i, nrun = 0, nmask = 0;
		bool alpha = false;
		while (j < ndraw && !strcmp(draw[j]->mask, mask))
			j++;
		if (mask[0]) {
			/* the mask's content alone; luminance times alpha, or alpha */
			for (int k = 0; k < n; k++)
				if (!strcmp(all[k]->maskDef, mask)) {
					run[nmask++] = all[k];
					alpha = all[k]->maskAlpha;
				}
			raster_run(rast, img, run, nmask, scale, layer, w, h);
			for (size_t q = 0; q < npix; q++) {
				const uint8_t *m = layer + q * 4;
				float lum = alpha ? 1 : (0.2125f * m[0] + 0.7154f * m[1] + 0.0721f * m[2]) / 255.0f;
				cover[q] = (uint8_t)(lum * m[3] + 0.5f);
			}
		}
		for (int k = i; k < j; k++)
			run[nrun++] = draw[k];
		raster_run(rast, img, run, nrun, scale, layer, w, h);
		/* a mask that names nothing hides what uses it, as in browsers */
		if (mask[0] && !nmask)
			memset(cover, 0, npix);
		composite(px, layer, mask[0] ? cover : NULL, npix);
		i = j;
	}
	/* nsvgDelete walks the original list */
	for (int k = 0; k < n; k++)
		all[k]->next = k + 1 < n ? all[k + 1] : NULL;
	img->shapes = n ? all[0] : NULL;
	Mem_Free(cover);
	Mem_Free(layer);
	Mem_Free(all);
}

static bool uses_masks(const NSVGimage *img)
{
	for (const NSVGshape *sh = img->shapes; sh; sh = sh->next)
		if (sh->mask[0] || sh->maskDef[0])
			return true;
	return false;
}

bool R_LooksLikeSVG(const uint8_t *data, size_t len)
{
	size_t n = len < 1024 ? len : 1024;
	for (size_t i = 0; i + 4 <= n; i++)
		if (!memcmp(data + i, "<svg", 4))
			return true;
	return false;
}

uint8_t *R_DecodeSVG(const uint8_t *data, size_t len, int *w, int *h)
{
	char *text = Mem_Alloc(len + 1);
	NSVGimage *img;
	NSVGrasterizer *rast;
	uint8_t *px = NULL;
	float scale = 2, longest;
	memcpy(text, data, len);
	text[len] = 0;
	img = nsvgParse(text, "px", 96);
	Mem_Free(text);
	if (!img)
		return NULL;
	if (img->width <= 0 || img->height <= 0) {
		nsvgDelete(img);
		return NULL;
	}
	longest = img->width > img->height ? img->width : img->height;
	if (longest * scale < 512)
		scale = 512 / longest;
	while (scale > 0.25f && (img->width * scale > 4096 || img->height * scale > 4096))
		scale /= 2;
	*w = (int)ceilf(img->width * scale);
	*h = (int)ceilf(img->height * scale);
	if ((rast = nsvgCreateRasterizer())) {
		px = Mem_Alloc((size_t)*w * *h * 4);
		if (uses_masks(img))
			raster_masked(rast, img, scale, px, *w, *h);
		else
			nsvgRasterize(rast, img, 0, 0, scale, px, *w, *h, *w * 4);
		nsvgDeleteRasterizer(rast);
	}
	nsvgDelete(img);
	return px;
}
