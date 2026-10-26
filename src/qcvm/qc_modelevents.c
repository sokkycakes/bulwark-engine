/* Bulwark engine - model events and actions for QC (FTE_GFX_MODELEVENTS and
 * frameforaction, as their doc comments describe them) on GoldSrc studio
 * models. Every VM gets these; a VM finds its models through vm->modelname.
 * How the time windows work is in docs/spec/models.md. ISC licence, see
 * LICENSE. */
#include "qc_internal.h"
#include "../common/studio.h"

#include <stdlib.h>

#define MAX_EVENTS 256

typedef struct {
	float time;
	int code;
	const char *data;
} modelevent_t;

const studio_model_t *QC_StudioModel(qcvm_t *vm, int modelindex)
{
	const char *name = vm->modelname ? vm->modelname(vm, modelindex) : NULL;
	return name ? Studio_Cached(name) : NULL;
}

static const studio_seq_t *seq_arg(qcvm_t *vm, const studio_model_t **mp)
{
	const studio_model_t *m = QC_StudioModel(vm, (int)QC_PFLOAT(vm, 0));
	int seq = (int)QC_PFLOAT(vm, 1);
	*mp = m;
	return m && seq >= 0 && seq < m->numseqs ? &m->seqs[seq] : NULL;
}

static bool in_window(float t, float lo, float hi)
{
	/* (lo, hi], except that a window starting at 0 takes events on frame 0 */
	return (t > lo || (lo <= 0 && t >= 0)) && t <= hi;
}

static int cmp_event(const void *a, const void *b)
{
	float d = ((const modelevent_t *)a)->time - ((const modelevent_t *)b)->time;
	return d < 0 ? -1 : d > 0;
}

/* events of seq between base and target in time order. A target before the
 * base means the animation started over: the window is then 0 to target.
 * Looping sequences repeat their events every duration. */
static int collect(const studio_model_t *m, const studio_seq_t *s, float base, float target, modelevent_t *out)
{
	int n = 0, seq = (int)(s - m->seqs);
	float dur = Studio_SeqDuration(m, seq);
	int first = 0, last = 0;
	if (target < base)
		base = 0;
	if (target <= base && !(base <= 0 && target >= 0))
		return 0;
	if ((s->flags & STUDIO_LOOPING) && dur > 0) {
		first = (int)(base / dur);
		last = (int)(target / dur);
		if (last - first > 4)
			first = last - 4; /* a long gap: only the last few laps matter */
	}
	for (int lap = first; lap <= last; lap++)
		for (int i = 0; i < s->numevents && n < MAX_EVENTS; i++) {
			float t = (s->fps > 0 ? s->events[i].frame / s->fps : 0) + lap * dur;
			if (!in_window(t, base, target))
				continue;
			out[n].time = t;
			out[n].code = s->events[i].event;
			out[n].data = s->events[i].options;
			n++;
		}
	qsort(out, (size_t)n, sizeof(out[0]), cmp_event);
	return n;
}

/* void(float modidx, float framenum, __inout float basetime, float targettime,
 *      void(float timestamp, int code, string data) callback) */
static void bi_processmodelevents(qcvm_t *vm)
{
	const studio_model_t *m;
	const studio_seq_t *s = seq_arg(vm, &m);
	float base = QC_PFLOAT(vm, 2), target = QC_PFLOAT(vm, 3);
	func_t callback = QC_PINT(vm, 4);
	int selfofs = QC_GlobalOfs(vm, "self");
	int32_t self = selfofs >= 0 ? vm->globals[selfofs].i : 0;
	modelevent_t ev[MAX_EVENTS];
	int n = s ? collect(m, s, base, target, ev) : 0;
	for (int i = 0; i < n && callback; i++) {
		QC_PFLOAT(vm, 0) = ev[i].time;
		QC_PINT(vm, 1) = ev[i].code;
		QC_PINT(vm, 2) = QC_NewString(vm, ev[i].data);
		vm->argc = 3;
		QC_Execute(vm, callback);
		if (selfofs >= 0)
			vm->globals[selfofs].i = self;
	}
	/* __inout: the caller reads basetime back from its parameter slot */
	QC_PFLOAT(vm, 2) = target;
}

/* float(float modidx, float framenum, __inout float basetime, float targettime,
 *       __out int code, __out string data) */
static void bi_getnextmodelevent(qcvm_t *vm)
{
	const studio_model_t *m;
	const studio_seq_t *s = seq_arg(vm, &m);
	float base = QC_PFLOAT(vm, 2), target = QC_PFLOAT(vm, 3);
	modelevent_t ev[MAX_EVENTS];
	int n = s ? collect(m, s, base, target, ev) : 0;
	if (n) {
		QC_PFLOAT(vm, 2) = ev[0].time;
		QC_PINT(vm, 4) = ev[0].code;
		QC_PINT(vm, 5) = QC_NewString(vm, ev[0].data);
	} else {
		QC_PFLOAT(vm, 2) = target;
	}
	QC_ReturnFloat(vm, n ? 1 : 0);
}

/* float(float modidx, float framenum, int eventidx, __out float timestamp,
 *       __out int code, __out string data) */
static void bi_getmodeleventidx(qcvm_t *vm)
{
	const studio_model_t *m;
	const studio_seq_t *s = seq_arg(vm, &m);
	int idx = QC_PINT(vm, 2);
	if (!s || idx < 0 || idx >= s->numevents) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	QC_PFLOAT(vm, 3) = s->fps > 0 ? s->events[idx].frame / s->fps : 0;
	QC_PINT(vm, 4) = s->events[idx].event;
	QC_PINT(vm, 5) = QC_NewString(vm, s->events[idx].options);
	QC_ReturnFloat(vm, 1);
}

/* float(float modidx, int actionid) */
static void bi_frameforaction(qcvm_t *vm)
{
	const studio_model_t *m = QC_StudioModel(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, (float)Studio_SeqForActivity(m, QC_PINT(vm, 1), (unsigned)rand()));
}

const qc_builtin_t qc_builtins_modelevents[] = {
	{"processmodelevents", bi_processmodelevents},
	{"getnextmodelevent", bi_getnextmodelevent},
	{"getmodeleventidx", bi_getmodeleventidx},
	{"frameforaction", bi_frameforaction},
	{NULL, NULL}};
