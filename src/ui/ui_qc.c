/* Bulwark engine - QuakeC access to UI views. ISC licence, see LICENSE.
 *
 *   float(string layout, float flags) ui_open      flags: 1 = takes input
 *   void(float view) ui_close
 *   float(float view) ui_isopen
 *   void(float view, string event, string json) ui_post
 *
 * Scripts call back with Bulwark.Send(action, arg), which reaches
 *   void(float view, string action, string arg) CSQC_UIAction
 * in the client game when it defines one, or Menu_UIAction in the menu
 * progs for views the menu progs opened. These names are Bulwark's own;
 * see docs/spec/ui.md. */
#include "ui.h"
#include "../client/client.h"

/* which progs opened each view, so its actions go back there. A view not
 * listed (opened from the console) belongs to the client game. */
#define UI_OWNERS 64
static struct {
	int view;
	qcvm_kind_t kind;
} owners[UI_OWNERS];

static void set_owner(int view, qcvm_kind_t kind)
{
	int i, free_slot = -1;
	for (i = 0; i < UI_OWNERS; i++) {
		if (owners[i].view && !UI_IsOpen(owners[i].view))
			owners[i].view = 0;
		if (!owners[i].view && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		owners[free_slot].view = view;
		owners[free_slot].kind = kind;
	}
}

static bool owned_by_menu(int view)
{
	for (int i = 0; i < UI_OWNERS; i++)
		if (owners[i].view == view)
			return owners[i].kind == QCVM_MENU;
	return false;
}

/* the menu progs is going away: its views go with it */
void UI_CloseMenuViews(void)
{
	for (int i = 0; i < UI_OWNERS; i++)
		if (owners[i].view && owners[i].kind == QCVM_MENU) {
			UI_Close(owners[i].view);
			owners[i].view = 0;
		}
}

static void ui_open(qcvm_t *vm)
{
	int view = UI_Open(QC_PSTR(vm, 0), vm->argc > 1 ? (int)QC_PFLOAT(vm, 1) : 0);
	if (view)
		set_owner(view, vm->kind);
	QC_ReturnFloat(vm, (float)view);
}

static void ui_close(qcvm_t *vm) { UI_Close((int)QC_PFLOAT(vm, 0)); }

static void ui_isopen(qcvm_t *vm) { QC_ReturnFloat(vm, UI_IsOpen((int)QC_PFLOAT(vm, 0)) ? 1 : 0); }

static void ui_post(qcvm_t *vm)
{
	UI_Post((int)QC_PFLOAT(vm, 0), QC_PSTR(vm, 1), vm->argc > 2 ? QC_PSTR(vm, 2) : NULL);
}

/* ui_draw(view, pos, size, alpha): draws a view opened with flag 2
 * (UI_EMBED) now, into a rectangle of the progs' 2D space, laid out at that
 * size. A HUD view drawn this way shows only on the frames the game draws
 * its HUD, and in the order of the game's other 2D. */
static void ui_draw(qcvm_t *vm)
{
	const float *pos = QC_PVEC(vm, 1), *size = QC_PVEC(vm, 2);
	float alpha = vm->argc > 3 ? QC_PFLOAT(vm, 3) : 1;
	UI_DrawAt((int)QC_PFLOAT(vm, 0), pos[0], pos[1], size[0], size[1], NULL, alpha);
}

const qc_builtin_t ui_builtins[] = {{"ui_open", ui_open},     {"ui_close", ui_close}, {"ui_isopen", ui_isopen},
                                    {"ui_post", ui_post},     {"ui_draw", ui_draw},   {NULL, NULL}};

static void action_to_qc(int view, const char *action, const char *arg)
{
	qcvm_t *vm = cl.csqc;
	const char *name = "CSQC_UIAction";
	int fn;
	if (owned_by_menu(view)) {
		vm = MQC_VM();
		name = "Menu_UIAction";
	}
	if (!vm || (fn = QC_FindFunction(vm, name)) <= 0) {
		Con_DPrintf("ui: view %d sent %s(%s) and nothing handles it\n", view, action, arg);
		return;
	}
	vm->globals[OFS_PARM(0)].f = (float)view;
	vm->globals[OFS_PARM(1)].i = QC_NewString(vm, action);
	vm->globals[OFS_PARM(2)].i = QC_NewString(vm, arg);
	vm->argc = 3;
	QC_Execute(vm, fn);
}

void UI_InitQC(void) { UI_SetActionHandler(action_to_qc); }
