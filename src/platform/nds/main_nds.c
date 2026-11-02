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

const char *const nds_basedirs[NDS_NUMBASEDIRS] = {"nitro:/", "fat:/bulwark/base/", "fat:/bulwark/", "fat:/"};
#define basedirs nds_basedirs
static char maps[MAX_MAPS][300];
static int nummaps;
static uint16_t palette[256];
static bool havepalette;

/* gfx/palette.lmp (768 bytes), or the 16x16 gfx/palette.tga Nuclide ships */
static void load_palette(void)
{
	size_t size;
	uint8_t *buf = Fs_Load("gfx/palette.lmp", &size);
	if (buf && size >= 768) {
		for (int i = 0; i < 256; i++)
			palette[i] = RGB15(buf[i * 3] >> 3, buf[i * 3 + 1] >> 3, buf[i * 3 + 2] >> 3);
		havepalette = true;
	}
	free(buf);
	if (havepalette || !(buf = Fs_Load("gfx/palette.tga", &size)))
		return;
	if (size >= 18 && buf[2] == 2 && buf[12] == 16 && buf[14] == 16 && (buf[16] == 24 || buf[16] == 32) &&
	    size >= 18u + buf[0] + 16 * 16 * (buf[16] / 8)) {
		int bpp = buf[16] / 8;
		bool topdown = buf[17] & 0x20;
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) {
				const uint8_t *p = buf + 18 + buf[0] + ((topdown ? y : 15 - y) * 16 + x) * bpp;
				palette[y * 16 + x] = RGB15(p[2] >> 3, p[1] >> 3, p[0] >> 3);
			}
		havepalette = true;
	}
	free(buf);
}

static void add_map(const char *name, void *user)
{
	(void)user;
	if (nummaps < MAX_MAPS)
		snprintf(maps[nummaps++], sizeof(maps[0]), "%s", name);
}

static void find_maps(void)
{
	nummaps = 0;
	Fs_List("maps", ".bsp", add_map, NULL);
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

/* the menu: joining a game, then the maps; returns -1 to join, or the map picked */
static int pick_map(int sel)
{
	const int rows = 17, entries = nummaps + 1;
	int top = 0;
	for (;;) {
		if (sel < top)
			top = sel;
		if (sel >= top + rows)
			top = sel - rows + 1;
		consoleClear();
		iprintf("Bulwark DS - pick one (A)\n\n");
		for (int i = top; i < entries && i < top + rows; i++)
			iprintf("%c %.29s\n", i == sel ? '>' : ' ', i ? short_name(maps[i - 1]) : "[Join a game over WiFi]");
		if (!nummaps)
			iprintf("\nNo maps found. Put Quake (BSP 29)\nmaps in /bulwark/base/maps on\nthe SD card.\n");
		for (;;) {
			uint32_t down;
			swiWaitForVBlank();
			scanKeys();
			down = keysDown();
			if (down & KEY_A)
				return sel - 1;
			if (down & KEY_TOUCH) {
				/* a tap on a line picks it: the list starts on the third text row */
				touchPosition tp;
				int row;
				touchRead(&tp);
				row = tp.py / 8 - 2 + top;
				if (tp.py >= 16 && row < entries && row < top + rows)
					return row - 1;
			}
			if ((down & KEY_DOWN) && sel + 1 < entries) {
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

void Nds_InitConsole(void)
{
	videoSetModeSub(MODE_0_2D);
	vramSetBankH(VRAM_H_SUB_BG);
	/* map at 0, font at 16 KB: both fit bank H's 32 KB */
	consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 0, 1, false, true);
}

void Nds_ResetTextures(void)
{
	Mp_Stop();
	glResetTextures();
	Mp_Init();
}

void Nds_Init3D(void)
{
	videoSetMode(MODE_0_3D);
	glInit();
	glEnable(GL_TEXTURE_2D | GL_ANTIALIAS);
	glClearColor(4, 4, 6, 31);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, 255, 191);
	/* textures in banks A and B; C and D take the two-pass captures
	 * (multipass_nds.c), and H holds the bottom screen's text */
	vramSetBankA(VRAM_A_TEXTURE);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankE(VRAM_E_TEX_PALETTE);
	Mp_Init();
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
int Nds_Vblanks(void) { return vblanks; }

/* bench.txt in a base directory lists maps (one file name a line) to time
 * one turn on the spot in each, for comparing builds on hardware */
#define MAX_RESULTS 8
static char results[MAX_RESULTS][64];
static int numresults;

/* a line is a map file name, then optionally a model and a count for a crowd
 * in front of the start: "e1m1.bsp soldier 12" */
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
				snprintf(names[n++], 300, "maps/%s", line);
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
	Ents_Load(havepalette ? palette : NULL);
	return true;
}

/* one frame; returns the vblanks it took */
static bool multipass;

static int frame(const ndscam_t *cam, ndsstats_t *st)
{
	static int last;
	int took;
	if (multipass) {
		Mp_Frame(cam, st);
	} else {
		Mp_Stop();
		Ents_Draw(cam, st);
		Gpu_Counts(&st->hwpolys, &st->hwverts);
		glFlush(0);
		swiWaitForVBlank();
	}
	took = vblanks - last;
	last = vblanks;
	return took;
}

/* the drawing modes off the defaults, for a bench line: u unmerged, p portals, n no portals, 2 two passes, f flicker */
static const char *modes(void)
{
	static char buf[8];
	snprintf(buf, sizeof(buf), "%s%s%s%s", World_MergeFaces ? "" : "u", World_Portals == WORLD_PORTALS_ON ? "p" : World_Portals == WORLD_PORTALS_OFF ? "n" : "", multipass ? "2" : "",
	         Ents_Flicker ? "f" : "");
	return buf;
}

static const char *flickstr(int n)
{
	static char buf[12];
	snprintf(buf, sizeof(buf), " f%d", n);
	return buf;
}

static void bench(const char *line)
{
	ndscam_t cam;
	ndsstats_t st;
	const int steps = 512;
	char path[300], model[40] = "";
	int start, worst = 0, sumpolys = 0, peakpolys = 0, peakverts = 0, peakents = 0, peakflicker = 0, skipped = 0, count = 0;
	if (!strncmp(line, "maps/multipass ", 15)) {
		multipass = !strcmp(line + 15, "on");
		return;
	}
	if (!strncmp(line, "maps/portals ", 13)) {
		World_Portals = !strcmp(line + 13, "on") ? WORLD_PORTALS_ON : !strcmp(line + 13, "off") ? WORLD_PORTALS_OFF : WORLD_PORTALS_AUTO;
		return;
	}
	if (!strncmp(line, "maps/merge ", 11)) {
		World_MergeFaces = strcmp(line + 11, "off") != 0;
		if (atoi(line + 11) > 0)
			World_MergeLight = atoi(line + 11);
		return;
	}
	if (!strncmp(line, "maps/skel ", 10)) {
		if (numresults < MAX_RESULTS)
			Skel_Bench(line + 10, results[numresults++], sizeof(results[0]));
		return;
	}
	if (!strncmp(line, "maps/flicker ", 13)) {
		Ents_Flicker = !strcmp(line + 13, "on");
		return;
	}
	if (numresults >= MAX_RESULTS || sscanf(line, "%299s %39s %d", path, model, &count) < 1 || !load_map(path, &cam))
		return;
	if (model[0] && count > 0)
		Ents_AddCrowd(&cam, model, count);
	frame(&cam, &st);
	start = vblanks;
	for (int i = 0; i < steps; i++) {
		int took;
		cam.yaw = (int16_t)(cam.yaw + 32768 / steps);
		took = frame(&cam, &st);
		if (took > worst)
			worst = took;
		peakpolys = st.hwpolys > peakpolys ? st.hwpolys : peakpolys;
		sumpolys += st.hwpolys;
		peakverts = st.hwverts > peakverts ? st.hwverts : peakverts;
		peakents = st.ents > peakents ? st.ents : peakents;
		peakflicker = st.entsflicker > peakflicker ? st.entsflicker : peakflicker;
		skipped += st.skipped;
	}
	snprintf(results[numresults++], sizeof(results[0]), "%s%s%.6s%s%.7s %d/%dfps p%d a%d v%d m%d s%d%s", modes(),
	         modes()[0] ? ":" : "", short_name(path), model[0] ? "+" : "", model,
	         steps * 60 / (vblanks - start), 60 / (worst ? worst : 1), peakpolys, sumpolys / steps, peakverts, peakents, skipped,
	         Ents_Flicker ? flickstr(peakflicker) : "");
	Ents_Free();
	World_Free();
	Nds_ResetTextures();
}

void Nds_PrintResults(void)
{
	if (!numresults)
		return;
	iprintf("\nbench: fps avg/worst, peak GPU\npolys, verts, models; faces cut;\nf: models taking turns\n");
	for (int i = 0; i < numresults; i++)
		iprintf("%s\n", results[i]);
}

static void show_stats(const char *map, const ndscam_t *cam, const ndsstats_t *st, int fps)
{
	consoleClear();
	iprintf("%s  %d fps  %s%s\n", short_name(map), fps, multipass ? "2 passes" : "1 pass", Ents_Flicker ? " flicker" : "");
	iprintf("world: faces %d polys %d\n", st->faces, st->polys);
	if (World_MergeFaces)
		iprintf("merged: %d faces joined at load\n", World_MergedFaces());
	iprintf("map: %d polys in all\n", World_TotalPolys());
	if (World_PortalLeaves() >= 0)
		iprintf("portals: %d leaves of %d PVS, %d tests, %d ms load\n", World_PortalLeaves(), World_PvsLeaves(),
		        World_PortalTests(), World_PortalLoadMs());
	else
		iprintf("portals: %d (%d ms), PVS used\n", World_PortalCount(), World_PortalLoadMs());
	iprintf("models: %d drawn, %d tris, %d cut\n", st->ents, st->entpolys, st->entsdropped);
	if (st->entsflicker)
		iprintf("taking turns: %d models\n", st->entsflicker);
	iprintf("GPU: %d polys, %d verts\n", st->hwpolys, st->hwverts);
	if (multipass)
		iprintf("near: %d of %d faces; far %d polys\n", st->split, st->treefaces, st->farpolys);
	if (st->skipped)
		iprintf("over the GPU limit: %d faces\n", st->skipped);
	iprintf("leaf %d  eye %ld %ld %ld\n", st->leaf, (long)(cam->org[0] / 8), (long)(cam->org[1] / 8),
	        (long)(cam->org[2] / 8));
	print_memory();
	iprintf("START: map list  SELECT: mode\n");
	Nds_PrintResults();
}

int main(int argc, char **argv)
{
	static char benchmaps[MAX_RESULTS][300], crowd[40];
	int crowdsize = 0;
	ndscam_t cam;
	ndsstats_t st;
	int sel = 0, frames = 0, lastvb = 0, lasttx = -1, lastty = -1, numbench;
	bool fat, nitro;

	Nds_InitConsole();
	Nds_Init3D();
	irqSet(IRQ_VBLANK, on_vblank);
	iprintf("Bulwark DS\n");
	fat = fatInitDefault();
	nitro = nitroFSInit(NULL);
	Fs_Init();
	iprintf("SD card: %s, ROM files: %s, paks: %d\n", fat ? "yes" : "no", nitro ? "yes" : "no", Fs_NumPaks());
	load_palette();
	if (!havepalette)
		iprintf("no gfx/palette.lmp: grey\n");
	find_maps();
	if (argc > 1 && argv[1][0]) {
		snprintf(maps[0], sizeof(maps[0]), "%s", argv[1]);
		nummaps = nummaps ? nummaps : 1;
	}
	if (Link_AutoJoin())
		Mp_Stop(), Link_Run(havepalette ? palette : NULL);
	numbench = read_bench(benchmaps, MAX_RESULTS);
	for (int i = 0; i < numbench; i++)
		bench(benchmaps[i]);
	/* a list ending in a skeleton stays on it: "skel <file> <copies>" (docs/platforms/nds.md) */
	if (numbench && !strncmp(benchmaps[numbench - 1], "maps/skel ", 10)) {
		Skel_View(benchmaps[numbench - 1] + 10);
		numbench = 0;
	}

	for (bool first = true;; first = false) {
		const char *path;
		if (first && numbench) {
			char line[300];
			snprintf(line, sizeof(line), "%s", benchmaps[numbench - 1]);
			sscanf(line, "%299s %39s %d", benchmaps[numbench - 1], crowd, &crowdsize);
			path = benchmaps[numbench - 1]; /* stay in the last map, with its crowd */
		} else {
			int picked = pick_map(nummaps ? sel + 1 : 0);
			if (picked < 0) {
				Mp_Stop(), Link_Run(havepalette ? palette : NULL);
				continue;
			}
			sel = picked;
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
		if (crowd[0] && crowdsize > 0)
			Ents_AddCrowd(&cam, crowd, crowdsize), crowd[0] = 0;
		print_memory();
		for (;;) {
			scanKeys();
			if (keysDown() & KEY_START)
				break;
			/* SELECT steps through 1 pass, 1 pass + flicker, 2 passes, 2 passes + flicker */
			if (keysDown() & KEY_SELECT) {
				multipass ^= Ents_Flicker;
				Ents_Flicker = !Ents_Flicker;
			}
			fly(&cam, keysHeld(), &lasttx, &lastty);
			frame(&cam, &st);
			if (++frames >= 60) {
				show_stats(path, &cam, &st, frames * 60 / (vblanks - lastvb > 0 ? vblanks - lastvb : 1));
				frames = 0;
				lastvb = vblanks;
			}
		}
		Ents_Free();
		World_Free();
		Nds_ResetTextures();
	}
	return 0;
}
