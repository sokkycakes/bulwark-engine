/* Bulwark engine - film playback: playfilm, cinematic, playvideo and
 * playclip show a video over everything else, with its sound.
 *
 * Two formats are read. RoQ, the cinematic format of Quake III era games,
 * as the public format notes describe it: chunks with a 16-bit id, 32-bit
 * size and 16-bit argument; codebooks of 2x2 YUV cells and 4x4 cells made
 * of them; frames coded as 16x16 macroblocks of 8x8 blocks that are
 * skipped, moved from the last frame, drawn from the codebooks or split
 * further; and DPCM sound. And AVI files with Motion JPEG or uncompressed
 * pictures and PCM sound, which is what "capture" writes.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../render/render.h"
#include "../sound/sound.h"
#include "../thirdparty/stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FMT_ROQ, FMT_AVI };

static struct {
	bool active, skippable;
	int format;
	char name[256];
	uint8_t *file;
	size_t len, pos;
	int width, height;
	double fps;
	uint8_t *rgba, *prev; /* current and last frame, RGBA */
	int image;            /* the renderer's copy */
	double start;         /* realtime of frame 0 */
	int frame;            /* frames decoded */
	bool image_dirty;
	/* RoQ */
	uint8_t cb2[256][4][4]; /* 2x2 cells as RGBA pixels */
	uint8_t cb4[256][4];    /* 4x4 cells: four 2x2 cell numbers */
	/* AVI */
	int vidstream, audstream;
	int audrate, audchannels, audbits;
	bool mjpeg, bottomup;
	int bitcount;
	size_t movi_end;
} cin;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void yuv_to_rgba(int y, int u, int v, uint8_t *out)
{
	u -= 128;
	v -= 128;
	out[0] = clamp255(y + (int)(1.402f * v));
	out[1] = clamp255(y - (int)(0.344f * u + 0.714f * v));
	out[2] = clamp255(y + (int)(1.772f * u));
	out[3] = 255;
}

/* --- RoQ -------------------------------------------------------------------------- */

#define ROQ_INFO 0x1001
#define ROQ_CODEBOOK 0x1002
#define ROQ_VQ 0x1011
#define ROQ_SOUND_MONO 0x1020
#define ROQ_SOUND_STEREO 0x1021
#define ROQ_SIGNATURE 0x1084

static void roq_codebook(const uint8_t *d, uint32_t size, uint16_t arg)
{
	int hi = arg >> 8, lo = arg & 0xff;
	int n2 = hi ? hi : 256, n4 = lo ? lo : 256;
	/* the counts are whatever makes the sizes add up */
	if ((uint32_t)(n2 * 6 + n4 * 4) != size) {
		n2 = hi ? hi : 256;
		n4 = (int)((size - (uint32_t)BW_MIN((uint32_t)n2 * 6, size)) / 4);
		if (n4 > 256)
			n4 = 256;
	}
	for (int i = 0; i < n2 && (uint32_t)(i * 6 + 6) <= size; i++) {
		const uint8_t *c = d + i * 6;
		for (int k = 0; k < 4; k++)
			yuv_to_rgba(c[k], c[4], c[5], cin.cb2[i][k]);
	}
	const uint8_t *q = d + n2 * 6;
	for (int i = 0; i < n4 && (uint32_t)(n2 * 6 + i * 4 + 4) <= size; i++)
		memcpy(cin.cb4[i], q + i * 4, 4);
}

/* a 2x2 cell at x,y, each pixel scale times over */
static void put_cell2(int x, int y, int cell, int scale)
{
	for (int k = 0; k < 4; k++) {
		const uint8_t *px = cin.cb2[cell][k];
		int cx = x + (k & 1) * scale, cy = y + (k >> 1) * scale;
		for (int sy = 0; sy < scale; sy++)
			for (int sx = 0; sx < scale; sx++) {
				int X = cx + sx, Y = cy + sy;
				if (X < cin.width && Y < cin.height)
					memcpy(cin.rgba + ((size_t)Y * cin.width + X) * 4, px, 4);
			}
	}
}

/* a 4x4 cell (four 2x2 cells), scale 1 for 4x4 pixels or 2 for 8x8 */
static void put_cell4(int x, int y, int cell, int scale)
{
	for (int k = 0; k < 4; k++)
		put_cell2(x + (k & 1) * 2 * scale, y + (k >> 1) * 2 * scale, cin.cb4[cell][k], scale);
}

/* a size by size block moved from the last frame */
static void motion(int x, int y, int size, int dx, int dy)
{
	for (int j = 0; j < size; j++) {
		int sy = y + j + dy, ty = y + j;
		if (ty >= cin.height)
			break;
		for (int i = 0; i < size; i++) {
			int sx = x + i + dx, tx = x + i;
			if (tx >= cin.width)
				break;
			sx = BW_CLAMP(sx, 0, cin.width - 1);
			sy = BW_CLAMP(sy, 0, cin.height - 1);
			memcpy(cin.rgba + ((size_t)ty * cin.width + tx) * 4, cin.prev + ((size_t)sy * cin.width + sx) * 4, 4);
		}
	}
}

typedef struct {
	const uint8_t *d, *end;
	uint16_t flags;
	int left; /* codes left in flags */
} vqread_t;

static int next_code(vqread_t *r)
{
	if (!r->left) {
		if (r->d + 2 > r->end)
			return 0;
		r->flags = rd16(r->d);
		r->d += 2;
		r->left = 8;
	}
	r->left--;
	return (r->flags >> (r->left * 2)) & 3;
}

static int next_byte(vqread_t *r) { return r->d < r->end ? *r->d++ : 0; }

static void roq_vq(const uint8_t *d, uint32_t size, uint16_t arg)
{
	vqread_t r = {d, d + size, 0, 0};
	int mx = (int8_t)(arg >> 8), my = (int8_t)(arg & 0xff);
	memcpy(cin.prev, cin.rgba, (size_t)cin.width * cin.height * 4);
	for (int by = 0; by < cin.height; by += 16)
		for (int bx = 0; bx < cin.width; bx += 16)
			for (int q = 0; q < 4; q++) {
				int x = bx + (q & 1) * 8, y = by + (q >> 1) * 8;
				int b;
				switch (next_code(&r)) {
				case 0:
					break; /* the same as last frame */
				case 1:
					b = next_byte(&r);
					motion(x, y, 8, 8 - (b >> 4) - mx, 8 - (b & 15) - my);
					break;
				case 2:
					put_cell4(x, y, next_byte(&r), 2);
					break;
				case 3:
					for (int s = 0; s < 4; s++) {
						int sx = x + (s & 1) * 4, sy = y + (s >> 1) * 4;
						switch (next_code(&r)) {
						case 0:
							break;
						case 1:
							b = next_byte(&r);
							motion(sx, sy, 4, 8 - (b >> 4) - mx, 8 - (b & 15) - my);
							break;
						case 2:
							put_cell4(sx, sy, next_byte(&r), 1);
							break;
						case 3:
							for (int c = 0; c < 4; c++)
								put_cell2(sx + (c & 1) * 2, sy + (c >> 1) * 2, next_byte(&r), 1);
							break;
						}
					}
					break;
				}
			}
	cin.image_dirty = true;
}

static void roq_sound(const uint8_t *d, uint32_t size, uint16_t arg, bool stereo)
{
	int channels = stereo ? 2 : 1;
	int frames = (int)size / channels;
	int16_t *out = Mem_Alloc((size_t)frames * 4 + 4);
	int pred[2] = {stereo ? (int16_t)(arg & 0xff00) : (int16_t)arg, stereo ? (int16_t)((arg & 0xff) << 8) : 0};
	for (int i = 0; i < frames; i++)
		for (int c = 0; c < channels; c++) {
			int b = d[i * channels + c];
			int delta = (b & 0x7f) * (b & 0x7f);
			pred[c] += (b & 0x80) ? -delta : delta;
			pred[c] = BW_CLAMP(pred[c], -32768, 32767);
			out[i * 2 + c] = (int16_t)pred[c];
			if (!stereo)
				out[i * 2 + 1] = (int16_t)pred[c];
		}
	S_RawSamples(out, frames, 22050, Cvar_Value("volume"));
	Mem_Free(out);
}

static bool alloc_frames(int w, int h)
{
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		return false;
	cin.width = w;
	cin.height = h;
	cin.rgba = Mem_Alloc((size_t)w * h * 4);
	cin.prev = Mem_Alloc((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++)
		cin.rgba[i * 4 + 3] = cin.prev[i * 4 + 3] = 255;
	return true;
}

/* decodes chunks until one frame is done; false at the end */
static bool roq_next_frame(void)
{
	while (cin.pos + 8 <= cin.len) {
		const uint8_t *h = cin.file + cin.pos;
		uint16_t id = rd16(h), arg = rd16(h + 6);
		uint32_t size = rd32(h + 2);
		cin.pos += 8;
		if (id == ROQ_SIGNATURE)
			continue;
		if (size > cin.len - cin.pos)
			return false;
		const uint8_t *d = cin.file + cin.pos;
		cin.pos += size;
		switch (id) {
		case ROQ_INFO:
			if (!cin.rgba && (size < 4 || !alloc_frames(rd16(d), rd16(d + 2))))
				return false;
			break;
		case ROQ_CODEBOOK:
			roq_codebook(d, size, arg);
			break;
		case ROQ_VQ:
			if (!cin.rgba)
				return false;
			roq_vq(d, size, arg);
			return true;
		case ROQ_SOUND_MONO:
		case ROQ_SOUND_STEREO:
			roq_sound(d, size, arg, id == ROQ_SOUND_STEREO);
			break;
		default:
			break; /* chunks we don't use */
		}
	}
	return false;
}

/* --- AVI ---------------------------------------------------------------------------- */

static bool avi_open(void)
{
	const uint8_t *f = cin.file;
	if (cin.len < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "AVI ", 4))
		return false;
	cin.vidstream = cin.audstream = -1;
	int stream = -1;
	char type[5] = "";
	size_t p = 12;
	/* walks the lists for the stream headers and the movie */
	while (p + 8 <= cin.len) {
		const uint8_t *c = f + p;
		uint32_t size = rd32(c + 4);
		if (!memcmp(c, "LIST", 4)) {
			if (p + 12 > cin.len)
				break;
			if (!memcmp(c + 8, "movi", 4)) {
				cin.pos = p + 12;
				cin.movi_end = BW_MIN(cin.len, p + 8 + size);
				break;
			}
			if (!memcmp(c + 8, "strl", 4))
				stream++;
			p += 12; /* into the list */
			continue;
		}
		const uint8_t *d = c + 8;
		if (p + 8 + size > cin.len)
			break;
		if (!memcmp(c, "avih", 4) && size >= 40) {
			uint32_t usec = rd32(d);
			cin.fps = usec ? 1000000.0 / usec : 25;
		} else if (!memcmp(c, "strh", 4) && size >= 32) {
			memcpy(type, d, 4);
			if (!memcmp(type, "vids", 4)) {
				uint32_t scale = rd32(d + 20), rate = rd32(d + 24);
				if (scale && rate)
					cin.fps = (double)rate / scale;
			}
		} else if (!memcmp(c, "strf", 4)) {
			if (!memcmp(type, "vids", 4) && size >= 40 && cin.vidstream < 0) {
				int w = (int)rd32(d + 4), h = (int)rd32(d + 8);
				cin.bitcount = rd16(d + 14);
				cin.mjpeg = !memcmp(d + 16, "MJPG", 4) || !memcmp(d + 16, "mjpg", 4);
				cin.bottomup = h > 0;
				if (!cin.mjpeg && (rd32(d + 16) != 0 || (cin.bitcount != 24 && cin.bitcount != 32)))
					return false; /* a codec we can't read */
				if (!alloc_frames(w, h < 0 ? -h : h))
					return false;
				cin.vidstream = stream;
			} else if (!memcmp(type, "auds", 4) && size >= 16 && cin.audstream < 0 && rd16(d) == 1) {
				cin.audchannels = rd16(d + 2);
				cin.audrate = (int)rd32(d + 4);
				cin.audbits = rd16(d + 14);
				if ((cin.audbits == 8 || cin.audbits == 16) && (cin.audchannels == 1 || cin.audchannels == 2))
					cin.audstream = stream;
			}
		}
		p += 8 + size + (size & 1);
	}
	return cin.vidstream >= 0 && cin.pos;
}

static void avi_audio(const uint8_t *d, uint32_t size)
{
	int bytes = cin.audbits / 8, frames = (int)size / (bytes * cin.audchannels);
	int16_t *out = Mem_Alloc((size_t)frames * 4 + 4);
	for (int i = 0; i < frames; i++)
		for (int c = 0; c < 2; c++) {
			const uint8_t *s = d + ((size_t)i * cin.audchannels + (c < cin.audchannels ? c : 0)) * bytes;
			out[i * 2 + c] = bytes == 1 ? (int16_t)((s[0] - 128) << 8) : (int16_t)rd16(s);
		}
	S_RawSamples(out, frames, cin.audrate, Cvar_Value("volume"));
	Mem_Free(out);
}

static void avi_picture(const uint8_t *d, uint32_t size)
{
	if (cin.mjpeg) {
		int w, h, comp;
		uint8_t *px = stbi_load_from_memory(d, (int)size, &w, &h, &comp, 4);
		if (px) {
			if (w == cin.width && h == cin.height)
				memcpy(cin.rgba, px, (size_t)w * h * 4);
			stbi_image_free(px);
		}
	} else {
		int bpp = cin.bitcount / 8, stride = (cin.width * bpp + 3) & ~3;
		if ((size_t)stride * cin.height > size)
			return;
		for (int y = 0; y < cin.height; y++) {
			const uint8_t *row = d + (size_t)(cin.bottomup ? cin.height - 1 - y : y) * stride;
			for (int x = 0; x < cin.width; x++) {
				uint8_t *o = cin.rgba + ((size_t)y * cin.width + x) * 4;
				o[0] = row[x * bpp + 2];
				o[1] = row[x * bpp + 1];
				o[2] = row[x * bpp];
				o[3] = 255;
			}
		}
	}
	cin.image_dirty = true;
}

static bool avi_next_frame(void)
{
	while (cin.pos + 8 <= cin.movi_end) {
		const uint8_t *c = cin.file + cin.pos;
		uint32_t size = rd32(c + 4);
		if (!memcmp(c, "LIST", 4)) { /* 'rec ' lists group chunks */
			cin.pos += 12;
			continue;
		}
		if (size > cin.movi_end - cin.pos - 8)
			return false;
		const uint8_t *d = c + 8;
		cin.pos += 8 + size + (size & 1);
		int stream = (c[0] - '0') * 10 + (c[1] - '0');
		if (stream == cin.audstream && c[2] == 'w' && c[3] == 'b')
			avi_audio(d, size);
		else if (stream == cin.vidstream && c[2] == 'd') {
			avi_picture(d, size);
			return true;
		}
	}
	return false;
}

/* --- playing ---------------------------------------------------------------------------- */

void CIN_Stop(void)
{
	if (!cin.active)
		return;
	Mem_Free(cin.file);
	Mem_Free(cin.rgba);
	Mem_Free(cin.prev);
	S_RawClear();
	memset(&cin, 0, sizeof(cin));
}

static bool next_frame(void)
{
	bool ok = cin.format == FMT_ROQ ? roq_next_frame() : avi_next_frame();
	if (ok)
		cin.frame++;
	return ok;
}

static bool play(const char *name, bool skippable)
{
	static const char *const tries[] = {"%s", "video/%s", "%s.roq", "video/%s.roq", "%s.avi", "video/%s.avi"};
	char path[300];
	size_t len = 0;
	uint8_t *data = NULL;
	CIN_Stop();
	for (size_t i = 0; i < BW_ARRAY_COUNT(tries) && !data; i++) {
		snprintf(path, sizeof(path), tries[i], name);
		data = (uint8_t *)FS_LoadFile(path, &len);
	}
	if (!data) {
		Con_Printf("Unable to play cinematic %s\n", name);
		return false;
	}
	memset(&cin, 0, sizeof(cin));
	cin.file = data;
	cin.len = len;
	Str_Copy(cin.name, path, sizeof(cin.name));
	if (len >= 8 && rd16(data) == ROQ_SIGNATURE) {
		cin.format = FMT_ROQ;
		cin.fps = rd16(data + 6) ? rd16(data + 6) : 30;
		cin.pos = 0;
	} else if (avi_open()) {
		cin.format = FMT_AVI;
	} else {
		Con_Printf("Unable to load video %s (RoQ, or AVI with Motion JPEG or uncompressed pictures)\n", path);
		Mem_Free(data);
		Mem_Free(cin.rgba);
		Mem_Free(cin.prev);
		memset(&cin, 0, sizeof(cin));
		return false;
	}
	cin.active = true;
	cin.skippable = skippable;
	if (!next_frame()) {
		Con_Printf("Unable to load video %s\n", path);
		CIN_Stop();
		return false;
	}
	cin.start = cl.realtime;
	return true;
}

bool CIN_Playing(void) { return cin.active; }

/* keys while a film plays: escape, space, enter or a click skips it */
bool CIN_KeyEvent(int key, bool down)
{
	if (!cin.active)
		return false;
	if (down && cin.skippable && (key == K_ESCAPE || key == K_SPACE || key == K_ENTER || key == K_MOUSE1))
		CIN_Stop();
	return true;
}

/* catches up with the clock; drawn by the renderer at the end of the frame */
void CL_DrawCinematic(int w, int h)
{
	if (!cin.active)
		return;
	int want = (int)((cl.realtime - cin.start) * cin.fps) + 1;
	for (int n = 0; cin.frame < want && n < 8; n++)
		if (!next_frame()) {
			CIN_Stop();
			return;
		}
	if (cin.frame < want)
		cin.start = cl.realtime - (cin.frame - 1) / cin.fps; /* too slow: drop the lag rather than race */
	if (cin.image_dirty || !cin.image) {
		if (!cin.image)
			cin.image = R_UploadImage("_cinematic", cin.rgba, cin.width, cin.height);
		else
			R_UpdateImage(cin.image, 0, 0, cin.width, cin.height, cin.rgba);
		cin.image_dirty = false;
	}
	/* black around it, the picture as big as fits */
	static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
	r2dvert_t v[4];
	memset(v, 0, sizeof(v));
	float bw = (float)w, bh = (float)h;
	for (int i = 0; i < 4; i++)
		v[i].rgba[3] = 255;
	v[1].x = v[2].x = bw;
	v[2].y = v[3].y = bh;
	R_Draw2DTriangles(0, v, 4, idx, 6, R2D_ALPHA);
	float scale = fminf(bw / cin.width, bh / cin.height);
	float pw = cin.width * scale, ph = cin.height * scale, x0 = (bw - pw) / 2, y0 = (bh - ph) / 2;
	for (int i = 0; i < 4; i++)
		memset(v[i].rgba, 255, 4);
	v[0].x = v[3].x = x0;
	v[1].x = v[2].x = x0 + pw;
	v[0].y = v[1].y = y0;
	v[2].y = v[3].y = y0 + ph;
	v[1].s = v[2].s = 1;
	v[2].t = v[3].t = 1;
	v[0].s = v[3].s = 0;
	v[0].t = v[1].t = 0;
	R_Draw2DTriangles(cin.image, v, 4, idx, 6, R2D_ALPHA);
}

/* playfilm/cinematic/playvideo <file>: a film, skippable with escape */
static void Playfilm_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("playfilm <filename>\n");
		return;
	}
	play(argv[1], true);
}

/* playclip <file>: a clip that plays to its end */
static void Playclip_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("playclip <filename>\n");
		return;
	}
	play(argv[1], false);
}

void CL_InitCinematics(void)
{
	Cmd_AddD("playfilm", Playfilm_f, "playfilm <filename>: play a RoQ or AVI film (escape skips it)");
	Cmd_AddD("cinematic", Playfilm_f, "cinematic <filename>: play a film from video/");
	Cmd_AddD("playvideo", Playfilm_f, "playvideo <filename>: play a film");
	Cmd_AddD("playclip", Playclip_f, "playclip <filename>: play a film to its end");
}
