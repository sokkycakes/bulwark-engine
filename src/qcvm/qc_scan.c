/* Bulwark engine - remembered answers for QC's string search loops.
 *
 * Nuclide finds an entity def by comparing its class name with every entry
 * of a global table (QCOP_FIND_S: for (i = ..; i < n; i++) if (s ==
 * table[i].f)), about 160 compares a lookup and dozens of lookups a server
 * frame, and the table does not change after the map loads. So each such
 * loop keeps, for every entry it has read, the handle it found there and an
 * id for that handle's text; texts that are equal share an id. A later run
 * then only checks that each entry still holds the same handle and compares
 * ids, and the interpreter skips straight to the last iteration it would
 * have run, which it then runs itself, so every temporary ends as before.
 *
 * A handle's text can change only through a character store
 * (qc_string_writes) or when the collector frees its slot and a new string
 * takes it (qc_string_frees); either empties what the caches know about
 * handles. A runtime handle whose slot is free is compared as the loop
 * would, every time.
 *
 * ISC licence, see LICENSE. */
#include "qc_internal.h"

#include <string.h>

#define ID_UNKNOWN (-1)
#define ID_UNFIXED (-2) /* no fixed text: compare it each time */
#define FIND_MAX_ENTRIES 16384
#define FIND_MAX_CACHES 64

typedef struct {
	uint32_t hash;
	int len;
	int id;
	char *text;
} findtext_t;

/* the handle an entry held when its text got its id */
typedef struct {
	int32_t handle, id;
} findent_t;

struct qc_findcache_s {
	int progs, stmt;
	bool bad; /* not the plain shape this needs: the loop runs as it is */
	int cap;
	findent_t *ent;
	findtext_t *texts; /* open addressing, tsize slots */
	int tsize, ntexts;
	uint32_t epoch; /* bumped each time the texts start over */
	uint32_t writes, frees;
	int numprogs;
};

static uint32_t text_hash(const char *s, int len)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < len; i++)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

static void texts_clear(qc_findcache_t *c)
{
	for (int i = 0; i < c->tsize; i++)
		Mem_Free(c->texts[i].text);
	Mem_Free(c->texts);
	c->texts = NULL;
	c->tsize = c->ntexts = 0;
	c->epoch++;
	for (int i = 0; i < c->cap; i++)
		c->ent[i].id = ID_UNKNOWN;
}

/* the id of a text, made if it is new */
static int text_id(qc_findcache_t *c, const char *s)
{
	int len = (int)strlen(s);
	uint32_t h = text_hash(s, len);
	unsigned i;
	if (c->ntexts * 2 >= c->tsize) {
		/* a table that keeps growing is not a fixed table: start over */
		if (c->ntexts > 4 * c->cap + 256)
			texts_clear(c);
		int size = c->tsize ? c->tsize * 2 : 256;
		findtext_t *old = c->texts;
		int oldsize = c->tsize;
		c->texts = Mem_Alloc(sizeof(*c->texts) * (size_t)size);
		c->tsize = size;
		for (int k = 0; k < oldsize; k++) {
			if (!old[k].text)
				continue;
			for (i = old[k].hash & (unsigned)(size - 1); c->texts[i].text; i = (i + 1) & (unsigned)(size - 1))
				;
			c->texts[i] = old[k];
		}
		Mem_Free(old);
	}
	for (i = h & (unsigned)(c->tsize - 1); c->texts[i].text; i = (i + 1) & (unsigned)(c->tsize - 1))
		if (c->texts[i].hash == h && c->texts[i].len == len && !memcmp(c->texts[i].text, s, (size_t)len))
			return c->texts[i].id;
	c->texts[i].hash = h;
	c->texts[i].len = len;
	c->texts[i].id = c->ntexts++;
	c->texts[i].text = Mem_Alloc((size_t)len + 1);
	memcpy(c->texts[i].text, s, (size_t)len);
	return c->texts[i].id;
}

/* the text of handle h if it stays the same until a character store or a
 * collection that frees something, else NULL */
static const char *fixed_text(qcvm_t *vm, int32_t h)
{
	uint32_t u = (uint32_t)h;
	if (u & QC_STRING_RUNTIME) {
		u &= ~QC_STRING_RUNTIME;
		return u && u < qc_nstrs ? qc_strs[u] : NULL;
	}
	return QC_GetString(vm, h);
}

/* 0 LT_I i n T0, 1 IFNOT T0, 2 BOUNDCHECK i, 3 MUL_I i stride T3, 4 LOADA
 * table T3 T4, 5 EQ_S key T4 T5 (either way round), 6 IFNOT T5; step ADD_I
 * i one i. What the loop writes must not be what it reads as fixed. */
static bool find_shape(const qcstatement_t *s0, const qcstatement_t *step)
{
	uint32_t iv = s0[0].a, key = s0[5].a == s0[4].c ? s0[5].b : s0[5].a;
	uint32_t w[5] = {s0[0].c, s0[3].c, s0[4].c, s0[5].c, iv};
	uint32_t in[4] = {s0[0].b, s0[3].b, key, step->b};
	if (s0[1].a != s0[0].c || s0[2].a != iv || s0[3].a != iv || s0[4].b != s0[3].c ||
	    (s0[5].a != s0[4].c && s0[5].b != s0[4].c) || s0[6].a != s0[5].c || step->a != iv || step->c != iv ||
	    QC_PlainOp(step->op) != OP_ADD_I)
		return false;
	for (int k = 0; k < 4; k++)
		for (int j = 0; j < 5; j++)
			if (in[k] == w[j])
				return false;
	for (int j = 0; j < 4; j++)
		if (w[j] == iv)
			return false;
	return true;
}

/* 0 LT_IF c limit T0, 1 IFNOT T0, 2 CONV_IF c T2, 3 CALL1H fn T2, 4 EQ_S
 * RETURN key T4 (either way round), 5 IFNOT T4; step ADD_I c k c. The
 * words after T2, which the call copies into the parameter with it, must
 * stay as they were too. */
static bool call_scan_shape(const qcstatement_t *s0, const qcstatement_t *step)
{
	uint32_t cv = s0[0].a, key = s0[4].a == OFS_RETURN ? s0[4].b : s0[4].a;
	uint32_t w[5] = {s0[0].c, s0[2].c, s0[4].c, cv, OFS_RETURN};
	uint32_t in[6] = {s0[0].b, key, step->b, s0[3].a, s0[2].c + 1, s0[2].c + 2};
	if (s0[1].a != s0[0].c || s0[2].a != cv || s0[3].b != s0[2].c || (s0[4].a != OFS_RETURN && s0[4].b != OFS_RETURN) ||
	    s0[5].a != s0[4].c || step->a != cv || step->c != cv || QC_PlainOp(step->op) != OP_ADD_I)
		return false;
	for (int k = 0; k < 6; k++) {
		for (int j = 0; j < 5; j++)
			if (in[k] == w[j])
				return false;
		if (in[k] >= OFS_PARM(0) && in[k] < OFS_PARM(0) + 3)
			return false;
	}
	return true;
}

/* the cache of the loop at statement sindex of the running progs, made
 * the first time; NULL when there is no room */
static qc_findcache_t *find_cache(qcvm_t *vm, int sindex, const qcstatement_t *s0, const qcstatement_t *step,
                                  bool (*shape)(const qcstatement_t *, const qcstatement_t *))
{
	struct qc_extra_s *x = vm->extra;
	qc_findcache_t *c;
	for (int i = 0; i < x->numfindcaches; i++) {
		c = &x->findcaches[i];
		if (c->stmt == sindex && c->progs == vm->curprogs)
			return c;
	}
	if (x->numfindcaches >= FIND_MAX_CACHES)
		return NULL;
	if (!x->findcaches)
		x->findcaches = Mem_Alloc(sizeof(*x->findcaches) * FIND_MAX_CACHES);
	c = &x->findcaches[x->numfindcaches++];
	memset(c, 0, sizeof(*c));
	c->progs = vm->curprogs;
	c->stmt = sindex;
	c->bad = !shape(s0, step);
	return c;
}

int QC_FindSkip(qcvm_t *vm, const qcstatement_t *s0, const qcstatement_t *step, int sindex)
{
	qcval_t *G = vm->globals;
	qc_findcache_t *c = find_cache(vm, sindex, s0, step, find_shape);
	int32_t i0, n, stride, base, lo, hi, key, kid, r;
	uint32_t epoch;
	const char *kt;
	if (!c || c->bad)
		return 0;
	i0 = G[s0[0].a].i;
	n = G[s0[0].b].i;
	if (i0 >= n - 1)
		return 0; /* nothing to skip */
	stride = G[s0[3].b].i;
	base = (int32_t)s0[4].a;
	lo = (int32_t)s0[2].c;
	hi = (int32_t)s0[2].b;
	/* every index in range, every entry inside the globals, and none of
	 * them a word the loop writes: else the loop runs as it is */
	if (G[step->b].i != 1 || i0 < 0 || i0 < lo || n > hi || n > FIND_MAX_ENTRIES || stride <= 0 ||
	    (int64_t)(n - 1) * stride > INT32_MAX - base || base + (int64_t)(n - 1) * stride + 1 > vm->globalslots)
		return 0;
	{
		uint32_t w[5] = {s0[0].c, s0[3].c, s0[4].c, s0[5].c, s0[0].a};
		for (int j = 0; j < 5; j++)
			if (w[j] >= (uint32_t)base && w[j] < (uint32_t)base + (uint32_t)(n * stride) &&
			    (w[j] - (uint32_t)base) % (uint32_t)stride == 0)
				return 0;
	}
	if (n > c->cap) {
		int cap = c->cap ? c->cap : 64;
		while (cap < n)
			cap *= 2;
		c->ent = Mem_Realloc(c->ent, sizeof(*c->ent) * (size_t)cap);
		for (int i = c->cap; i < cap; i++) {
			c->ent[i].handle = 0;
			c->ent[i].id = ID_UNKNOWN;
		}
		c->cap = cap;
	}
	if (c->writes != qc_string_writes || c->frees != qc_string_frees || c->numprogs != vm->numprogs) {
		c->writes = qc_string_writes;
		c->frees = qc_string_frees;
		c->numprogs = vm->numprogs;
		for (int i = 0; i < c->cap; i++)
			c->ent[i].id = ID_UNKNOWN;
	}
	key = G[s0[5].a == s0[4].c ? s0[5].b : s0[5].a].i;
	kt = QC_GetString(vm, key);
	kid = text_id(c, kt);
	epoch = c->epoch;
	{
		const qcval_t *p = G + base;
		findent_t *e = c->ent;
		for (r = i0; r < n; r++) {
			int32_t h = p[r * stride].i;
			if (h != e[r].handle || e[r].id < 0) {
				const char *t;
				if (h == e[r].handle && e[r].id == ID_UNFIXED) {
					if (!strcmp(QC_GetString(vm, h), kt))
						break;
					continue;
				}
				t = fixed_text(vm, h);
				e[r].handle = h;
				e[r].id = t ? text_id(c, t) : ID_UNFIXED;
				if (c->epoch != epoch) {
					/* the texts started over: the key needs its id again */
					epoch = c->epoch;
					kid = text_id(c, kt);
				}
				if (e[r].id == ID_UNFIXED ? !strcmp(QC_GetString(vm, h), kt) : e[r].id == kid)
					break;
				continue;
			}
			if (e[r].id == kid)
				break;
		}
	}
	/* the loop runs the matching iteration, or the last one, itself */
	return (r < n ? r : n - 1) - i0;
}

void QC_FreeFindCaches(qcvm_t *vm)
{
	struct qc_extra_s *x = vm->extra;
	for (int i = 0; i < x->numfindcaches; i++) {
		qc_findcache_t *c = &x->findcaches[i];
		texts_clear(c);
		Mem_Free(c->ent);
	}
	Mem_Free(x->findcaches);
	x->findcaches = NULL;
	x->numfindcaches = 0;
}

/* QCOP_CALL_SCAN over argv() (see call_scan_shape): the iterations whose
 * word is not the key are skipped, each making its string as argv() would;
 * the loop runs the one that matches, or the last, itself, which writes
 * every word they wrote. */
int QC_CallScanSkip(qcvm_t *vm, const qcstatement_t *s0, const qcstatement_t *step, int sindex, qc_builtin_fn fn,
                    int maxskip)
{
	qcval_t *G = vm->globals;
	qc_findcache_t *c = find_cache(vm, sindex, s0, step, call_scan_shape);
	uint32_t key = s0[4].a == OFS_RETURN ? s0[4].b : s0[4].a;
	if (!c || c->bad)
		return 0;
	return QC_ArgvScanSkip(vm, fn, G[s0[0].a].i, G[step->b].i, G[s0[0].b].f, G[key].i, maxskip);
}
