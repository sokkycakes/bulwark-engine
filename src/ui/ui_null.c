/* Bulwark engine - UI views when the build has no PanoramaEngine (consoles,
 * where its C++20 runtime and QuickJS don't fit). Every view fails to open,
 * so games fall back to drawing their menus with the 2D builtins.
 * ISC licence, see LICENSE. */
#include "ui.h"

void UI_Init(void) {}
void UI_Shutdown(void) {}
void UI_SetActionHandler(ui_action_fn fn) { (void)fn; }
void UI_SetQueryHandler(ui_query_fn fn) { (void)fn; }
int UI_Open(const char *layout, int flags) { return UI_OpenEx(layout, flags, NULL); }

int UI_OpenEx(const char *layout, int flags, const char *bootstrap)
{
	(void)flags, (void)bootstrap;
	Con_DPrintf("ui: %s not opened, this build has no UI views\n", layout);
	return 0;
}

bool UI_LayoutExists(const char *layout) { (void)layout; return false; }
void UI_SetFlags(int view, int flags) { (void)view, (void)flags; }
int UI_Flags(int view) { (void)view; return 0; }

void UI_DrawAt(int view, float x, float y, float w, float h, const float *rgb, float alpha)
{
	(void)view, (void)x, (void)y, (void)w, (void)h, (void)rgb, (void)alpha;
}

void UI_ViewSize(int view, float *w, float *h)
{
	(void)view;
	*w = *h = 0;
}

bool UI_AddFont(const char *family, const char *path, int weight) { (void)family, (void)path, (void)weight; return false; }
void UI_Close(int view) { (void)view; }
void UI_CloseAll(void) {}
bool UI_IsOpen(int view) { (void)view; return false; }
void UI_Post(int view, const char *event, const char *json) { (void)view, (void)event, (void)json; }
void UI_Eval(int view, const char *js) { (void)view, (void)js; }
bool UI_WantsInput(void) { return false; }
bool UI_KeyEvent(int key, bool down, int unicode) { (void)key, (void)down, (void)unicode; return false; }
void UI_MouseMove(float x, float y) { (void)x, (void)y; }
void UI_Frame(double dt) { (void)dt; }
void UI_Draw(void) {}
bool UI_PreCommand(int argc, const char **argv) { (void)argc, (void)argv; return false; }
