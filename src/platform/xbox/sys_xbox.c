/* Bulwark engine - Xbox start-up: the arguments file, the log, memory
 * figures and the fatal-error screen. ISC licence, see LICENSE.
 *
 * The XBE's own folder is mounted as D: (nxdk's automount), and that is the
 * base directory: D:\base\ holds the game, D:\args.txt the command line
 * (the same words as on a desktop, e.g. "-game base +map lqdm1"), and
 * D:\bulwark.log gets everything the engine prints. Run from a DVD, D: is
 * read-only and the log goes only to the kernel debugger (DbgPrint, which
 * xemu's serial port shows with a debug kernel). Quitting goes back to
 * the dashboard (Plat_Shutdown). */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <dirent.h>
#include <sys/stat.h>

#include "../../common/common.h"
#include "../platform.h"
#include "xbox.h"

#define BASEDIR "D:"

void bwx_net_start(void);
void bwx_net_disable(void);

static FILE *logfile;
static bool dbgprint = true;
static char *argbuf;
static char *args[64];

void XBX_Log(const char *text)
{
	if (logfile) {
		fputs(text, logfile);
		fflush(logfile);
	}
	if (dbgprint) {
		/* DbgPrint takes a format and has a small buffer: send it in pieces */
		char piece[256];
		size_t n = strlen(text);
		for (size_t o = 0; o < n; o += sizeof(piece) - 1) {
			size_t len = BW_MIN(n - o, sizeof(piece) - 1);
			memcpy(piece, text + o, len);
			piece[len] = 0;
			DbgPrint("%s", piece);
		}
	}
}

static size_t lowest_free = (size_t)-1, logged_free = (size_t)-1;
/* -xmemlog: log each new low of free memory, by 512 KB steps. Off, big
 * allocations skip the kernel query (XBX_NoteFreeMemory) */
bool xbx_memlog;

size_t XBX_FreeMemory(void)
{
	MM_STATISTICS ms;
	ms.Length = sizeof(ms);
	if (!NT_SUCCESS(MmQueryStatistics(&ms)))
		return 0;
	size_t f = (size_t)ms.AvailablePages * 4096;
	if (f < lowest_free)
		lowest_free = f;
	return f;
}

/* after a big allocation (only under -xmemlog): the least memory there has
 * been free shows how close loading came to running out */
void XBX_NoteFreeMemory(const void *from)
{
	size_t f = XBX_FreeMemory();
	if (xbx_memlog && f + (512u << 10) <= logged_free) {
		char b[96];
		logged_free = f;
		snprintf(b, sizeof(b), "xmemlog: %u KB free, from %p\n", (unsigned)(f >> 10), from);
		XBX_Log(b);
	}
}

/* set by -xmem: the size of console being played */
static size_t sim_total;

/* 64 MB on a retail console, 128 on an upgraded or development one */
size_t XBX_TotalMemory(void)
{
	if (sim_total)
		return sim_total;
	MM_STATISTICS ms;
	ms.Length = sizeof(ms);
	if (!NT_SUCCESS(MmQueryStatistics(&ms)))
		return 0;
	return (size_t)ms.TotalPhysicalPages * 4096;
}

/* what malloc has taken from the kernel: PDCLib grows its heap with
 * NtAllocateVirtualMemory, so this is the virtual memory the title holds
 * outside its image and the GPU's contiguous blocks */
static size_t heap_bytes(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	size_t total = 0;
	for (uintptr_t a = 0x10000; a < 0x80000000u; a += mbi.RegionSize) {
		if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
			break;
		if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE)
			total += mbi.RegionSize;
	}
	return total;
}

/* xbox_meminfo regions: the committed private regions of 64 KB or more,
 * to tell malloc's from other users' */
void XBX_PrintRegions(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	for (uintptr_t a = 0x10000; a < 0x80000000u; a += mbi.RegionSize) {
		if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
			break;
		if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= 65536)
			Con_Printf("  region %p: %u KB (allocation base %p)\n", mbi.BaseAddress, (unsigned)(mbi.RegionSize >> 10),
			           mbi.AllocationBase);
	}
}

/* PDCLib's malloc is dlmalloc 2.8.6 over NtAllocateVirtualMemory: 64 KB
 * segments for the heap, requests of 256 KB and more mapped on their own
 * and returned on free, whole free segments released. Its statistics are
 * exported unprefixed-but-for-"dl"; the layout is dlmalloc's own. */
struct bwx_mallinfo {
	size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
extern struct bwx_mallinfo dlmallinfo(void);

void XBX_PrintMemory(const char *when)
{
	MM_STATISTICS ms;
	ms.Length = sizeof(ms);
	if (!NT_SUCCESS(MmQueryStatistics(&ms)))
		return;
	Con_Printf("memory %s: %u KB free (lowest %u KB) of %u KB (pool %u KB, images %u KB, stacks %u KB, cache %u KB)\n",
	           when, (unsigned)(ms.AvailablePages * 4), (unsigned)(BW_MIN(lowest_free, (size_t)ms.AvailablePages * 4096) >> 10),
	           (unsigned)(ms.TotalPhysicalPages * 4), (unsigned)(ms.PoolPagesCommitted * 4),
	           (unsigned)(ms.ImagePagesCommitted * 4), (unsigned)(ms.StackPagesCommitted * 4),
	           (unsigned)(ms.CachePagesCommitted * 4));
	struct bwx_mallinfo mi = dlmallinfo();
	Con_Printf("  C heap %u KB: malloc's heap %u KB (%u KB of it free, %u chunks), %u KB mapped directly\n",
	           (unsigned)(heap_bytes() >> 10), (unsigned)(mi.arena >> 10), (unsigned)(mi.fordblks >> 10),
	           (unsigned)mi.ordblks, (unsigned)(mi.hblkhd >> 10));
	XBGL_PrintGpuMemory();
}

/* a fatal error: say so on the screen, then wait for the console's reset */
void XBX_FatalScreen(const char *text)
{
	pb_show_debug_screen();
	debugClearScreen();
	debugPrint("Bulwark stopped:\n\n%s\n\nThe log is in bulwark.log beside the XBE.\n", text);
	for (;;)
		Sleep(1000);
}

/* -fstest <dir>: what opendir, readdir and stat say about a folder, for
 * checking the file system shims against a kernel */
static void fs_test(const char *dir)
{
	DIR *d = opendir(dir);
	Con_Printf("fstest %s: opendir %s (error %lu)\n", dir, d ? "ok" : "failed", (unsigned long)GetLastError());
	if (!d)
		return;
	struct dirent *de;
	int n = 0;
	while ((de = readdir(d)) && n < 20) {
		char full[512];
		struct stat st;
		snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		int r = stat(full, &st);
		Con_Printf("  %s type %d stat %d mode %o size %ld\n", de->d_name, de->d_type, r, r ? 0 : (unsigned)st.st_mode,
		           r ? 0L : (long)st.st_size);
		n++;
	}
	Con_Printf("fstest: %d entries\n", n);
	closedir(d);
}

/* -cpubench: a dependent chain of simple integer operations, about one per
 * clock on a Pentium III, timed with the performance counter: how fast the
 * CPU (or an emulator's) runs compared with the real 733 MHz part */
static void cpu_bench(void)
{
	volatile uint32_t sink;
	uint32_t x = 1;
	ULONGLONG t0 = KeQueryPerformanceCounter();
	for (uint32_t i = 0; i < 100000000u; i++)
		x = x * 1664525u + 1013904223u;
	ULONGLONG t1 = KeQueryPerformanceCounter();
	sink = x;
	(void)sink;
	double secs = (double)(t1 - t0) / (double)KeQueryPerformanceFrequency();
	Con_Printf("cpubench: 100M multiply-adds in %.3f s (a 733 MHz Pentium III: about %.2f s, 5 clocks each)\n", secs, 100e6 * 5 / 733e6);
	/* single-precision SSE: a multiply then an add, 4 + 3 clocks of latency */
	volatile float fsink;
	float f = 1.0f;
	t0 = KeQueryPerformanceCounter();
	for (uint32_t i = 0; i < 20000000u; i++)
		f = f * 0.999999f + 0.000001f;
	t1 = KeQueryPerformanceCounter();
	fsink = f;
	(void)fsink;
	secs = (double)(t1 - t0) / (double)KeQueryPerformanceFrequency();
	Con_Printf("cpubench: 20M float multiply-adds in %.3f s (Pentium III: about %.2f s, 7 clocks each)\n", secs, 20e6 * 7 / 733e6);
	/* memory: sum 16 MB, 4 bytes at a time (the P3's 133 MHz bus: roughly 0.5-0.8 GB/s) */
	uint32_t *buf = malloc(16u << 20);
	if (buf) {
		memset(buf, 1, 16u << 20);
		uint32_t sum = 0;
		t0 = KeQueryPerformanceCounter();
		for (int pass = 0; pass < 4; pass++)
			for (uint32_t i = 0; i < (4u << 20); i++)
				sum += buf[i];
		t1 = KeQueryPerformanceCounter();
		sink = sum;
		secs = (double)(t1 - t0) / (double)KeQueryPerformanceFrequency();
		Con_Printf("cpubench: read 64 MB in %.3f s (%.0f MB/s)\n", secs, 64 / secs);
		free(buf);
	}
}

/* the kernel commits and releases memory a page at a time, and gives
 * committed pages zeroed */
void *Mem_PageAlloc(size_t size, size_t commit)
{
	void *p = VirtualAlloc(NULL, size ? size : 1, MEM_RESERVE, PAGE_READWRITE);
	commit = commit < size ? commit : size;
	if (!p || (commit && !VirtualAlloc(p, commit, MEM_COMMIT, PAGE_READWRITE)))
		Sys_Error("out of memory allocating %u pages", (unsigned)(commit / 4096));
	return p;
}

void Mem_PageFree(void *p)
{
	if (p)
		VirtualFree(p, 0, MEM_RELEASE);
}

bool Mem_PageCommit(void *p, size_t size) { return !size || VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE); }

void Mem_PageDecommit(void *p, size_t size)
{
	if (size)
		VirtualFree(p, size, MEM_DECOMMIT);
}

/* -xmem <MB>: play a smaller console on a bigger one. The kernel xemu
 * boots (Roswell) only starts on 128 MB, so a 64 MB retail console is
 * tested by committing the difference as memory nobody uses and reporting
 * the smaller size to the engine. The kernel gives committed pages from
 * its upper 64 MB first, which titles can't get contiguous memory from,
 * so what is left looks like a retail console's RAM. -xmemsizes <MB> only
 * takes the smaller console's settings, to measure what they use. */
static void limit_memory(unsigned mb, bool hold)
{
	size_t total = XBX_TotalMemory(), target = (size_t)mb << 20, held = 0;
	if (!mb || total <= target)
		return;
	size_t want = hold ? total - target : 0;
	for (size_t chunk = 4u << 20; chunk >= (64u << 10) && held < want;) {
		size_t n = BW_MIN(chunk, want - held);
		volatile uint8_t *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!p) {
			chunk >>= 1;
			continue;
		}
		for (size_t o = 0; o < n; o += 4096)
			p[o] = 1;
		held += n;
	}
	sim_total = target;
	Con_Printf("xmem: holding %u KB, playing a %u MB console\n", (unsigned)(held >> 10), mb);
}

/* words separated by spaces, "quoted" words keep theirs */
static int split_args(char *s, char **out, int max)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
			s++;
		if (!*s)
			break;
		if (*s == '"') {
			out[n++] = ++s;
			while (*s && *s != '"')
				s++;
		} else {
			out[n++] = s;
			while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
				s++;
		}
		if (*s)
			*s++ = 0;
	}
	return n;
}

void Plat_Init(int *argc, char ***argv)
{
	/* the GPU code sets the mode it renders at; this one lets debugPrint
	 * show something if start-up fails before that */
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

	logfile = fopen(BASEDIR "\\bulwark.log", "w");
	Con_Printf("Bulwark %s on the Xbox, %u KB free at start\n", BULWARK_VERSION, (unsigned)(XBX_FreeMemory() >> 10));

	args[0] = *argc > 0 && *argv ? (*argv)[0] : "bulwark";
	int n = 1;
	FILE *f = fopen(BASEDIR "\\args.txt", "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		argbuf = calloc(1, (size_t)len + 1);
		if (argbuf && fread(argbuf, 1, (size_t)len, f) == (size_t)len)
			n += split_args(argbuf, args + 1, (int)BW_ARRAY_COUNT(args) - 2);
		fclose(f);
	}
	/* the Xbox's own switches; the engine's parser doesn't know them */
	bool net = true;
	int kept = 1;
	for (int i = 1; i < n; i++) {
		if (!strcmp(args[i], "-nonet"))
			net = false;
		else if (!strcmp(args[i], "-nodbgprint"))
			dbgprint = false;
		else if (!strcmp(args[i], "-cpubench"))
			cpu_bench();
		else if (!strcmp(args[i], "-fstest") && i + 1 < n)
			fs_test(args[++i]);
		else if (!strcmp(args[i], "-xmem") && i + 1 < n)
			limit_memory((unsigned)atoi(args[++i]), true);
		else if (!strcmp(args[i], "-xmemlog"))
			xbx_memlog = true;
		else if (!strcmp(args[i], "-xmemsizes") && i + 1 < n)
			limit_memory((unsigned)atoi(args[++i]), false);
		else
			args[kept++] = args[i];
	}
	n = kept;
	args[n] = NULL;
	if (sim_total)
		Con_Printf("%u KB free after -xmem\n", (unsigned)(XBX_FreeMemory() >> 10));
	*argc = n;
	*argv = args;
	/* LAN games need the network; it starts in the background (DHCP) */
	if (net)
		bwx_net_start();
	else
		bwx_net_disable();
}

const char *Plat_DefaultBaseDir(void) { return BASEDIR; }

/* quit: back to the dashboard. Returning from main would get there too
 * (nxdk's exit() ends in a quick reboot with no launch data, which boots
 * the dashboard), but by way of the C library's exit handlers while the
 * sound and network threads still run. XLaunchXBE(NULL) writes the launch
 * data page as LDT_LAUNCH_DASHBOARD, the way a title asks for the
 * dashboard, and reboots straight away. It returns only if it can't get
 * memory for that page, and then main's return still gets there. */
void Plat_Shutdown(void)
{
	Con_Printf("quit: back to the dashboard\n");
	if (logfile)
		fclose(logfile);
	logfile = NULL;
	XLaunchXBE(NULL);
}
