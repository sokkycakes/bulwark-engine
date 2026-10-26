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

typedef struct {
	char *data;
	uint32_t len;
	uint8_t used;
	uint8_t mark;
} qcstr_t;

static qcstr_t *strs;
static uint32_t nstrs, capstrs;
static uint32_t *freelist;
static uint32_t nfree, capfree;
/* every slot in use, so a collection only visits live and new strings:
 * a map load can leave millions of slots, almost all of them free */
static uint32_t *usedlist;
static uint32_t nused, capused;
static uint32_t live, threshold = 8192;

#define SLOT_MASK 0x7fffffffu

int QC_LiveStrings(void) { return (int)live; }

const char *QC_GetString(qcvm_t *vm, string_t s)
{
	uint32_t u = (uint32_t)s;
	if (!s)
		return "";
	if (!(u & QC_STRING_RUNTIME))
		return QC_ProgsString(vm, s);
	u &= SLOT_MASK;
	if (u >= nstrs || !strs[u].used)
		return "";
	return strs[u].data;
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
	if (u >= nstrs || !strs[u].used)
		return NULL;
	*len = strs[u].len;
	return strs[u].data;
}

string_t QC_NewStringLen(qcvm_t *vm, const char *s, size_t len)
{
	uint32_t slot;
	(void)vm;
	if (nfree) {
		slot = freelist[--nfree];
	} else {
		if (nstrs == 0)
			nstrs = 1; /* slot 0 would be the bit pattern of -0.0f */
		if (nstrs >= capstrs) {
			capstrs = capstrs ? capstrs * 2 : 4096;
			strs = Mem_Realloc(strs, sizeof(qcstr_t) * capstrs);
		}
		if (nstrs >= SLOT_MASK)
			Sys_Error("QC string table exhausted");
		slot = nstrs++;
	}
	strs[slot].data = Mem_Alloc(len + 1);
	memcpy(strs[slot].data, s, len);
	strs[slot].data[len] = 0;
	strs[slot].len = (uint32_t)len;
	strs[slot].used = 1;
	strs[slot].mark = 0;
	if (nused >= capused) {
		capused = capused ? capused * 2 : 4096;
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
			strs[v].mark = 1;
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

void QC_CollectGarbage(qcvm_t *vm, bool force)
{
	uint32_t freed = 0;
	(void)vm;
	if (!force && live < threshold)
		return;
	if (QC_AnyExecuting())
		return;
	for (uint32_t i = 0; i < nused; i++)
		strs[usedlist[i]].mark = 0;
	for (int i = 0; i < QC_VMCount(); i++)
		mark_vm(QC_VMAt(i));
	uint32_t kept = 0;
	for (uint32_t i = 0; i < nused; i++) {
		uint32_t slot = usedlist[i];
		if (strs[slot].mark) {
			usedlist[kept++] = slot;
			continue;
		}
		Mem_Free(strs[slot].data);
		strs[slot].data = NULL;
		strs[slot].used = 0;
		if (nfree >= capfree) {
			capfree = capfree ? capfree * 2 : 4096;
			freelist = Mem_Realloc(freelist, sizeof(uint32_t) * capfree);
		}
		freelist[nfree++] = slot;
		freed++;
		live--;
	}
	nused = kept;
	/* a collection costs about the same however much it frees, so let
	 * garbage build up: Nuclide makes tens of thousands of temporary
	 * strings a frame */
	threshold = BW_MAX(65536u, live * 2);
	Con_DPrintf("qc gc: freed %u strings, %u live\n", freed, live);
}
