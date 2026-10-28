/* Bulwark engine - the Nintendo DS map viewer: start-up, files, the map
 * list and the controls. ISC licence, see LICENSE.
 *
 * Maps come from the ROM's own file system (nitro:/maps) or from the
 * flashcart's SD card (fat:/bulwark/base/maps), through its DLDI driver.
 * The bottom screen lists them. The top screen draws the one that's picked.
 *
 *   D-pad up/down   move           D-pad left/right  turn
 *   L / R           strafe         X / B             look up / down
 *   A / Y           rise / sink    touch and drag    look around
 *   START           back to the map list
 *
 * A bench.txt next to maps/ (one map file name a line) times a turn on the
 * spot in each of those maps at start-up and shows the results. */
#include <dirent.h>
#include <fat.h>
#include <filesystem.h>
#include <malloc.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nds.h"

#define MAX_MAPS 64

static const char *const basedirs[] = {"nitro:/", "fat:/bulwark/base/", "fat:/bulwark/", "fat:/"};
static char maps[MAX_MAPS][300];
static int nummaps;
static uint16_t palette[256];
static bool havepalette;

static bool read_file(const char *path, uint8_t *buf, size_t size)
{
	FILE *f = fopen(path, "rb");
	bool ok;
	if (!f)
		return false;
	ok = fread(buf, 1, size, f) == size;
	fclose(f);
	return ok;
}

/* gfx/palette.lmp (768 bytes), or the 16x16 gfx/palette.tga Nuclide ships */
static void load_palette(void)
{
	static uint8_t buf[18 + 16 * 16 * 4];
	char path[300];
	for (size_t b = 0; b < sizeof(basedirs) / sizeof(basedirs[0]) && !havepalette; b++) {
		snprintf(path, sizeof(path), "%sgfx/palette.lmp", basedirs[b]);
		if (read_file(path, buf, 768)) {
			for (int i = 0; i < 256; i++)
				palette[i] = RGB15(buf[i * 3] >> 3, buf[i * 3 + 1] >> 3, buf[i * 3 + 2] >> 3);
			havepalette = true;
			break;
		}
		snprintf(path, sizeof(path), "%sgfx/palette.tga", basedirs[b]);
		if (read_file(path, buf, 18 + 16 * 16 * 3) && buf[2] == 2 && buf[12] == 16 && buf[14] == 16 &&
		    (buf[16] == 24 || buf[16] == 32)) {
			int bpp = buf[16] / 8;
			bool topdown = buf[17] & 0x20;
			if (bpp == 4 && !read_file(path, buf, 18 + 16 * 16 * 4))
				continue;
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++) {
					const uint8_t *p = buf + 18 + buf[0] + ((topdown ? y : 15 - y) * 16 + x) * bpp;
					palette[y * 16 + x] = RGB15(p[2] >> 3, p[1] >> 3, p[0] >> 3);
				}
			havepalette = true;
		}
	}
}

static void find_maps(void)
{
	nummaps = 0;
	for (size_t b = 0; b < sizeof(basedirs) / sizeof(basedirs[0]); b++) {
		char dir[280];
		DIR *d;
		struct dirent *e;
		snprintf(dir, sizeof(dir), "%smaps", basedirs[b]);
		d = opendir(dir);
		if (!d)
			continue;
		while ((e = readdir(d)) && nummaps < MAX_MAPS) {
			size_t n = strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".bsp") == 0)
				snprintf(maps[nummaps++], sizeof(maps[0]), "%s/%s", dir, e->d_name);
		}
		closedir(d);
	}
}

static const char *short_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

static void print_memory(void)
{
	struct mallinfo mi = mallinfo();
	iprintf("RAM %d KB used, VRAM tex %d KB\n", mi.uordblks >> 10, World_TextureBytes() >> 10);
}

/* the map list; returns the index picked */
static int pick_map(int sel)
{
	int top = 0;
	for (;;) {
		if (sel < top)
			top = sel;
		if (sel >= top + 18)
			top = sel - 17;
		consoleClear();
		iprintf("Bulwark DS - pick a map (A)\n\n");
		for (int i = top; i < nummaps && i < top + 18; i++)
			iprintf("%c %.29s\n", i == sel ? '>' : ' ', short_name(maps[i]));
		for (;;) {
			uint32_t down;
			swiWaitForVBlank();
			scanKeys();
			down = keysDown();
			if (down & KEY_A)
				return sel;
			if (down & KEY_TOUCH) {
				/* a tap on a name picks it: the list starts on the third text row */
				touchPosition tp;
				int row;
				touchRead(&tp);
				row = tp.py / 8 - 2 + top;
				if (tp.py >= 16 && row < nummaps && row < top + 18)
					return row;
			}
			if ((down & KEY_DOWN) && sel + 1 < nummaps) {
				sel++;
				break;
			}
			if ((down & KEY_UP) && sel > 0) {
				sel--;
				break;
			}
		}
	}
}

static void init_3d(void)
{
	videoSetMode(MODE_0_3D);
	glInit();
	glEnable(GL_TEXTURE_2D | GL_ANTIALIAS);
	glClearColor(4, 4, 6, 31);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, 255, 191);
	/* textures in banks A, B and D; C shows the text on the bottom screen */
	vramSetBankA(VRAM_A_TEXTURE);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankD(VRAM_D_TEXTURE);
	vramSetBankE(VRAM_E_TEX_PALETTE);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* 70 degrees tall; near 4 map units, far 8192 (1/8 units in 20.12) */
	gluPerspectivef32(70 * 32768 / 360, floattof32(256.0f / 192.0f), 32, 8192 * 8);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
}

static void fly(ndscam_t *cam, uint32_t held, int *lasttx, int *lastty)
{
	const int turn = 32768 / 180, speed = 6 * 8; /* about 360 units a second */
	int sy = sinLerp(cam->yaw), cy = cosLerp(cam->yaw);
	if (held & KEY_LEFT)
		cam->yaw += turn;
	if (held & KEY_RIGHT)
		cam->yaw -= turn;
	if (held & KEY_X)
		cam->pitch += turn;
	if (held & KEY_B)
		cam->pitch -= turn;
	if (held & KEY_TOUCH) {
		touchPosition tp;
		touchRead(&tp);
		if (*lasttx >= 0) {
			cam->yaw -= (tp.px - *lasttx) * 64;
			cam->pitch -= (tp.py - *lastty) * 64;
		}
		*lasttx = tp.px, *lastty = tp.py;
	} else
		*lasttx = -1;
	cam->pitch = BW_NDS_CLAMP(cam->pitch, -8000, 8000);
	cam->yaw = (int16_t)cam->yaw;
	if (held & KEY_UP)
		cam->org[0] += cy * speed >> 12, cam->org[1] += sy * speed >> 12;
	if (held & KEY_DOWN)
		cam->org[0] -= cy * speed >> 12, cam->org[1] -= sy * speed >> 12;
	if (held & KEY_L)
		cam->org[0] -= sy * speed >> 12, cam->org[1] += cy * speed >> 12;
	if (held & KEY_R)
		cam->org[0] += sy * speed >> 12, cam->org[1] -= cy * speed >> 12;
	if (held & KEY_A)
		cam->org[2] += speed;
	if (held & KEY_Y)
		cam->org[2] -= speed;
}

static volatile int vblanks;
static void on_vblank(void) { vblanks++; }

/* bench.txt in a base directory lists maps (one file name a line) to time
 * one turn on the spot in each, for comparing builds on hardware */
#define MAX_RESULTS 8
static char results[MAX_RESULTS][48];
static int numresults;

static int read_bench(char names[][300], int max)
{
	char path[300], line[256];
	int n = 0;
	for (size_t b = 0; b < sizeof(basedirs) / sizeof(basedirs[0]) && !n; b++) {
		FILE *f;
		snprintf(path, sizeof(path), "%sbench.txt", basedirs[b]);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (n < max && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0])
				snprintf(names[n++], 300, "%smaps/%s", basedirs[b], line);
		}
		fclose(f);
	}
	return n;
}

static bool load_map(const char *path, ndscam_t *cam)
{
	char err[128];
	consoleClear();
	iprintf("loading %s...\n", short_name(path));
	if (!World_Load(path, havepalette ? palette : NULL, err, sizeof(err))) {
		iprintf("%s\n", err);
		return false;
	}
	World_StartPosition(cam);
	return true;
}

/* one frame; returns the vblanks it took */
static int frame(const ndscam_t *cam, ndsstats_t *st)
{
	static int last;
	int took;
	World_Draw(cam, st);
	glFlush(0);
	swiWaitForVBlank();
	took = vblanks - last;
	last = vblanks;
	return took;
}

static void bench(const char *path)
{
	ndscam_t cam;
	ndsstats_t st;
	const int steps = 512;
	int start, worst = 0, peakverts = 0, skipped = 0;
	if (numresults >= MAX_RESULTS || !load_map(path, &cam))
		return;
	frame(&cam, &st);
	start = vblanks;
	for (int i = 0; i < steps; i++) {
		int took;
		cam.yaw = (int16_t)(cam.yaw + 32768 / steps);
		took = frame(&cam, &st);
		if (took > worst)
			worst = took;
		peakverts = st.verts > peakverts ? st.verts : peakverts;
		skipped += st.skipped;
	}
	snprintf(results[numresults++], sizeof(results[0]), "%.8s %2dfps min%2d v%4d s%d", short_name(path),
	         steps * 60 / (vblanks - start), 60 / (worst ? worst : 1), peakverts, skipped);
	World_Free();
	glResetTextures();
}

static void show_stats(const char *map, const ndscam_t *cam, const ndsstats_t *st, int fps)
{
	consoleClear();
	iprintf("%s  %d fps\n", short_name(map), fps);
	iprintf("faces %d  verts %d  polys %d\n", st->faces, st->verts, st->polys);
	if (st->skipped)
		iprintf("over the GPU limit: %d faces\n", st->skipped);
	iprintf("leaf %d  eye %ld %ld %ld\n", st->leaf, (long)(cam->org[0] / 8), (long)(cam->org[1] / 8),
	        (long)(cam->org[2] / 8));
	print_memory();
	iprintf("START: map list\n");
	if (numresults) {
		iprintf("\nbench: avg, worst, peak verts,\nfaces over the limit\n");
		for (int i = 0; i < numresults; i++)
			iprintf("%s\n", results[i]);
	}
}

int main(int argc, char **argv)
{
	static char benchmaps[MAX_RESULTS][300];
	ndscam_t cam;
	ndsstats_t st;
	int sel = 0, frames = 0, lastvb = 0, lasttx = -1, lastty = -1, numbench;
	bool fat, nitro;

	consoleDemoInit();
	init_3d();
	irqSet(IRQ_VBLANK, on_vblank);
	iprintf("Bulwark DS\n");
	fat = fatInitDefault();
	nitro = nitroFSInit(NULL);
	iprintf("SD card: %s, ROM files: %s\n", fat ? "yes" : "no", nitro ? "yes" : "no");
	load_palette();
	if (!havepalette)
		iprintf("no gfx/palette.lmp: grey\n");
	find_maps();
	if (argc > 1 && argv[1][0]) {
		snprintf(maps[0], sizeof(maps[0]), "%s", argv[1]);
		nummaps = nummaps ? nummaps : 1;
	}
	if (!nummaps) {
		iprintf("\nNo maps found. Put Quake (BSP 29)\nmaps in /bulwark/base/maps on\nthe SD card.\n");
		for (;;)
			swiWaitForVBlank();
	}
	numbench = read_bench(benchmaps, MAX_RESULTS);
	for (int i = 0; i < numbench; i++)
		bench(benchmaps[i]);

	for (bool first = true;; first = false) {
		const char *path;
		if (first && numbench) {
			path = benchmaps[numbench - 1]; /* stay in the last one */
		} else {
			sel = nummaps > 1 ? pick_map(sel) : 0;
			path = maps[sel];
		}
		if (!load_map(path, &cam)) {
			iprintf("A: back\n");
			do {
				swiWaitForVBlank();
				scanKeys();
			} while (!(keysDown() & KEY_A));
			continue;
		}
		print_memory();
		for (;;) {
			scanKeys();
			if (keysDown() & KEY_START)
				break;
			fly(&cam, keysHeld(), &lasttx, &lastty);
			frame(&cam, &st);
			if (++frames >= 60) {
				show_stats(path, &cam, &st, frames * 60 / (vblanks - lastvb > 0 ? vblanks - lastvb : 1));
				frames = 0;
				lastvb = vblanks;
			}
		}
		World_Free();
		glResetTextures();
	}
	return 0;
}
