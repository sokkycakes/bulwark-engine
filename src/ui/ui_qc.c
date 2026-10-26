/* Bulwark engine - QuakeC access to UI views. ISC licence, see LICENSE.
 *
 *   float(string layout, float flags) ui_open      flags: 1 = takes input
 *   void(float view) ui_close
 *   float(float view) ui_isopen
 *   void(float view, string event, string json) ui_post
 *
 * Scripts call back with Bulwark.Send(action, arg), which reaches
 *   void(float view, string action, string arg) CSQC_UIAction
 * in the client game when it defines one. These names are Bulwark's own;
 * see docs/spec/ui.md. */
#include "ui.h"
#include "../client/client.h"

static void ui_open(qcvm_t *vm)
{
	QC_ReturnFloat(vm, (float)UI_Open(QC_PSTR(vm, 0), vm->argc > 1 ? (int)QC_PFLOAT(vm, 1) : 0));
}

static void ui_close(qcvm_t *vm) { UI_Close((int)QC_PFLOAT(vm, 0)); }

static void ui_isopen(qcvm_t *vm) { QC_ReturnFloat(vm, UI_IsOpen((int)QC_PFLOAT(vm, 0)) ? 1 : 0); }

static void ui_post(qcvm_t *vm)
{
	UI_Post((int)QC_PFLOAT(vm, 0), QC_PSTR(vm, 1), vm->argc > 2 ? QC_PSTR(vm, 2) : NULL);
}

const qc_builtin_t ui_builtins[] = {
	{"ui_open", ui_open}, {"ui_close", ui_close}, {"ui_isopen", ui_isopen}, {"ui_post", ui_post}, {NULL, NULL}};

static void action_to_csqc(int view, const char *action, const char *arg)
{
	qcvm_t *vm = cl.csqc;
	int fn;
	if (!vm || (fn = QC_FindFunction(vm, "CSQC_UIAction")) <= 0) {
		Con_DPrintf("ui: view %d sent %s(%s) and nothing handles it\n", view, action, arg);
		return;
	}
	vm->globals[OFS_PARM(0)].f = (float)view;
	vm->globals[OFS_PARM(1)].i = QC_NewString(vm, action);
	vm->globals[OFS_PARM(2)].i = QC_NewString(vm, arg);
	vm->argc = 3;
	QC_Execute(vm, fn);
}

void UI_InitQC(void) { UI_SetActionHandler(action_to_csqc); }
