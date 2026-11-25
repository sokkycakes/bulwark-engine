/* Bulwark engine - records every malloc, calloc, realloc, memalign and free
 * of a desktop run, for heapsim.c to replay as the Xbox's C library would.
 * ISC licence, see LICENSE.
 *
 *   cc -O2 -shared -fPIC -o heaptrace.so heaptrace.c -lpthread
 *   MTRACE_OUT=run.trace LD_PRELOAD=./heaptrace.so bulwark ... +map lqdm1
 *
 * Records are packed (op, pointer, size, old pointer or alignment, caller);
 * run.trace.maps gets /proc/self/maps at exit, so the replay can keep only
 * the calls the engine makes (not the desktop GL driver's). glibc only. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

#pragma pack(push, 1)
typedef struct {
	uint8_t op; /* 1 malloc, 2 calloc, 3 realloc, 4 free, 5 memalign */
	uint64_t p, size, old, caller;
} rec_t;
#pragma pack(pop)

static int fd = -1, inited;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static rec_t buf[65536];
static int nbuf;
static __thread int busy;

static void flush(void)
{
	if (fd >= 0 && nbuf) {
		ssize_t r = write(fd, buf, sizeof(rec_t) * nbuf);
		(void)r;
	}
	nbuf = 0;
}

static void put_(int op, void *p, size_t s, void *old, void *caller)
{
	if (busy)
		return;
	busy = 1;
	pthread_mutex_lock(&mu);
	if (!inited) {
		inited = 1;
		const char *f = getenv("MTRACE_OUT");
		if (f)
			fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	}
	if (fd >= 0) {
		buf[nbuf++] = (rec_t){(uint8_t)op, (uintptr_t)p, s, (uintptr_t)old, (uintptr_t)caller};
		if (nbuf == 65536)
			flush();
	}
	pthread_mutex_unlock(&mu);
	busy = 0;
}
#define put(op, p, s, o) put_(op, p, s, o, __builtin_return_address(0))

__attribute__((destructor)) static void fin(void)
{
	pthread_mutex_lock(&mu);
	flush();
	pthread_mutex_unlock(&mu);
	const char *f = getenv("MTRACE_OUT");
	if (!f || strlen(f) > 500)
		return;
	char path[512], b[4096];
	strcpy(path, f);
	strcat(path, ".maps");
	int in = open("/proc/self/maps", O_RDONLY), out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ssize_t n;
	while (in >= 0 && out >= 0 && (n = read(in, b, sizeof(b))) > 0) {
		ssize_t w = write(out, b, (size_t)n);
		(void)w;
	}
	close(in);
	close(out);
}

void *malloc(size_t s)
{
	void *p = __libc_malloc(s);
	if (p)
		put(1, p, s, 0);
	return p;
}
void *calloc(size_t n, size_t s)
{
	void *p = __libc_calloc(n, s);
	if (p)
		put(2, p, n * s, 0);
	return p;
}
void *realloc(void *o, size_t s)
{
	void *p = __libc_realloc(o, s);
	if (!o) {
		if (p)
			put(1, p, s, 0);
	} else if (!s) {
		put(4, o, 0, 0);
	} else if (p) {
		put(3, p, s, o);
	}
	return p;
}
void free(void *p)
{
	if (p)
		put(4, p, 0, 0);
	__libc_free(p);
}
void *memalign(size_t a, size_t s)
{
	void *p = __libc_memalign(a, s);
	if (p)
		put(5, p, s, (void *)a);
	return p;
}
void *aligned_alloc(size_t a, size_t s) { return memalign(a, s); }
int posix_memalign(void **r, size_t a, size_t s)
{
	void *p = memalign(a, s);
	if (!p)
		return ENOMEM;
	*r = p;
	return 0;
}
