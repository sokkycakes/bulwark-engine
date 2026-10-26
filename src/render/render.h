/* Bulwark engine - renderer interface. ISC licence, see LICENSE.
 *
 * The client owns the model list (cl.models); the renderer keeps its own
 * data for each index and draws what the client game hands it through the
 * scene builtins (clearscene, addentity, renderscene, ...). Without a
 * window (headless) every call still works, nothing is drawn, and queries
 * such as model bounds and string widths still answer. */
#ifndef BW_RENDER_H
#define BW_RENDER_H

#include "../common/common.h"
#include "../qcvm/qcvm.h"

extern const qc_builtin_t r_builtins[];

bool R_Init(bool headless);
void R_Shutdown(void);
bool R_Active(void);           /* true when there is a window to draw into */
void R_GetSize(int *w, int *h);

void R_PrecacheModel(int index); /* cl.models[index] was just named */
bool R_ModelBounds(int index, float *mins, float *maxs);
void R_ClearMap(void);
void R_NewMap(void);             /* cl.world is loaded */

void R_BeginFrame(void);
void R_EndFrame(void);          /* draws the console and presents */

/* 2D drawing for other modules (UI toolkits and the like). Coordinates are
 * virtual pixels from the top left, as R_GetSize reports them; colours are
 * 0-255 RGBA. Calls are only valid between R_BeginFrame and R_EndFrame. */
typedef struct {
	float x, y;
	float s, t;
	uint8_t rgba[4];
} r2dvert_t;

enum { R2D_ALPHA, R2D_ADD, R2D_PREMULTIPLIED, R2D_MULTIPLY };

int R_ImageHandle(const char *name);    /* image or material by name, 0 if none */
int R_UploadImage(const char *name, const uint8_t *rgba, int w, int h); /* creates or replaces */
void R_UpdateImage(int handle, int x, int y, int w, int h, const uint8_t *rgba); /* a region, tightly packed */
void R_ImageSize(int handle, int *w, int *h);
float R_Get2DScale(void); /* framebuffer pixels per virtual pixel: lay out at R_GetSize * scale for sharp text */
void R_Draw2DTriangles(int image, const r2dvert_t *verts, int numverts, const uint16_t *idx, int numidx,
                       int blend);      /* image 0 draws untextured */
void R_Set2DClip(float x, float y, float w, float h);
void R_Reset2DClip(void);

#endif
