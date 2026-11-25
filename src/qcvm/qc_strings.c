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
static char **strs;
static uint32_t nstrs, capstrs;
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

static uint32_t text_len(const char *text) { return ((const uint32_t *)(const void *)text)[-1]; }

int QC_LiveStrings(void) { return (int)live; }

const char *QC_GetString(qcvm_t *vm, string_t s)
{
	uint32_t u = (uint32_t)s;
	if (!s)
		return "";
	if (!(u & QC_STRING_RUNTIME))
		return QC_ProgsString(vm, s);
	u &= SLOT_MASK;
	if (u >= nstrs || !strs[u])
		return "";
	return strs[u];
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
	if (u >= nstrs || !strs[u])
		return NULL;
	*len = text_len(strs[u]);
	return strs[u];
}

static void resize_slots(uint32_t cap)
{
	capstrs = cap;
	strs = Mem_Realloc(strs, sizeof(char *) * capstrs);
	marks = Mem_Realloc(marks, capstrs / 8);
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
	block = Mem_Alloc(sizeof(uint32_t) + len + 1);
	block[0] = (uint32_t)len;
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
	for (int i = 0; i < vm->extra->tokens.count; i++)
		mark_value((uint32_t)vm->extra->tokens.argv[i]);
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

void QC_CollectGarbage(qcvm_t *vm, bool force)
{
	uint32_t freed = 0, top = 0, kept = 0;
	(void)vm;
	if (!force && live < threshold)
		return;
	if (QC_AnyExecuting())
		return;
	if (!capstrs)
		return;
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
		Mem_Free((uint32_t *)(void *)strs[slot] - 1);
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
