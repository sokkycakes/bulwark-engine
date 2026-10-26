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

#endif
