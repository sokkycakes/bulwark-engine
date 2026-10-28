/* Bulwark engine - uri_get (DP_QC_URI_GET): fetch a URL in the background
 * and hand the body to the game's URI_Get_Callback. Shared by the server,
 * client and menu modules. The builtin's contract is its doc comment in
 * Nuclide's fteextensions.qc: it returns 1 when the request started, then
 * the callback gets the caller's id, 0 or the HTTP status, the body as a
 * string and its length. POST (the third and fourth arguments) needs a
 * request body the HTTP client cannot send yet, so it fails with 0.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../net/http.h"

#define MAX_URI_REQUESTS 32

typedef struct {
	qcvm_t *vm;
	http_t *http;
	float id;
} urireq_t;

static urireq_t reqs[MAX_URI_REQUESTS];

static void bi_uri_get(qcvm_t *vm)
{
	const char *url = QC_PSTR(vm, 0);
	if (vm->argc > 3 && *QC_PSTR(vm, 3)) {
		Con_DPrintf("uri_get: POST to %s is not supported\n", url);
		QC_ReturnFloat(vm, 0);
		return;
	}
	for (int i = 0; i < MAX_URI_REQUESTS; i++) {
		if (reqs[i].http)
			continue;
		if (!(reqs[i].http = HTTP_Get(url, NULL)))
			break;
		reqs[i].vm = vm;
		reqs[i].id = QC_PFLOAT(vm, 1);
		QC_ReturnFloat(vm, 1);
		return;
	}
	QC_ReturnFloat(vm, 0);
}

const qc_builtin_t qc_uri_builtins[] = {{"uri_get", bi_uri_get}, {"uri_post", bi_uri_get}, {NULL, NULL}};

static bool vm_alive(qcvm_t *vm)
{
	for (int i = 0; i < QC_VMCount(); i++)
		if (QC_VMAt(i) == vm)
			return true;
	return false;
}

void QC_URIFrame(void)
{
	for (int i = 0; i < MAX_URI_REQUESTS; i++) {
		urireq_t *r = &reqs[i];
		int state;
		if (!r->http)
			continue;
		if (!vm_alive(r->vm)) {
			HTTP_Free(r->http);
			r->http = NULL;
			continue;
		}
		state = HTTP_State(r->http);
		if (state == HTTP_RUNNING)
			continue;
		{
			size_t len = 0;
			const char *body = state == HTTP_DONE ? HTTP_Body(r->http, &len) : NULL;
			qcvm_t *vm = r->vm;
			int fn = QC_FindFunction(vm, "URI_Get_Callback");
			if (fn > 0) {
				vm->globals[OFS_PARM(0)].f = r->id;
				vm->globals[OFS_PARM(1)].f = state == HTTP_DONE ? (float)HTTP_Code(r->http) : 0;
				vm->globals[OFS_PARM(2)].i = body ? QC_NewStringLen(vm, body, len) : 0;
				vm->globals[OFS_PARM(3)].i = (int32_t)len;
				vm->argc = 4;
				QC_Execute(vm, fn);
			}
		}
		HTTP_Free(r->http);
		r->http = NULL;
	}
}
