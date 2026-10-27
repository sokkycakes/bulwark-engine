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
		nsvgRasterize(rast, img, 0, 0, scale, px, *w, *h, *w * 4);
		nsvgDeleteRasterizer(rast);
	}
	nsvgDelete(img);
	return px;
}
