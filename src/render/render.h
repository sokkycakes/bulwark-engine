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
/* fn gets the next frame as a JPEG; false when nothing is drawn (headless) */
typedef void (*r_capture_fn)(const void *jpeg, size_t len);
bool R_CaptureScreen(r_capture_fn fn);
/* fn gets every frame's pixels (RGB, rows bottom up) until set to NULL */
typedef void (*r_grab_fn)(const uint8_t *rgb, int w, int h);
void R_SetFrameGrab(r_grab_fn fn);
/* encodes RGB rows bottom up as png, jpg, tga or bmp; false for others */
bool R_EncodeImage(const char *ext, const uint8_t *rgb, int w, int h, int quality, strbuf_t *out);
void R_Shutdown(void);
bool R_Active(void);           /* true when there is a window to draw into */
bool R_CursorMode(void);       /* the focused progs (menu while open, else client game) wants a free cursor */
void R_ResetCursorMode(qcvm_t *vm); /* forget a progs' setcursormode as it unloads */
void R_GetSize(int *w, int *h);

void R_PrecacheModel(int index); /* cl.models[index] was just named */
bool R_ModelBounds(int index, float *mins, float *maxs);
void R_ClearMap(void);
void R_NewMap(void);             /* cl.world is loaded */

/* particle effects by their index in cl.particles */
void R_PointParticles(int index, const float *org, const float *dir, float count);
/* drawpic of a name another module draws itself: return true when it is
 * one. With size non-NULL it is asked for the picture's size instead. */
typedef bool (*r_pichook_t)(const char *name, float x, float y, float w, float h, const float *rgb, float alpha,
                            float *size);
void R_SetPicHook(r_pichook_t fn);
extern const qc_builtin_t r_skin_builtins[];   /* r_skin.c: custom skins, remapshader, r_readimage */
extern const qc_builtin_t r_qclight_builtins[]; /* r_rtlights.c: dynamiclight_* */

/* a client game entity drawn every frame from now on (makestatic) */
void R_MakeStatic(struct qcvm_s *vm, int e);
void R_SpawnFlash(const float *org, float radius, float lifetime, const float *rgb);
void R_TrailParticles(int index, const float *start, const float *end);

/* a click on a showpic picture with a command; true if it ran one */
bool R_ShowpicTouch(float x, float y);

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
uint8_t *R_DecodeImageFile(const uint8_t *data, size_t len, int *w, int *h); /* PNG, JPEG, TGA...: RGBA, Mem_Free it */
float R_Get2DScale(void); /* framebuffer pixels per virtual pixel: lay out at R_GetSize * scale for sharp text */
void R_Draw2DTriangles(int image, const r2dvert_t *verts, int numverts, const uint16_t *idx, int numidx,
                       int blend);      /* image 0 draws untextured */
void R_Set2DClip(float x, float y, float w, float h);
/* 2D drawing between these lands in an off-screen w x h target that End
 * scales over the whole screen with nearest filtering (chunky pixels) */
void R_BeginPixelated2D(int w, int h);
void R_EndPixelated2D(void);
void R_Reset2DClip(void);

/* simple 2D drawing in virtual pixels, for the engine's own menus: text
 * in the console font at a size (8 is the font's own), boxes, pictures */
float R_Text(float x, float y, const char *text, float size, const float *rgb, float alpha);
float R_TextWidth(const char *text, float size);
void R_Rect(float x, float y, float w, float h, const float *rgb, float alpha);
bool R_Picture(float x, float y, float w, float h, const char *name, float alpha); /* false if there's no such picture */
bool R_PictureSize(const char *name, int *w, int *h);

/* the main view's origin and forward direction, from the last frame */
void R_MainView(float origin[3], float forward[3]);
/* $r_editlights_current_* macros (r_editlights); NULL for other names */
const char *R_EditLightsMacro(const char *name);

/* model shots (r_modelshot.c): a model drawn on its own, lit by a studio
 * light on a transparent background, into the image named name (the UI
 * shows it as "$rt:name"). Framed on the model by measuring where it draws;
 * portrait frames head and shoulders. Each call draws it again (call it
 * every frame for a live one, between R_BeginFrame and R_EndFrame). The
 * image exists from the first call, blank while there is no window. */
typedef struct {
	const char *model;
	const char *anim; /* sequence or frame name or number; NULL for the model's idle */
	float time;       /* seconds into it */
	float yaw;        /* the model's turn; 0 faces the camera */
	int width, height;
	bool portrait;
} r_modelshot_t;
bool R_ModelShot(const char *name, const r_modelshot_t *s);
bool R_ModelShotExists(const char *name);
void R_InitModelShots(void);

/* modelviewer and mapedit (r_viewer.c): while active they take keys and the mouse */
bool V_Active(void);
bool V_KeyEvent(int key, bool down);
bool V_MouseMove(float dx, float dy);
void V_Close(void);
void V_Draw(int w, int h);
void V_Init(void);

#endif
