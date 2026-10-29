/* Bulwark engine - uri_get (DP_QC_URI_GET): fetch a URL in the background
 * and hand the body to the game's URI_Get_Callback. Shared by the server,
 * client and menu modules. The builtin's contract is its doc comment in
 * Nuclide's fteextensions.qc: it returns 1 when the request started, then
 * the callback gets the caller's id, 0 or the HTTP status, the body as a
 * string and its length. The third and fourth arguments make it a POST
 * (or another method, see bi_uri_get), and the callback then gets any
 * status the server answers with, error bodies included.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cvar.h"
#include "../net/http.h"

#define MAX_URI_REQUESTS 32

typedef struct {
	qcvm_t *vm;
	http_t *http;
	float id;
	int progs; /* the progs that asked, so an addprogs module gets its own answer */
} urireq_t;

static urireq_t reqs[MAX_URI_REQUESTS];

/* SpacetimeDB (or any token-guarded web API): requests the server's QC
 * makes to an address under stdb_url carry "Authorization: Bearer
 * <stdb_token>", added here so the token never passes through QC strings.
 * Neither cvar is saved to config.cfg; set them from a cfg kept out of git. */
static cvar_t *stdb_url, *stdb_token;

static void uri_cvars(void)
{
	if (stdb_url)
		return;
	stdb_url = Cvar_Get("stdb_url", "", CVAR_NOSAVE,
	                    "Web API base (e.g. https://maincloud.spacetimedb.com) whose requests from the server's QC carry stdb_token");
	stdb_token = Cvar_Get("stdb_token", "", CVAR_NOSAVE, "Bearer token sent with the server's requests to stdb_url");
}

/* only the server's code gets the token: CSQC and menu code come from
 * elsewhere, and a listen server shares this process with them */
static bool wants_token(qcvm_t *vm, const char *url)
{
	size_t n;
	uri_cvars();
	n = strlen(stdb_url->string);
	return vm == sv.vm && n && stdb_token->string[0] && !strncmp(url, stdb_url->string, n) &&
	       (stdb_url->string[n - 1] == '/' || url[n] == '/' || url[n] == '?' || !url[n]);
}

/* uri_get(url, id, optional mimetype, optional body): with a body (or a
 * mimetype naming a method, see below) it's a POST. FTE's builtin accepts
 * the same arguments under the name uri_post. As an extension, a mimetype
 * of "METHOD content/type" (e.g. "PUT application/json") picks the method. */
static void bi_uri_get(qcvm_t *vm)
{
	const char *url = QC_PSTR(vm, 0);
	const char *mime = vm->argc > 2 ? QC_PSTR(vm, 2) : "";
	const char *body = vm->argc > 3 ? QC_PSTR(vm, 3) : "";
	bool token = wants_token(vm, url);
	for (int i = 0; i < MAX_URI_REQUESTS; i++) {
		if (reqs[i].http)
			continue;
		if (*body || *mime || token) {
			char method[16] = "GET", headers[2048] = "";
			const char *sp = strchr(mime, ' ');
			if (sp && sp - mime < (int)sizeof(method)) {
				Str_Copy(method, mime, (size_t)(sp - mime) + 1);
				mime = sp + 1;
			} else if (*body || *mime) {
				Str_Copy(method, "POST", sizeof(method));
			}
			if (token)
				snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", stdb_token->string);
			reqs[i].http = HTTP_Request(method, url, *mime ? mime : "application/x-www-form-urlencoded",
			                            strcmp(method, "GET") ? body : NULL, strlen(body), headers);
		} else {
			reqs[i].http = HTTP_Get(url, NULL);
		}
		if (!reqs[i].http)
			break;
		reqs[i].vm = vm;
		reqs[i].id = QC_PFLOAT(vm, 1);
		reqs[i].progs = vm->curprogs;
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
			int fn = r->progs < vm->numprogs ? QC_FindFunctionIn(vm, r->progs, "URI_Get_Callback") : 0;
			if (fn <= 0)
				fn = QC_FindFunction(vm, "URI_Get_Callback");
			if (fn > 0) {
				QC_Parm(vm, 0)->f = r->id;
				QC_Parm(vm, 1)->f = state == HTTP_DONE ? (float)HTTP_Code(r->http) : 0;
				QC_Parm(vm, 2)->i = body ? QC_NewStringLen(vm, body, len) : 0;
				QC_Parm(vm, 3)->i = (int32_t)len;
				vm->argc = 4;
				QC_Execute(vm, fn);
			}
		}
		HTTP_Free(r->http);
		r->http = NULL;
	}
}
