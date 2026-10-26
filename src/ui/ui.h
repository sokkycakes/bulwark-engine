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
	UI_INPUT = 1 /* the view takes keyboard and mouse input */
};

/* scripts reach the engine through Bulwark.Send(action, arg); "cmd" runs a
 * console command, everything else goes to this hook (the client game) */
typedef void (*ui_action_fn)(int view, const char *action, const char *arg);

void UI_Init(void);
void UI_Shutdown(void);
void UI_SetActionHandler(ui_action_fn fn);
void UI_InitQC(void); /* sends script actions to CSQC_UIAction */

int UI_Open(const char *layout, int flags); /* returns a view number, 0 on failure */
void UI_Close(int view);
void UI_CloseAll(void);
bool UI_IsOpen(int view);
/* calls every Bulwark.On(event, fn) handler in the view with the decoded
 * JSON value (json may be NULL or empty for no value) */
void UI_Post(int view, const char *event, const char *json);
void UI_Eval(int view, const char *js);

bool UI_WantsInput(void);
bool UI_KeyEvent(int key, bool down, int unicode); /* true when a view used it */
void UI_MouseMove(float x, float y);                /* window pixels */

void UI_Frame(double dt);
void UI_Draw(void); /* between R_BeginFrame and R_EndFrame */

extern const qc_builtin_t ui_builtins[];

#ifdef __cplusplus
}
#endif

#endif
