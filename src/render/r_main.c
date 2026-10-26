/* Bulwark engine - renderer front end. ISC licence, see LICENSE. */
#include "render.h"
#include "../client/client.h"

static bool r_active;

bool R_Init(bool headless) { r_active = !headless; return true; }
void R_Shutdown(void) {}
bool R_Active(void) { return r_active; }
void R_GetSize(int *w, int *h) { *w = 640; *h = 480; }
void R_PrecacheModel(int index) {}
bool R_ModelBounds(int index, float *mins, float *maxs) { return false; }
void R_ClearMap(void) {}
void R_NewMap(void) {}
void R_BeginFrame(void) {}
void R_EndFrame(void) {}

const qc_builtin_t r_builtins[] = {{NULL, NULL}};
