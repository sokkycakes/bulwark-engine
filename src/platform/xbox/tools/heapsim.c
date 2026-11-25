/* Bulwark engine - replays a heaptrace.c recording through the Xbox C
 * library's allocator, to see what the heap costs on the console beyond
 * what the engine asks for. ISC licence, see LICENSE.
 *
 *   cc -m32 -O2 -w -DNXDK_MALLOC='"$NXDK_DIR/lib/pdclib/functions/_dlmalloc/malloc.c"' \
 *      -o heapsim heapsim.c
 *   ./heapsim run.trace run.trace.maps [steady-op [mmap-threshold [trim-threshold]]]
 *
 * nxdk's PDCLib malloc is Doug Lea's dlmalloc 2.8.6 (public domain, CC0),
 * built without sbrk: the heap grows by 64 KB segments of
 * NtAllocateVirtualMemory, requests of 256 KB and up are mapped on their
 * own (page-rounded, as NXDK isn't WIN32 to dlmalloc) and freed straight
 * back, whole free segments are released, and as the kernel frees only
 * whole allocations, a trim of the top that splits one fails and switches
 * trimming off. This file includes that same malloc.c with the same
 * settings, built 32-bit as on the console, over a model of the kernel:
 * first fit from the bottom at 64 KB granularity, 4 KB commit pages,
 * MEM_RELEASE of whole allocations only (dlmalloc's win32munmap).
 *
 * It prints the peak and the state at steady-op (a record number in the
 * run's steady stretch, after loading and before shutdown frees): the
 * pages committed, the bytes live, and where the free space sits. The
 * traced sizes are the desktop's (64-bit pointers), so absolute figures
 * run a little high; the overhead is what it measures. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_SIZE (1536u << 20)
#define GRAN (64u << 10)
#define PAGE 4096u
static uint8_t *arena;
static uint8_t *base_of[ARENA_SIZE / PAGE];   /* the allocation each reserved page belongs to */
static uint32_t pages_at[ARENA_SIZE / PAGE]; /* committed pages of the allocation based here */
static uint32_t committed, peak;

/* NtAllocateVirtualMemory(MEM_RESERVE | MEM_COMMIT) */
static void *vm_alloc(size_t size)
{
	size_t pages = (size + PAGE - 1) / PAGE, span = (pages * PAGE + GRAN - 1) / GRAN * GRAN / PAGE;
	for (size_t at = GRAN; at + span * PAGE <= ARENA_SIZE; at += GRAN) {
		size_t p0 = at / PAGE, n;
		for (n = 0; n < span && !base_of[p0 + n]; n++)
			;
		if (n < span)
			continue;
		for (n = 0; n < span; n++)
			base_of[p0 + n] = arena + at;
		mprotect(arena + at, pages * PAGE, PROT_READ | PROT_WRITE);
		pages_at[p0] = (uint32_t)pages;
		committed += (uint32_t)pages;
		if (committed > peak)
			peak = committed;
		return arena + at;
	}
	return (void *)-1;
}

/* dlmalloc's win32munmap: VirtualQuery, then MEM_RELEASE allocation by allocation */
static int vm_free(void *ptr, size_t size)
{
	uint8_t *c = ptr;
	while (size) {
		if (c < arena || c >= arena + ARENA_SIZE)
			return -1;
		size_t p = (size_t)(c - arena) / PAGE, rs = (size_t)pages_at[p] * PAGE;
		if (base_of[p] != c || rs > size)
			return -1;
		for (size_t i = 0; i < (rs + GRAN - 1) / GRAN * GRAN / PAGE; i++)
			base_of[p + i] = NULL;
		madvise(c, rs, MADV_DONTNEED);
		mprotect(c, rs, PROT_NONE);
		committed -= (uint32_t)(rs / PAGE);
		c += rs, size -= rs;
	}
	return 0;
}

/* PDCLib's settings for NXDK (malloc.c's own NXDK block can't be used on a host) */
#define REGTEST 1
#define USE_DL_PREFIX 1
#define USE_LOCKS 0
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MALLOC_INSPECT_ALL 1
#define DEFAULT_GRANULARITY (64u << 10)
#define MMAP(s) vm_alloc(s)
#define DIRECT_MMAP(s) vm_alloc(s)
#define MUNMAP(a, s) vm_free((a), (s))
#ifndef NXDK_MALLOC
#define NXDK_MALLOC "malloc.c"
#endif
#include NXDK_MALLOC

#pragma pack(push, 1)
typedef struct {
	uint8_t op;
	uint64_t p, size, old, caller;
} rec_t;
#pragma pack(pop)

/* trace pointer -> replay pointer and size */
#define HBITS 22
#define HMASK ((1u << HBITS) - 1)
static uint64_t hk[1 << HBITS];
static void *hv[1 << HBITS];
static size_t hs[1 << HBITS];
static size_t hslot(uint64_t k) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> (64 - HBITS)); }
static size_t hfind(uint64_t k)
{
	size_t i = hslot(k);
	while (hk[i] && hk[i] != k)
		i = (i + 1) & HMASK;
	return i;
}
static void hdel(size_t i)
{
	size_t j = i;
	hk[i] = 0;
	for (;;) {
		j = (j + 1) & HMASK;
		if (!hk[j])
			return;
		size_t h = hslot(hk[j]);
		if (i <= j ? (i < h && h <= j) : (i < h || h <= j))
			continue;
		hk[i] = hk[j], hv[i] = hv[j], hs[i] = hs[j], hk[j] = 0;
		i = j;
	}
}

static uint64_t keep_lo[64], keep_hi[64];
static int nkeep;
static int kept(uint64_t c)
{
	for (int i = 0; i < nkeep; i++)
		if (c >= keep_lo[i] && c < keep_hi[i])
			return 1;
	return 0;
}

static size_t free_by_size[6], whole_free_pages;
static void inspect(void *start, void *end, size_t used, void *arg)
{
	(void)arg;
	if (used)
		return;
	size_t sz = (size_t)((char *)end - (char *)start);
	free_by_size[sz < 1024 ? 0 : sz < 4096 ? 1 : sz < 16384 ? 2 : sz < 65536 ? 3 : sz < 262144 ? 4 : 5] += sz;
	uintptr_t a = ((uintptr_t)start + 64 + PAGE - 1) & ~(uintptr_t)(PAGE - 1), e = ((uintptr_t)end - 16) & ~(uintptr_t)(PAGE - 1);
	if (e > a)
		whole_free_pages += (e - a) / PAGE;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "heapsim trace maps [steady-op [mmap-threshold [trim-threshold]]]\n");
		return 1;
	}
	arena = mmap(NULL, ARENA_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (arena == MAP_FAILED)
		return 1;
	long steady = argc > 3 ? atol(argv[3]) : 0;
	if (argc > 4)
		dlmallopt(M_MMAP_THRESHOLD, atoi(argv[4]));
	if (argc > 5)
		dlmallopt(M_TRIM_THRESHOLD, atoi(argv[5]));
	/* the engine, SDL, zlib and the C library; not the desktop's GL and X */
	FILE *m = fopen(argv[2], "r");
	char line[1024];
	while (m && fgets(line, sizeof(line), m) && nkeep < 64) {
		unsigned long long lo, hi;
		char perm[8];
		if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) != 3 || perm[2] != 'x')
			continue;
		if (strstr(line, "/bulwark") || strstr(line, "libSDL2") || strstr(line, "/libc.so") || strstr(line, "libz.so"))
			keep_lo[nkeep] = lo, keep_hi[nkeep++] = hi;
	}
	if (m)
		fclose(m);
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	rec_t r;
	long n = 0;
	size_t live = 0, peaklive = 0, live_at_peak = 0;
	while (fread(&r, sizeof(r), 1, f) == 1) {
		size_t i;
		n++;
		switch (r.op) {
		case 1:
		case 2:
		case 5:
			if (kept(r.caller)) {
				void *p = r.op == 5 ? dlmemalign(r.old, r.size) : r.op == 2 ? dlcalloc(1, r.size) : dlmalloc(r.size);
				if (!p) {
					fprintf(stderr, "out of memory at record %ld\n", n);
					return 1;
				}
				i = hfind(r.p);
				hk[i] = r.p, hv[i] = p, hs[i] = r.size;
				live += r.size;
			}
			break;
		case 3:
			i = hfind(r.old);
			if (hk[i]) {
				void *p = dlrealloc(hv[i], r.size);
				live += r.size - hs[i];
				hdel(i);
				i = hfind(r.p);
				hk[i] = r.p, hv[i] = p, hs[i] = r.size;
			}
			break;
		case 4:
			i = hfind(r.p);
			if (hk[i]) {
				dlfree(hv[i]);
				live -= hs[i];
				hdel(i);
			}
			break;
		}
		if (live > peaklive)
			peaklive = live;
		if (committed == peak)
			live_at_peak = live;
		if (n == steady) {
			struct mallinfo mi = dlmallinfo();
			dlmalloc_inspect_all(inspect, NULL);
			printf("at record %ld: %u KB committed for %zu KB live (%ld KB over); malloc's heap %zu KB, of it free "
			       "%zu KB in %zu chunks, %zu KB in whole pages; %zu KB mapped directly\n",
			       n, committed * 4, live >> 10, (long)committed * 4 - (long)(live >> 10), (size_t)mi.arena >> 10,
			       (size_t)mi.fordblks >> 10, (size_t)mi.ordblks, whole_free_pages * 4, (size_t)mi.hblkhd >> 10);
			printf("  free chunks by size, KB: <1K %zu, <4K %zu, <16K %zu, <64K %zu, <256K %zu, larger %zu\n",
			       free_by_size[0] >> 10, free_by_size[1] >> 10, free_by_size[2] >> 10, free_by_size[3] >> 10,
			       free_by_size[4] >> 10, free_by_size[5] >> 10);
		}
	}
	fclose(f);
	printf("%ld records; peak %u KB committed (%zu KB live then), most live %zu KB\n", n, peak * 4, live_at_peak >> 10,
	       peaklive >> 10);
	return 0;
}
