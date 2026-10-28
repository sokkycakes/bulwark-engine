/* Bulwark engine - the DS view in two passes, for twice the GPU's 2048
 * polygons a frame at 30 fps. ISC licence, see LICENSE.
 *
 * The GPU draws one frame's polygons per screen refresh. Here a frame takes
 * two refreshes:
 *
 *   refresh 1  the far pass: the faces past the split, and the models out
 *              there. The display capture unit copies the 3D image alone
 *              into VRAM C. The screen meanwhile shows VRAM D, which holds
 *              the last finished frame, so nobody sees half a scene.
 *   refresh 2  the near pass: VRAM C becomes a texture on one quad filling
 *              the screen at the back of the depth range, and the nearest
 *              faces and models are drawn over it. The screen shows this
 *              pass as it is drawn, and the capture unit copies it into
 *              VRAM D for the next refresh 1.
 *
 * The world is walked once a frame, nearest first. Front-to-back BSP order
 * means no face is hidden by a face later in the list, so the near pass
 * takes the first faces that fit a pass and the far pass the rest, and the
 * near image can simply lie over the far one. Models and brush entities go
 * to whichever side of the split face they stand on.
 *
 * VRAM C and D are kept out of texture use (Mp_Init), so textures live in
 * A and B: 256 KB instead of 384 KB.
 */
#include <nds.h>
#include <stdint.h>
#include <string.h>

#include "nds.h"

#define QUAD_POLYS 1
#define QUAD_VERTS 4

static bool running;

void Mp_Init(void)
{
	vramSetBankC(VRAM_C_LCD);
	vramSetBankD(VRAM_D_LCD);
	glLockVRAMBank(VRAM_C);
	glLockVRAMBank(VRAM_D);
}

void Mp_Stop(void)
{
	if (!running)
		return;
	REG_DISPCAPCNT = 0;
	vramSetBankC(VRAM_C_LCD);
	videoSetMode(MODE_0_3D);
	running = false;
}

/* the far pass's picture, from VRAM C as texture slot 2, over the whole screen at the back */
static void draw_backdrop(void)
{
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
	/* direct colour, 256x256, at slot 2 (256 KB into texture memory) */
	GFX_TEX_FORMAT = (0x40000 >> 3) | (TEXTURE_SIZE_256 << 20) | (TEXTURE_SIZE_256 << 23) | (7 << 26);
	glColor(RGB15(31, 31, 31));
	glBegin(GL_QUADS);
	/* clip space; z just short of the far end, so everything nearer covers it */
	glTexCoord2t16(0, 0);
	glVertex3v16(-4096, 4096, 4095);
	glTexCoord2t16(0, 192 << 4);
	glVertex3v16(-4096, -4096, 4095);
	glTexCoord2t16(256 << 4, 192 << 4);
	glVertex3v16(4096, -4096, 4095);
	glTexCoord2t16(256 << 4, 0);
	glVertex3v16(4096, 4096, 4095);
	glEnd();
	glPopMatrix(1);
	glMatrixMode(GL_PROJECTION);
	glPopMatrix(1);
	glMatrixMode(GL_MODELVIEW);
}

void Mp_Frame(const ndscam_t *cam, ndsstats_t *st)
{
	int32_t split2;
	int k, polys, verts, farpolys, farverts;
	running = true;

	World_Begin(cam, st);
	Ents_Select(cam);
	World_Collect();
	/* the near pass holds the backdrop, the view model, and the nearest faces
	 * and models that fit */
	{
		static int32_t mdist[256];
		static int mpolys[256];
		int viewpolys, n = Ents_Costs(mdist, mpolys, 256, &viewpolys);
		k = World_Split(WORLD_MAX_VERTS - viewpolys * 3 - QUAD_VERTS, WORLD_MAX_POLYS - viewpolys - QUAD_POLYS, mdist,
		                mpolys, n, &split2);
	}

	/* the far pass */
	World_SetView(cam);
	polys = World_Cost(k, World_NumTreeFaces(), split2, false, &verts);
	Ents_DrawPass(cam, split2, false, false, Ents_FlickerBudget(polys, verts), st);
	Gpu_Counts(&polys, &verts);
	World_DrawPass(k, World_NumTreeFaces(), split2, false, st, WORLD_MAX_VERTS - verts, WORLD_MAX_POLYS - polys);
	Gpu_Counts(&farpolys, &farverts);
	glFlush(0);
	swiWaitForVBlank();
	/* this refresh draws the far pass: capture it, and show the last frame */
	vramSetBankC(VRAM_C_LCD);
	REG_DISPCNT = MODE_FB3 | ENABLE_3D | DISPLAY_BG0_ACTIVE;
	REG_DISPCAPCNT = DCAP_ENABLE | DCAP_MODE(DCAP_MODE_A) | DCAP_SRC_A(DCAP_SRC_A_3DONLY) |
	                 DCAP_SIZE(DCAP_SIZE_256x192) | DCAP_BANK(DCAP_BANK_VRAM_C);

	/* the near pass */
	draw_backdrop();
	World_SetView(cam);
	polys = World_Cost(0, k, split2, true, &verts);
	Ents_DrawPass(cam, split2, true, true, Ents_FlickerBudget(polys + QUAD_POLYS, verts + QUAD_VERTS), st);
	Gpu_Counts(&polys, &verts);
	World_DrawPass(0, k, split2, true, st, WORLD_MAX_VERTS - verts, WORLD_MAX_POLYS - polys);
	Gpu_Counts(&polys, &verts);
	st->hwpolys = farpolys + polys;
	st->split = k, st->treefaces = World_NumTreeFaces(), st->farpolys = farpolys;
	st->hwverts = farverts + verts;
	glFlush(0);
	swiWaitForVBlank();
	/* this refresh draws the near pass over the far one: show it, and keep a copy */
	vramSetBankC(VRAM_C_TEXTURE_SLOT2);
	videoSetMode(MODE_0_3D);
	REG_DISPCAPCNT = DCAP_ENABLE | DCAP_MODE(DCAP_MODE_A) | DCAP_SRC_A(DCAP_SRC_A_3DONLY) |
	                 DCAP_SIZE(DCAP_SIZE_256x192) | DCAP_BANK(DCAP_BANK_VRAM_D);
}
