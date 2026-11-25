/* Bulwark engine - Panorama-style UI views (XML layouts, CSS, JavaScript),
 * run by the vendored PanoramaEngine library and drawn through the
 * renderer's 2D API. ISC licence, see LICENSE.
 *
 * A view is one loaded layout document. Views draw in the order they were
 * opened, after the game's own 2D. A view opened with UI_INPUT takes keys
 * and the mouse before the game does. Layouts, styles, scripts, images and
 * fonts are read through the file system under panorama/, so
 * "layout/titlemenu.xml" means panorama/layout/titlemenu.xml in a game dir. */
#ifndef BW_UI_H
#define BW_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../common/common.h"
#include "../qcvm/qcvm.h"

enum {
	UI_INPUT = 1, /* the view takes keyboard and mouse input */
	UI_EMBED = 2, /* not drawn with the others: UI_DrawAt puts it where it is wanted */
	UI_SEATS = 4  /* with UI_INPUT: also takes split screen guests' keys in a game */
};

/* scripts reach the engine through Bulwark.Send(action, arg); "cmd" runs a
 * console command, everything else goes to this hook (the client game) */
typedef void (*ui_action_fn)(int view, const char *action, const char *arg);

void UI_Init(void);
void UI_Shutdown(void);
void UI_SetActionHandler(ui_action_fn fn);
/* fte_query(request) in a script returns what this writes to reply */
typedef void (*ui_query_fn)(int view, const char *request, strbuf_t *reply);
void UI_SetQueryHandler(ui_query_fn fn);
void UI_InitQC(void); /* sends script actions to CSQC_UIAction or Menu_UIAction */
void UI_CloseMenuViews(void); /* closes the views the menu progs opened */

int UI_Open(const char *layout, int flags); /* returns a view number, 0 on failure */
/* as UI_Open, running bootstrap (JavaScript, may be NULL) before the layout's scripts */
int UI_OpenEx(const char *layout, int flags, const char *bootstrap);
bool UI_LayoutExists(const char *layout);
void UI_SetFlags(int view, int flags);
int UI_Flags(int view);
/* draws a UI_EMBED view into a rectangle of virtual pixels, laid out at that
 * size; rgb and alpha tint it */
void UI_DrawAt(int view, float x, float y, float w, float h, const float *rgb, float alpha);
void UI_ViewSize(int view, float *w, float *h); /* virtual pixels */
/* registers a font for CSS font-family: family, at a weight; path is a
 * game file. Views opened after this can use it. */
bool UI_AddFont(const char *family, const char *path, int weight);
void UI_Close(int view);
void UI_CloseAll(void);
bool UI_IsOpen(int view);
/* calls every Bulwark.On(event, fn) handler in the view with the decoded
 * JSON value (json may be NULL or empty for no value) */
void UI_Post(int view, const char *event, const char *json);
void UI_Eval(int view, const char *js);

bool UI_WantsInput(void);
bool UI_WantsSeatInput(void); /* the input view takes guests' keys (UI_SEATS) */
/* who the next key events come from: the device (0 keyboard and mouse,
 * 1.. a controller slot + 1) and its split screen seat (-1 none); key
 * events tell scripts as "device" and "seat" */
void UI_SetKeySource(int device, int seat);
bool UI_KeyEvent(int key, bool down, int unicode); /* true when a view used it */
void UI_MouseMove(float x, float y);                /* window pixels */

void UI_Frame(double dt);
void UI_Draw(void); /* between R_BeginFrame and R_EndFrame */

extern const qc_builtin_t ui_builtins[];

#ifdef __cplusplus
}
#endif

#endif
