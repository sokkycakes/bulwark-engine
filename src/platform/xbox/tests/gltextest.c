/* Bulwark engine - host test of gl_xbox.c's texture storage: which format
 * each kind of texture gets, the sizes of DXT mip chains and cube faces,
 * that what lands in "GPU memory" decodes back to the picture, and the
 * update paths. ISC licence, see LICENSE.
 *
 *   NXDK_DIR=... src/platform/xbox/tests/run_dxttest.sh
 *
 * It compiles gl_xbox.c itself with stand-ins for pbkit, the kernel and
 * the engine (stub/), contiguous memory coming from the C heap; nothing
 * is drawn. nxdk's nv_regs.h supplies the register names. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gl_xbox.c"

/* --- stand-ins ------------------------------------------------------------------------ */

void *MmAllocateContiguousMemoryEx(size_t size, uintptr_t lo, uintptr_t hi, size_t align, DWORD protect)
{
	(void)lo, (void)hi, (void)align, (void)protect;
	return calloc(1, size);
}
void MmFreeContiguousMemory(void *p) { free(p); }
ULONGLONG KeQueryPerformanceCounter(void) { return 0; }
ULONGLONG KeQueryPerformanceFrequency(void) { return 1; }
DWORD GetTickCount(void) { return 0; }
/* covers the register offsets gl_xbox.c reads (NV_USER + 0x44 the furthest) */
uint32_t bw_stub_videoregs[(0x00800000 + 0x1000) / 4];
static uint32_t pbuf[1 << 16];
uint32_t *pb_begin(void) { return pbuf; }
void pb_end(uint32_t *p) { (void)p; }
void pb_reset(void) {}
int pb_busy(void) { return 0; }
int pb_finished(void) { return 0; }
DWORD *pb_back_buffer(void) { return NULL; }
DWORD pb_back_buffer_width(void) { return 640; }
DWORD pb_back_buffer_height(void) { return 480; }
DWORD pb_back_buffer_pitch(void) { return 2560; }
void pb_size(DWORD size) { (void)size; }
int pb_init(void) { return 1; }
void pb_kill(void) {}
void pb_show_front_screen(void) {}
void pb_target_back_buffer(void) {}
DWORD pb_get_vbl_counter(void) { return 0; }
DWORD pb_wait_for_vbl(void) { return 0; }
size_t XBX_FreeMemory(void) { return 0; }
void XBX_NoteFreeMemory(const void *from) { (void)from; }
size_t XBX_TotalMemory(void) { return (size_t)64 << 20; }

void Con_Printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
void Con_DPrintf(const char *fmt, ...) { (void)fmt; }
static cvar_t cvars[32];
static int numcvars;
cvar_t *Cvar_Get(const char *name, const char *defvalue, int flags, const char *description)
{
	(void)flags, (void)description;
	cvar_t *c = &cvars[numcvars++];
	c->name = (char *)name;
	c->value = (float)atof(defvalue), c->ivalue = atoi(defvalue);
	return c;
}
void Cmd_AddD(const char *name, cmd_fn fn, const char *description) { (void)name, (void)fn, (void)description; }
/* shaders aren't compiled here */
void SB_Append(strbuf_t *sb, const char *s, size_t n) { (void)sb, (void)s, (void)n; }
void SB_AppendStr(strbuf_t *sb, const char *s) { (void)sb, (void)s; }
void SB_Free(strbuf_t *sb) { (void)sb; }

/* --- helpers ------------------------------------------------------------------------- */

static int failures;
#define CHECK(cond, ...)                                                                                     \
	do {                                                                                                     \
		if (!(cond)) {                                                                                       \
			failures++;                                                                                      \
			printf("FAIL line %d: ", __LINE__);                                                              \
			printf(__VA_ARGS__);                                                                             \
			printf("\n");                                                                                    \
		}                                                                                                    \
	} while (0)

static uint32_t rng = 1;
static int rnd(int n)
{
	rng = rng * 1664525u + 1013904223u;
	return (int)((rng >> 8) % (uint32_t)n);
}

/* smooth colours, optional soft alpha, or noise */
static uint8_t *image(int w, int h, int alpha, int noise)
{
	uint8_t *p = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *o = p + ((size_t)y * w + x) * 4;
			o[0] = noise ? (uint8_t)rnd(256) : (uint8_t)(x * 255 / w);
			o[1] = noise ? (uint8_t)rnd(256) : (uint8_t)(y * 255 / h);
			o[2] = noise ? (uint8_t)rnd(256) : (uint8_t)(128 + 60 * sin(x * 0.05) * cos(y * 0.05));
			o[3] = alpha == 1 ? (uint8_t)(((x / 8 + y / 8) & 1) ? 255 : 0) : alpha == 2 ? (uint8_t)((x + y) * 255 / (w + h)) : 255;
		}
	return p;
}

static double psnr_rgba(const uint8_t *a, const uint8_t *b, int n)
{
	double se = 0;
	for (int i = 0; i < n * 4; i++)
		se += (a[i] - b[i]) * (double)(a[i] - b[i]);
	return se > 0 ? 10 * log10(255.0 * 255 * n * 4 / se) : 99;
}

static GLuint new_texture(void)
{
	GLuint id;
	xb_glGenTextures(1, &id);
	xb_glBindTexture(GL_TEXTURE_2D, id);
	return id;
}

/* --- tests ---------------------------------------------------------------------------- */

static void test_world_dxt1(void)
{
	uint8_t *src = image(300, 200, 0, 0);
	GLuint id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 300, 200, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xb_glGenerateMipmap(GL_TEXTURE_2D);
	xtex_t *t = get_texs(id);
	CHECK(t->fmt == FMT_DXT1, "opaque world texture as %#x", t->fmt);
	CHECK(t->tw == 512 && t->th == 256 && t->levels == 10, "%dx%d, %d levels", t->tw, t->th, t->levels);
	size_t want = 0;
	for (int l = 0; l < 10; l++)
		want += (size_t)BW_MAX((512 >> l) / 4, 1) * BW_MAX((256 >> l) / 4, 1) * 8;
	CHECK(t->bytes == want, "chain of %zu bytes, want %zu", t->bytes, want);
	/* level 0 against the resampled picture */
	int tw, th;
	uint8_t *ref = prepare_rgba(src, 300, 200, 512, &tw, &th), *dec = malloc((size_t)tw * th * 4);
	DXT_DecodeImage(t->data, dec, tw, th, 8);
	double p = psnr_rgba(ref, dec, tw * th);
	CHECK(p > 35, "level 0 at %.1f dB", p);
	/* the last level (1x1, padded to a block) is about the average */
	uint8_t last[64];
	DXT_DecodeBlock1(t->data + t->bytes - 8, last);
	double avg[3] = {0};
	for (int i = 0; i < tw * th; i++)
		for (int c = 0; c < 3; c++)
			avg[c] += ref[i * 4 + c];
	for (int c = 0; c < 3; c++)
		CHECK(fabs(avg[c] / (tw * th) - last[c]) < 12, "1x1 level channel %d: %d vs %.0f", c, last[c], avg[c] / (tw * th));
	printf("world DXT1: 512x256, %zu bytes (16-bit: %zu), level 0 %.1f dB\n", t->bytes,
	       chain_bytes(512, 256, 10, FMT_R5G6B5), p);

	/* an update of a mipmapped DXT texture rewrites the blocks it touches */
	uint8_t patch[5 * 5 * 4];
	for (int i = 0; i < 25; i++)
		patch[i * 4] = 255, patch[i * 4 + 1] = 0, patch[i * 4 + 2] = 255, patch[i * 4 + 3] = 255;
	uint8_t *before = malloc((size_t)tw * th * 4);
	memcpy(before, dec, (size_t)tw * th * 4);
	t->w = t->tw, t->h = t->th; /* as if uploaded at this size */
	xb_glTexSubImage2D(GL_TEXTURE_2D, 0, 3, 3, 5, 5, GL_RGBA, GL_UNSIGNED_BYTE, patch);
	CHECK(t->fmt == FMT_DXT1, "a mipmapped DXT texture stays DXT on update");
	DXT_DecodeImage(t->data, dec, tw, th, 8);
	int bad = 0;
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 12; x++) {
			const uint8_t *d = dec + ((size_t)y * tw + x) * 4, *b = before + ((size_t)y * tw + x) * 4;
			if (x >= 3 && x < 8 && y >= 3 && y < 8)
				bad += abs(d[0] - 255) > 24 || abs(d[1] - 0) > 24 || abs(d[2] - 255) > 24;
			else if (x >= 8 || y >= 8)
				bad += memcmp(d, b, 4) != 0; /* blocks the patch doesn't reach: untouched */
		}
	CHECK(!bad, "%d texels wrong after the update", bad);
	free(before), free(ref), free(dec), free(src);
}

static void test_world_other(void)
{
	/* cut-out textures keep 16 bits; so does everything with compression off */
	uint8_t *src = image(64, 64, 1, 0);
	GLuint id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xb_glGenerateMipmap(GL_TEXTURE_2D);
	CHECK(get_texs(id)->fmt == FMT_A1R5G5B5, "cut-out world texture as %#x", get_texs(id)->fmt);
	free(src);
	gl_xbox_texcompress->ivalue = 0;
	src = image(64, 64, 0, 0);
	id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xb_glGenerateMipmap(GL_TEXTURE_2D);
	CHECK(get_texs(id)->fmt == FMT_R5G6B5, "with gl_xbox_texcompress 0: %#x", get_texs(id)->fmt);
	CHECK(get_texs(id)->bytes == chain_bytes(64, 64, 7, FMT_R5G6B5), "16-bit chain size");
	gl_xbox_texcompress->ivalue = 1;
	free(src);
}

static void test_pictures(void)
{
	/* a big smooth picture with alpha: DXT5 at its first draw */
	uint8_t *src = image(512, 512, 2, 0);
	GLuint id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xtex_t *t = get_texs(id);
	CHECK(t->fmt == FMT_A8B8G8R8 && t->try_dxt && !t->data && t->pending, "a fresh picture waits in main memory");
	tex2d(id);
	CHECK(t->fmt == FMT_DXT5 && t->bytes == 512 * 512 && !t->pending && !t->try_dxt,
	      "smooth picture after its first draw: %#x, %zu bytes", t->fmt, t->bytes);
	uint8_t *dec = malloc(512 * 512 * 4);
	DXT_DecodeImage(t->data, dec, 512, 512, 16);
	double p = psnr_rgba(src, dec, 512 * 512);
	CHECK(p >= 40, "picture at %.1f dB", p);
	printf("picture DXT5: 512x512, %zu bytes (32-bit: %d), %.1f dB\n", t->bytes, 512 * 512 * 4, p);

	/* updated: back to 32 bits, with the update in place */
	uint8_t patch[16 * 16 * 4];
	memset(patch, 200, sizeof(patch));
	xb_glTexSubImage2D(GL_TEXTURE_2D, 0, 8, 8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, patch);
	CHECK(t->fmt == FMT_A8B8G8R8 && t->bytes == 512 * 512 * 4, "an updated picture goes back to 32 bits");
	uint8_t *back = unswizzle(t);
	CHECK(back && !memcmp(back + (10 * 512 + 10) * 4, patch, 4), "the update landed");
	CHECK(back && abs(back[(300 * 512 + 300) * 4] - dec[(300 * 512 + 300) * 4]) <= 1, "the rest kept the DXT picture");
	free(back), free(dec), free(src);

	/* noise isn't near-lossless: stays 32-bit; the next upload decides it */
	src = image(512, 512, 0, 1);
	GLuint noisy = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	free(src);
	src = image(512, 512, 0, 0);
	GLuint smooth = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	CHECK(get_texs(noisy)->fmt == FMT_A8B8G8R8 && !get_texs(noisy)->pending && get_texs(noisy)->data,
	      "noisy picture kept at 32 bits");
	/* a big world texture is compressed once, as a world texture */
	xb_glGenerateMipmap(GL_TEXTURE_2D);
	CHECK(get_texs(smooth)->fmt == FMT_DXT1 && get_texs(smooth)->mipped && !get_texs(smooth)->try_dxt,
	      "512x512 world texture: %#x", get_texs(smooth)->fmt);
	free(src);

	/* small pictures (HUD sheets) and lightmaps are left alone */
	src = image(256, 256, 0, 0);
	id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	tex2d(id);
	CHECK(get_texs(id)->fmt == FMT_A8B8G8R8, "256x256 picture stays 32-bit");
	free(src);
	uint8_t *rgb = calloc(1024 * 1024, 3);
	id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1024, 1024, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	tex2d(id);
	CHECK(get_texs(id)->fmt == FMT_A8B8G8R8, "lightmap stays 32-bit");
	free(rgb);
}

static void test_palette(void)
{
	/* a sprite sheet: Quake palette colours with a transparent index */
	uint8_t *src = malloc(256 * 128 * 4);
	for (int i = 0; i < 256 * 128; i++) {
		int c = rnd(200);
		src[i * 4] = (uint8_t)c, src[i * 4 + 1] = (uint8_t)(c * 3), src[i * 4 + 2] = (uint8_t)(255 - c);
		src[i * 4 + 3] = c < 20 ? 0 : 255;
	}
	GLuint id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xtex_t *t = get_texs(id);
	CHECK(t->fmt == FMT_I8 && t->bytes == PAL_BYTES + 256 * 128, "few colours: palette, %#x %zu bytes", t->fmt, t->bytes);
	uint8_t *back = unswizzle(t);
	CHECK(back && !memcmp(back, src, 256 * 128 * 4), "the palette picture is exact");
	free(back);
	printf("palette: 256x128, %zu bytes (32-bit: %d)\n", t->bytes, 256 * 128 * 4);

	/* an update goes back to 32 bits with the rest intact */
	uint8_t patch[4 * 4 * 4];
	memset(patch, 77, sizeof(patch));
	xb_glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, patch);
	CHECK(t->fmt == FMT_A8B8G8R8, "an updated palette picture goes back to 32 bits");
	back = unswizzle(t);
	CHECK(back && !memcmp(back + (5 * 256 + 5) * 4, patch, 4) && !memcmp(back + 100 * 4, src + 100 * 4, 4),
	      "the update landed and the rest is kept");
	free(back);

	/* mipmapped: the world path decides from the full picture */
	id = new_texture();
	xb_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
	xb_glGenerateMipmap(GL_TEXTURE_2D);
	CHECK(get_texs(id)->mipped && get_texs(id)->fmt != FMT_I8, "a mipmapped texture leaves the palette");
	free(src);
}

static void test_cube(void)
{
	GLuint id;
	xb_glGenTextures(1, &id);
	xb_glBindTexture(GL_TEXTURE_CUBE_MAP, id);
	uint8_t *faces[6];
	for (int f = 0; f < 6; f++) {
		faces[f] = image(64, 64, 0, 0);
		for (int i = 0; i < 64 * 64; i++)
			faces[f][i * 4 + 2] = (uint8_t)(f * 40);
		xb_glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces[f]);
	}
	xtex_t *t = get_texs(id);
	CHECK(t->cube && t->fmt == FMT_DXT1 && t->bytes == 6 * 2048, "sky: %#x, %zu bytes", t->fmt, t->bytes);
	uint8_t dec[64 * 64 * 4];
	DXT_DecodeImage(t->data + 2048 * 3, dec, 64, 64, 8);
	CHECK(psnr_rgba(faces[3], dec, 64 * 64) > 35, "face 3 at %.1f dB", psnr_rgba(faces[3], dec, 64 * 64));
	for (int f = 0; f < 6; f++)
		free(faces[f]);
}

/* A run's textures (texture dumps with a log.txt of uploads and deletions
 * up to a "steady" line) through the storage code, with and without
 * compression: the GPU memory they take. */
static size_t replay(const char *dir, int compress)
{
	char path[1024], line[256];
	static int last[65536];
	memset(last, 0, sizeof(last));
	snprintf(path, sizeof(path), "%s/log.txt", dir);
	FILE *l = fopen(path, "r");
	if (!l)
		return 0;
	while (fgets(line, sizeof(line), l) && strncmp(line, "steady", 6)) {
		unsigned id;
		int n;
		if (sscanf(line, "upload %u %d", &id, &n) == 2 && id < 65536)
			last[id] = n;
		else if (sscanf(line, "delete %u", &id) == 1 && id < 65536)
			last[id] = 0;
	}
	fclose(l);
	gl_xbox_texcompress->ivalue = compress;
	size_t start = gpu_used;
	GLuint ids[512];
	int count = 0;
	for (int id = 0; id < 65536 && count < 512; id++) {
		if (!last[id])
			continue;
		snprintf(path, sizeof(path), "%s/%04d.tex", dir, last[id]);
		FILE *f = fopen(path, "rb");
		int hdr[4];
		if (!f || fread(hdr, sizeof(hdr), 1, f) != 1) {
			if (f)
				fclose(f);
			continue;
		}
		size_t size = (size_t)hdr[0] * hdr[1] * hdr[2];
		uint8_t *px = malloc(size);
		if (fread(px, size, 1, f) != 1) {
			fclose(f), free(px);
			continue;
		}
		fclose(f);
		ids[count] = new_texture();
		xb_glTexImage2D(GL_TEXTURE_2D, 0, hdr[3], hdr[0], hdr[1], 0, hdr[2] == 3 ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE, px);
		snprintf(path, sizeof(path), "%s/%04d.mip", dir, last[id]);
		if ((f = fopen(path, "rb"))) {
			fclose(f);
			xb_glGenerateMipmap(GL_TEXTURE_2D);
		}
		free(px);
		count++;
	}
	for (int i = 0; i < count; i++)
		tex2d(ids[i]); /* drawn */
	run_deferred(true);
	size_t used = gpu_used - start;
	printf("%s: %d textures, %zu KB of GPU memory with gl_xbox_texcompress %d\n", dir, count, used >> 10, compress);
	if (compress)
		XBGL_PrintGpuMemory();
	xb_glDeleteTextures(count, ids);
	run_deferred(true);
	gl_xbox_texcompress->ivalue = 1;
	return used;
}

int main(int argc, char **argv)
{
	XBGL_RegisterCvars();
	test_world_dxt1();
	test_world_other();
	test_pictures();
	test_palette();
	test_cube();
	run_deferred(true);
	if (argc > 1) {
		size_t off = replay(argv[1], 0), on = replay(argv[1], 1);
		printf("saved %zu KB\n", (off - on) >> 10);
	}
	printf(failures ? "gltextest: %d failures\n" : "gltextest: all passed\n", failures);
	return failures != 0;
}
