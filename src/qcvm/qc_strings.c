/* Bulwark engine - QuakeC string handles and garbage collection.
 *
 * QC code written for this engine family treats every string it gets from a
 * builtin as persistent: it stores sprintf() results in fields without any
 * strzone() call. So engine-made strings live in one process-wide table and
 * are reclaimed by a conservative mark and sweep that runs between QC calls.
 * The mark phase scans every word QC can reach (globals, entity fields, the
 * saved-locals stack, memalloc blocks, hash table values) and keeps any word
 * that looks like a live handle. A float that happens to share the bit pattern
 * only delays a free, it never frees something in use.
 *
 * ISC licence, see LICENSE. */
#include "qc_internal.h"

/* A slot holds the text of one string, NULL when free. The length sits in
 * the word just before the text, in the same allocation, so a slot is one
 * pointer: a map load can make a quarter of a million strings before the
 * first collection, and the table is kept for the rest of the session. */
char **qc_strs;
uint32_t qc_nstrs;
static uint32_t capstrs;
/* shorter names for them in this file */
#define strs qc_strs
#define nstrs qc_nstrs
static uint8_t *marks; /* one bit a slot, for the collector */
/* free slots below nstrs; all slots below freescan are taken, so the
 * lowest free one is reused first and the table can shrink again */
static uint32_t nfree, freescan = 1;
/* every slot in use, so a collection only visits live and new strings:
 * a map load can leave millions of slots, almost all of them free */
static uint32_t *usedlist;
static uint32_t nused, capused;
static uint32_t live, threshold = 8192;

#define SLOT_MASK 0x7fffffffu
#define MIN_SLOTS 4096u

/* the room in a string; its text can end sooner (LEN_SHORTENED) */
static uint32_t text_len(const char *text) { return ((const uint32_t *)(const void *)text)[-1] & ~LEN_SHORTENED; }

/* Most strings are short and live a few frames, so they come from free
 * lists of fixed-size blocks in one region instead of malloc and free each
 * time (the Xbox's C library allocator is slow). The region is bounded and
 * strings that don't fit it go to malloc. A block's size class follows
 * from the length stored at its start. */
#define POOL_CLASSES 4
#ifdef BW_LOWMEM
#define POOL_BYTES (2u << 20)
#else
#define POOL_BYTES (8u << 20)
#endif
/* A map load fills the region (90k strings at once on lqdm1) and a match
 * then keeps a fifth of that alive, so on a 64 MB Xbox the region is 1 MB:
 * the load's overflow goes to malloc and back when it is collected. */
#define POOL_BYTES_SMALL (1u << 20)
static char *pool, *pool_next;
static size_t pool_bytes;
static void *pool_free[POOL_CLASSES]; /* each free block holds the next */

static int pool_class(size_t bytes)
{
	return bytes <= 16 ? 0 : bytes <= 32 ? 1 : bytes <= 64 ? 2 : bytes <= 128 ? 3 : -1;
}

static uint32_t *block_alloc(size_t bytes)
{
	int k = pool_class(bytes);
	void *b;
	if (k >= 0 && pool_free[k]) {
		b = pool_free[k];
		pool_free[k] = *(void **)b;
		return b;
	}
	if (k >= 0) {
		size_t size = (size_t)16 << k;
		if (!pool) {
			pool_bytes = BW_SMALL_MEMORY() ? POOL_BYTES_SMALL : POOL_BYTES;
			pool = pool_next = Mem_Realloc(NULL, pool_bytes);
		}
		if ((size_t)(pool + pool_bytes - pool_next) >= size) {
			b = pool_next;
			pool_next += size;
			return b;
		}
	}
	return Mem_Realloc(NULL, bytes);
}

static void block_free(uint32_t *block)
{
	int k = pool_class(sizeof(uint32_t) + (block[0] & ~LEN_SHORTENED) + 1);
	if (!pool || (uintptr_t)block - (uintptr_t)pool >= pool_bytes) {
		Mem_Free(block);
		return;
	}
	*(void **)(void *)block = pool_free[k];
	pool_free[k] = block;
}

int QC_LiveStrings(void) { return (int)live; }

void QC_PrintStringStats(void)
{
	Con_Printf("strings: %u live, %u slots of %u, pool %u of %u KB used\n", (unsigned)live, (unsigned)nstrs,
	           (unsigned)capstrs, (unsigned)(pool ? (pool_next - pool) >> 10 : 0), (unsigned)(pool_bytes >> 10));
}

/* QC_GetString, inline for the string compares (QC_ProgsString's work too) */
static inline const char *text_of(qcvm_t *vm, string_t s)
{
	uint32_t u = (uint32_t)s;
	if (!(u & QC_STRING_RUNTIME)) {
		int pn = QC_FUNC_PROGS(s);
		int32_t ofs = QC_FUNC_INDEX(s);
		const qcprogs_t *p;
		if (pn >= vm->numprogs)
			return "";
		p = &vm->progs[pn];
		return ofs <= 0 || ofs >= p->stringsize ? "" : p->strings + ofs;
	}
	u &= SLOT_MASK;
	if (!u || u >= nstrs || !strs[u])
		return "";
	return strs[u];
}

const char *QC_GetString(qcvm_t *vm, string_t s) { return text_of(vm, s); }

/* the interpreter's string ==. Two engine strings of different lengths
 * differ, unless QC stored a NUL into one of them. */
bool QC_StringsEqual(qcvm_t *vm, string_t a, string_t b)
{
	const char *x, *y;
	uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
	if (a == b)
		return true;
	if (ua & ub & QC_STRING_RUNTIME) {
		ua &= SLOT_MASK;
		ub &= SLOT_MASK;
		x = ua && ua < nstrs ? strs[ua] : NULL;
		y = ub && ub < nstrs ? strs[ub] : NULL;
		if (x && y) {
			uint32_t lx = ((const uint32_t *)(const void *)x)[-1], ly = ((const uint32_t *)(const void *)y)[-1];
			if (!((lx | ly) & LEN_SHORTENED))
				return lx == ly && x[0] == y[0] && !memcmp(x, y, lx);
		}
	}
	x = text_of(vm, a);
	y = text_of(vm, b);
	return x[0] == y[0] && !strcmp(x, y);
}

void QC_StringSetChar(qcvm_t *vm, string_t s, int64_t idx, int ch)
{
	size_t len;
	char *p = QC_StringBuffer(vm, s, &len);
	uint32_t u = (uint32_t)s;
	if (!p || idx < 0 || (uint64_t)idx >= len)
		QC_RunError(vm, "string index %lld out of range", (long long)idx);
	p[idx] = (char)ch;
	if (!(char)ch && (u & QC_STRING_RUNTIME))
		((uint32_t *)(void *)p)[-1] |= LEN_SHORTENED;
	qc_string_writes++;
}

char *QC_StringBuffer(qcvm_t *vm, string_t s, size_t *len)
{
	uint32_t u = (uint32_t)s;
	if (!s)
		return NULL;
	if (!(u & QC_STRING_RUNTIME)) {
		int pn = QC_FUNC_PROGS(s), ofs = QC_FUNC_INDEX(s);
		if (pn >= vm->numprogs || ofs >= vm->progs[pn].stringsize)
			return NULL;
		*len = strlen(vm->progs[pn].strings + ofs);
		return vm->progs[pn].strings + ofs;
	}
	u &= SLOT_MASK;
	if (!u || u >= nstrs || !strs[u])
		return NULL;
	*len = text_len(strs[u]);
	return strs[u];
}

static void resize_slots(uint32_t cap)
{
	capstrs = cap;
	strs = Mem_Realloc(strs, sizeof(char *) * capstrs);
	marks = Mem_Realloc(marks, capstrs / 8);
	strs[0] = NULL; /* slot 0 is never handed out */
}

string_t QC_NewStringLen(qcvm_t *vm, const char *s, size_t len)
{
	uint32_t slot;
	uint32_t *block;
	(void)vm;
	if (len > 0x7ffffff0u)
		Sys_Error("QC string too long");
	if (nfree) {
		while (strs[freescan])
			freescan++;
		slot = freescan++;
		nfree--;
	} else {
		if (nstrs == 0)
			nstrs = 1; /* slot 0 would be the bit pattern of -0.0f */
		if (nstrs >= capstrs)
			resize_slots(capstrs ? capstrs * 2 : MIN_SLOTS);
		if (nstrs >= SLOT_MASK)
			Sys_Error("QC string table exhausted");
		slot = nstrs++;
		freescan = nstrs;
	}
	block = block_alloc(sizeof(uint32_t) + len + 1);
	block[0] = (uint32_t)len;
	/* binary text with a NUL in it ends early, as one stored from QC does */
	if (memchr(s, 0, len))
		block[0] |= LEN_SHORTENED;
	strs[slot] = (char *)(block + 1);
	memcpy(strs[slot], s, len);
	strs[slot][len] = 0;
	if (nused >= capused) {
		capused = capused ? capused * 2 : MIN_SLOTS;
		usedlist = Mem_Realloc(usedlist, sizeof(uint32_t) * capused);
	}
	usedlist[nused++] = slot;
	live++;
	return (string_t)(QC_STRING_RUNTIME | slot);
}

string_t QC_NewString(qcvm_t *vm, const char *s)
{
	if (!s)
		return 0;
	return QC_NewStringLen(vm, s, strlen(s));
}

static void mark_value(uint32_t v)
{
	if (v & QC_STRING_RUNTIME) {
		v &= SLOT_MASK;
		if (v && v < nstrs)
			marks[v >> 3] |= (uint8_t)(1u << (v & 7));
	}
}

static void mark_words(const qcval_t *w, size_t count)
{
	for (size_t i = 0; i < count; i++)
		mark_value(w[i].u);
}

static void mark_vm(qcvm_t *vm)
{
	for (int i = 0; i < vm->numprogs; i++)
		mark_words(QC_ProgsGlobals(vm, i), (size_t)vm->progs[i].globalslots);
	for (int e = 0; e < vm->num_edicts; e++) {
		/* recently freed entities can still be read, so keep their strings too */
		mark_words(QC_EdictFields(vm, e), (size_t)vm->edict_words);
	}
	mark_words(vm->localstack, (size_t)vm->localstack_used);
	for (size_t ofs = 0; ofs < vm->heap_size;) {
		size_t size;
		bool used = QC_HeapBlockUsed(vm, ofs, &size);
		if (!size)
			break;
		if (used)
			mark_words((const qcval_t *)(vm->mem + vm->heap_ofs + ofs + 16), (size - 16) / 4);
		ofs += size;
	}
	QC_MarkTokens(vm, mark_value);
	QC_MarkHashTables(vm, mark_value);
}

/* the smallest power-of-two capacity, at least MIN_SLOTS, that holds n */
static uint32_t pow2_cap(uint32_t n)
{
	uint32_t c = MIN_SLOTS;
	while (c < n && c < 0x80000000u)
		c *= 2;
	return c;
}

static int gc_runs;
static double gc_seconds;

void QC_TakeGCStats(int *runs, double *seconds)
{
	*runs = gc_runs, *seconds = gc_seconds;
	gc_runs = 0, gc_seconds = 0;
}

static void collect(void);

void QC_CollectGarbage(qcvm_t *vm, bool force)
{
	(void)vm;
	if (!force && live < threshold)
		return;
	if (QC_AnyExecuting())
		return;
	if (!capstrs)
		return;
	double t0 = Sys_Seconds();
	collect();
	gc_seconds += Sys_Seconds() - t0;
	gc_runs++;
}

static void collect(void)
{
	uint32_t freed = 0, top = 0, kept = 0;
	memset(marks, 0, capstrs / 8);
	for (int i = 0; i < QC_VMCount(); i++)
		mark_vm(QC_VMAt(i));
	for (uint32_t i = 0; i < nused; i++) {
		uint32_t slot = usedlist[i];
		if (marks[slot >> 3] & (1u << (slot & 7))) {
			usedlist[kept++] = slot;
			if (slot > top)
				top = slot;
			continue;
		}
		block_free((uint32_t *)(void *)strs[slot] - 1);
		strs[slot] = NULL;
		if (slot < freescan)
			freescan = slot;
		freed++;
		live--;
	}
	nused = kept;
	/* a collection costs about the same however much it frees, so let
	 * garbage build up: Nuclide makes tens of thousands of temporary
	 * strings a frame */
	threshold = BW_MAX(65536u, live * 2);
	/* drop the free slots above the last live one, and give back table
	 * room a burst of strings (a map load) left behind; keep enough for
	 * the strings made before the next collection */
	nstrs = top + 1;
	nfree = nstrs - 1 - live;
	if (freescan > nstrs)
		freescan = nstrs;
	{
		uint32_t want = pow2_cap(BW_MAX(nstrs, live + threshold));
		uint32_t want_used = pow2_cap(live + threshold);
		if (capstrs > 2 * want)
			resize_slots(want);
		if (capused > 2 * want_used) {
			capused = want_used;
			usedlist = Mem_Realloc(usedlist, sizeof(uint32_t) * capused);
		}
	}
	Con_DPrintf("qc gc: freed %u strings, %u live\n", freed, live);
}
