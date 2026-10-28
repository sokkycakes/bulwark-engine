/* Bulwark engine - 3DS start-up: system services, memory split, the
 * arguments file and the log. ISC licence, see LICENSE.
 *
 * There is no command line on the 3DS, so arguments come from
 * sdmc:/bulwark/args.txt (the same words as on a desktop, e.g.
 * "-game base +map ai"). Everything printed shows on the bottom screen and
 * goes to sdmc:/bulwark/bulwark.log. */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include "../../common/common.h"
#include "../platform.h"
#include "ctr.h"

#define BASEDIR "sdmc:/bulwark"

/* libctru splits the application's memory between the normal heap and the
 * linear heap the GPU reads. By default the normal heap stops at 24 MB and
 * the rest goes linear; the engine (QuakeC VMs, maps, sounds) needs more
 * than that, and textures and vertex buffers fit in 20 MB. */
u32 __ctru_linear_heap_size = 20 << 20;
/* the main thread's stack: 32 KB by default, and the engine keeps several
 * 8 KB buffers on it (print formatting, file names, QC calls) */
u32 __stacksize__ = 1 << 20;

static FILE *logfile;
static const devoptab_t *console_dev;
static devoptab_t tee_dev;

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (logfile) {
		fwrite(ptr, 1, len, logfile);
		fflush(logfile);
	}
	return console_dev->write_r(r, fd, ptr, len);
}

static char *argbuf;
static char *args[64];

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

void CTR_PrintMemory(const char *when)
{
	extern u32 __ctru_heap_size;
	struct mallinfo mi = mallinfo();
	Con_Printf("memory %s: heap %u of %u KB used, linear %u of %u KB free\n", when,
	           (unsigned)(mi.uordblks >> 10), (unsigned)(__ctru_heap_size >> 10), (unsigned)(linearSpaceFree() >> 10),
	           (unsigned)(__ctru_linear_heap_size >> 10));
}

void Plat_Init(int *argc, char ***argv)
{
	bool isnew = false;
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	/* the New 3DS's 804 MHz clock and L2 cache */
	APT_CheckNew3DS(&isnew);
	if (isnew)
		osSetSpeedupEnable(true);
	mkdir(BASEDIR, 0777);
	logfile = fopen(BASEDIR "/bulwark.log", "w");
	console_dev = devoptab_list[STD_OUT];
	tee_dev = *console_dev;
	tee_dev.write_r = tee_write;
	devoptab_list[STD_OUT] = &tee_dev;
	devoptab_list[STD_ERR] = &tee_dev;
	printf("Bulwark %s on %s 3DS\n", BULWARK_VERSION, isnew ? "a New" : "an Old");

	/* the program's own name, then the file's words */
	args[0] = *argc > 0 ? (*argv)[0] : "bulwark";
	int n = 1;
	FILE *f = fopen(BASEDIR "/args.txt", "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		argbuf = calloc(1, (size_t)len + 1);
		if (argbuf && fread(argbuf, 1, (size_t)len, f) == (size_t)len)
			n += split_args(argbuf, args + 1, (int)BW_ARRAY_COUNT(args) - 2);
		fclose(f);
	}
	args[n] = NULL;
	*argc = n;
	*argv = args;
	/* sockets for LAN games; loopback works without them */
	static u32 *socbuf;
	socbuf = memalign(0x1000, 0x100000);
	if (socbuf && R_FAILED(socInit(socbuf, 0x100000))) {
		free(socbuf);
		socbuf = NULL;
	}
}

const char *Plat_DefaultBaseDir(void) { return BASEDIR; }

void Plat_Shutdown(void)
{
	socExit();
	if (logfile)
		fclose(logfile);
	logfile = NULL;
	gfxExit();
}
