/* Bulwark engine - the POSIX functions the engine uses, on the Xbox
 * kernel through nxdk's Win32 subset. ISC licence, see LICENSE.
 *
 * See compat/bw_xbox.h for why this exists. Everything here is small and
 * written for what the engine calls, not as a general POSIX layer. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/net.h>

#include <ctype.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* --- file names ------------------------------------------------------------ */

char *bwx_ospath(const char *path, char *out, size_t size)
{
	size_t o = 0;
	const char *p = path;
	if (!size)
		return out;
	/* relative names hang off D:, the XBE's folder */
	if (!(p[0] && p[1] == ':')) {
		if (size > 3) {
			out[o++] = 'D', out[o++] = ':', out[o++] = '\\';
		}
		while (*p == '/' || *p == '\\' || (p[0] == '.' && (p[1] == '/' || p[1] == '\\')))
			p += *p == '.' ? 2 : 1;
	}
	for (; *p && o + 1 < size; p++) {
		char c = *p == '/' ? '\\' : *p;
		/* "a//b" and "a/./b" */
		if (c == '\\' && o && out[o - 1] == '\\')
			continue;
		if (c == '.' && o && out[o - 1] == '\\' && (p[1] == '/' || p[1] == '\\')) {
			p++;
			continue;
		}
		out[o++] = c;
	}
	out[o] = 0;
	return out;
}

#undef fopen
#undef remove
#undef rename

void FS_NoteWrite(void); /* fs.c: the engine's file lookups are stale */
static bool known_missing(const char *os);
static void forget_listings(void);

/* the engine is about to change a folder (wrote is false), or has (true):
 * fs.c's remembered lookups and the folder listings here are stale. The
 * listings are forgotten again afterwards, in case another thread listed
 * the folder in between. */
static void note_write(bool wrote)
{
	if (!wrote)
		FS_NoteWrite();
	forget_listings();
}

FILE *bwx_fopen(const char *path, const char *mode)
{
	char os[MAX_PATH];
	FILE *f;
	bwx_ospath(path, os, sizeof(os));
	if (!strpbrk(mode, "wa+")) {
		if (known_missing(os)) {
			errno = ENOENT;
			return NULL;
		}
		return fopen(os, mode);
	}
	note_write(false);
	f = fopen(os, mode);
	note_write(true);
	return f;
}

int bwx_remove(const char *path)
{
	char os[MAX_PATH];
	bool ok;
	note_write(false);
	bwx_ospath(path, os, sizeof(os));
	ok = DeleteFileA(os) || RemoveDirectoryA(os);
	note_write(true);
	if (ok)
		return 0;
	errno = ENOENT;
	return -1;
}

int bwx_rename(const char *from, const char *to)
{
	char a[MAX_PATH], b[MAX_PATH];
	bool ok;
	note_write(false);
	bwx_ospath(from, a, sizeof(a));
	bwx_ospath(to, b, sizeof(b));
	DeleteFileA(b); /* POSIX rename replaces the target */
	ok = MoveFileA(a, b);
	note_write(true);
	if (ok)
		return 0;
	errno = ENOENT;
	return -1;
}

int unlink(const char *path) { return bwx_remove(path); }

int rmdir(const char *path)
{
	char os[MAX_PATH];
	bool ok;
	note_write(false);
	ok = RemoveDirectoryA(bwx_ospath(path, os, sizeof(os)));
	note_write(true);
	return ok ? 0 : -1;
}

int bwx_fileno(FILE *f) { return (int)(intptr_t)f; }

static time_t filetime_to_unix(FILETIME ft)
{
	uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	/* 100 ns ticks since 1601 */
	return t < 116444736000000000ULL ? 0 : (time_t)((t - 116444736000000000ULL) / 10000000ULL);
}

int statvfs(const char *path, struct statvfs *out)
{
	char os[260], root[4] = "D:\\";
	ULARGE_INTEGER avail, total, free_;
	bwx_ospath(path, os, sizeof(os));
	if (os[0] && os[1] == ':')
		root[0] = os[0];
	memset(out, 0, sizeof(*out));
	if (!GetDiskFreeSpaceExA(root, &avail, &total, &free_))
		return -1;
	out->f_bsize = out->f_frsize = 4096;
	out->f_blocks = total.QuadPart / 4096;
	out->f_bfree = free_.QuadPart / 4096;
	out->f_bavail = avail.QuadPart / 4096;
	return 0;
}

int stat(const char *path, struct stat *st)
{
	char os[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA d;
	size_t n;
	bwx_ospath(path, os, sizeof(os));
	/* "D:\maps\" names the folder */
	n = strlen(os);
	while (n > 3 && os[n - 1] == '\\')
		os[--n] = 0;
	memset(st, 0, sizeof(*st));
	if (n == 2 || (n == 3 && os[2] == '\\')) { /* a drive's root */
		st->st_mode = S_IFDIR;
		return 0;
	}
	if (known_missing(os) || !GetFileAttributesExA(os, GetFileExInfoStandard, &d)) {
		errno = ENOENT;
		return -1;
	}
	st->st_mode = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
	st->st_size = (off_t)d.nFileSizeLow;
	st->st_mtime = filetime_to_unix(d.ftLastWriteTime);
	st->st_atime = filetime_to_unix(d.ftLastAccessTime);
	st->st_ctime = filetime_to_unix(d.ftCreationTime);
	return 0;
}

int fstat(int fd, struct stat *st)
{
	FILE *f = (FILE *)(intptr_t)fd;
	long pos, end;
	memset(st, 0, sizeof(*st));
	if (!f || (pos = ftell(f)) < 0 || fseek(f, 0, SEEK_END)) {
		errno = EBADF;
		return -1;
	}
	end = ftell(f);
	fseek(f, pos, SEEK_SET);
	st->st_mode = S_IFREG;
	st->st_size = end;
	return 0;
}

int mkdir(const char *path, mode_t mode)
{
	char os[MAX_PATH];
	bool ok;
	(void)mode;
	note_write(false);
	bwx_ospath(path, os, sizeof(os));
	ok = CreateDirectoryA(os, NULL);
	DWORD err = ok ? 0 : GetLastError();
	note_write(true);
	if (ok)
		return 0;
	errno = err == ERROR_ALREADY_EXISTS ? EEXIST : ENOENT;
	return -1;
}

int access(const char *path, int mode)
{
	struct stat st;
	(void)mode;
	return stat(path, &st);
}

char *getcwd(char *buf, size_t size)
{
	if (!buf || size < 3)
		return NULL;
	strcpy(buf, "D:");
	return buf;
}

int chdir(const char *path)
{
	(void)path;
	return 0;
}

/* --- directories ---------------------------------------------------------------- */

/* The kernel's own directory calls rather than FindFirstFileA: that one
 * opens the folder by its name with a backslash on the end, which the real
 * kernel takes and other kernels (and stricter file systems) refuse. */
struct bwx_dir {
	HANDLE h;
	bool first;
	struct {
		FILE_DIRECTORY_INFORMATION info;
		char name[MAX_PATH];
	} buf;
	struct dirent de;
};

/* a folder by its kernel name, for listing */
static NTSTATUS open_folder(const char *os, HANDLE *h)
{
	char buf[MAX_PATH];
	size_t n = strlen(os);
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attr;
	IO_STATUS_BLOCK io;
	if (n + 2 > sizeof(buf))
		return STATUS_OBJECT_NAME_INVALID;
	memcpy(buf, os, n + 1);
	while (n > 3 && buf[n - 1] == '\\')
		buf[--n] = 0;
	if (n == 2) /* "D:" is the drive's root */
		buf[n++] = '\\', buf[n] = 0;
	RtlInitAnsiString(&name, buf);
	InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, ObDosDevicesDirectory(), NULL);
	return NtOpenFile(h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                  FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

DIR *opendir(const char *path)
{
	char os[MAX_PATH];
	DIR *d;
	NTSTATUS st;
	bwx_ospath(path, os, sizeof(os));
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	st = open_folder(os, &d->h);
	if (!NT_SUCCESS(st)) {
		free(d);
		errno = ENOENT;
		return NULL;
	}
	d->first = true;
	return d;
}

struct dirent *readdir(DIR *d)
{
	IO_STATUS_BLOCK io;
	NTSTATUS st;
	ANSI_STRING all;
	if (!d || !d->h)
		return NULL;
	RtlInitAnsiString(&all, "*");
	memset(&d->buf, 0, sizeof(d->buf));
	st = NtQueryDirectoryFile(d->h, NULL, NULL, NULL, &io, &d->buf, sizeof(d->buf), FileDirectoryInformation,
	                          d->first ? &all : NULL, d->first);
	d->first = false;
	if (!NT_SUCCESS(st))
		return NULL; /* STATUS_NO_MORE_FILES, or an empty folder's STATUS_NO_SUCH_FILE */
	size_t len = d->buf.info.FileNameLength < sizeof(d->de.d_name) - 1 ? d->buf.info.FileNameLength : sizeof(d->de.d_name) - 1;
	memcpy(d->de.d_name, d->buf.info.FileName, len);
	d->de.d_name[len] = 0;
	d->de.d_type = (d->buf.info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
	return &d->de;
}

int closedir(DIR *d)
{
	if (!d)
		return -1;
	if (d->h)
		NtClose(d->h);
	free(d);
	return 0;
}

/* --- remembered folder listings ---------------------------------------------------------
 * The engine asks for many files that aren't there: it tries each search
 * path (stbuilt, base, data) and each image type in turn, and in a match
 * the game looks for files such as a model's .phy as things first spawn.
 * Each miss is a walk through the file system's folders, and on a DVD can
 * be a seek, which stalls the frame that asked. Nothing changes a
 * console's game folders but the engine, so the first look into a folder
 * lists it once, and misses in it are answered from the list. Files that
 * are there still go to the kernel. Any write by the engine forgets every
 * list. */
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif
#define LISTINGS 512 /* folders, a power of two */

typedef struct {
	char *path;      /* lower case, no trailing backslash; NULL: a free slot */
	bool missing;    /* the folder isn't there */
	char *pool;      /* the names, lower case, each ended by a 0 */
	uint32_t *slots; /* hash table of offsets into pool, plus one; 0 is empty */
	uint32_t mask;
} listing_t;

static listing_t listings[LISTINGS];
static int numlistings;
static pthread_mutex_t listlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t name_hash(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (uint8_t)tolower((unsigned char)s[i])) * 16777619u;
	return h;
}

static void forget_listings_locked(void)
{
	for (int i = 0; i < LISTINGS; i++) {
		free(listings[i].path), free(listings[i].pool), free(listings[i].slots);
		memset(&listings[i], 0, sizeof(listings[i]));
	}
	numlistings = 0;
}

static void forget_listings(void)
{
	pthread_mutex_lock(&listlock);
	forget_listings_locked();
	pthread_mutex_unlock(&listlock);
}

static bool listing_has(const listing_t *l, const char *name, size_t n)
{
	for (uint32_t i = name_hash(name, n) & l->mask; l->slots[i]; i = (i + 1) & l->mask) {
		const char *e = l->pool + l->slots[i] - 1;
		if (!strncasecmp(e, name, n) && !e[n])
			return true;
	}
	return false;
}

/* reads a folder's names into l; false if they couldn't all be read */
static bool list_folder(const char *os, listing_t *l)
{
	HANDLE h;
	NTSTATUS st = open_folder(os, &h);
	if (st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND) {
		l->missing = true;
		return true;
	}
	if (!NT_SUCCESS(st))
		return false;
	size_t used = 0, cap = 1024;
	uint32_t count = 0;
	char *pool = malloc(cap);
	bool first = true, ok = pool != NULL;
	struct {
		FILE_DIRECTORY_INFORMATION info;
		char name[MAX_PATH];
	} buf;
	ANSI_STRING all;
	RtlInitAnsiString(&all, "*");
	while (ok) {
		IO_STATUS_BLOCK io;
		memset(&buf, 0, sizeof(buf));
		st = NtQueryDirectoryFile(h, NULL, NULL, NULL, &io, &buf, sizeof(buf), FileDirectoryInformation, first ? &all : NULL,
		                          first);
		first = false;
		if (st == STATUS_NO_MORE_FILES || st == STATUS_NO_SUCH_FILE)
			break;
		if (!NT_SUCCESS(st)) {
			ok = false;
			break;
		}
		size_t n = (size_t)buf.info.FileNameLength < sizeof(buf.name) - 1 ? (size_t)buf.info.FileNameLength : sizeof(buf.name) - 1;
		if (used + n + 1 > cap) {
			char *grown = realloc(pool, cap = (used + n + 1) * 2);
			if (!grown) {
				ok = false;
				break;
			}
			pool = grown;
		}
		for (size_t i = 0; i < n; i++)
			pool[used + i] = (char)tolower((unsigned char)buf.info.FileName[i]);
		pool[used + n] = 0;
		used += n + 1;
		count++;
	}
	NtClose(h);
	uint32_t size = 16;
	while (size < count * 2)
		size *= 2;
	uint32_t *slots = ok ? calloc(size, sizeof(*slots)) : NULL;
	if (!slots) {
		free(pool);
		return false;
	}
	for (size_t o = 0; o < used; o += strlen(pool + o) + 1) {
		uint32_t i = name_hash(pool + o, strlen(pool + o)) & (size - 1);
		while (slots[i])
			i = (i + 1) & (size - 1);
		slots[i] = (uint32_t)o + 1;
	}
	l->pool = pool, l->slots = slots, l->mask = size - 1;
	return true;
}

static bool missing_locked(const char *os, size_t n, int depth);

/* the listing of the folder os[0..n), made if need be; NULL if unknown */
static listing_t *folder_listing(const char *os, size_t n, int depth)
{
	char dir[MAX_PATH];
	if (n < 2 || n >= sizeof(dir) || depth > 32)
		return NULL;
	for (size_t i = 0; i < n; i++)
		dir[i] = (char)tolower((unsigned char)os[i]);
	dir[n] = 0;
	uint32_t i = name_hash(dir, n) & (LISTINGS - 1);
	while (listings[i].path && strcmp(listings[i].path, dir))
		i = (i + 1) & (LISTINGS - 1);
	if (listings[i].path)
		return &listings[i];
	listing_t l = {0};
	/* a folder its parent's list doesn't name isn't there; a drive's root always is */
	if (n > 2 && missing_locked(dir, n, depth + 1))
		l.missing = true;
	else if (!list_folder(dir, &l))
		return NULL;
	if (numlistings >= LISTINGS * 3 / 4) {
		forget_listings_locked();
		i = name_hash(dir, n) & (LISTINGS - 1);
	}
	l.path = strdup(dir);
	if (!l.path) {
		free(l.pool), free(l.slots);
		return NULL;
	}
	listings[i] = l;
	numlistings++;
	return &listings[i];
}

/* true when os[0..n) surely isn't there: its folder's list doesn't name it */
static bool missing_locked(const char *os, size_t n, int depth)
{
	size_t slash = n;
	while (slash > 0 && os[slash - 1] != '\\')
		slash--;
	if (slash < 3 || slash == n) /* a drive, or a name ending in a backslash */
		return false;
	/* "D:\x" is in the folder "D:", the others in the path before the backslash */
	listing_t *l = folder_listing(os, slash == 3 ? 2 : slash - 1, depth);
	if (!l)
		return false;
	return l->missing || !listing_has(l, os + slash, n - slash);
}

static bool known_missing(const char *os)
{
	if (!(os[0] && os[1] == ':' && os[2] == '\\'))
		return false;
	pthread_mutex_lock(&listlock);
	bool missing = missing_locked(os, strlen(os), 0);
	pthread_mutex_unlock(&listlock);
	return missing;
}

/* --- time ------------------------------------------------------------------------- */

int clock_gettime(clockid_t id, struct timespec *ts)
{
	if (id == CLOCK_REALTIME) {
		LARGE_INTEGER st;
		KeQuerySystemTime(&st);
		uint64_t t = (uint64_t)st.QuadPart - 116444736000000000ULL;
		ts->tv_sec = (time_t)(t / 10000000ULL);
		ts->tv_nsec = (long)(t % 10000000ULL) * 100;
		return 0;
	}
	/* the performance counter runs at 733 MHz / 1.125 MHz depending on the
	 * kernel; KeQueryPerformanceFrequency says which */
	static uint64_t freq;
	if (!freq)
		freq = KeQueryPerformanceFrequency();
	uint64_t c = KeQueryPerformanceCounter();
	ts->tv_sec = (time_t)(c / freq);
	ts->tv_nsec = (long)((c % freq) * 1000000000ULL / freq);
	return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	struct timespec ts;
	(void)tz;
	clock_gettime(CLOCK_REALTIME, &ts);
	tv->tv_sec = (long)ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
	return 0;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
	struct tm *r = localtime(t);
	if (!r)
		return NULL;
	*out = *r;
	return out;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
	struct tm *r = gmtime(t);
	if (!r)
		return NULL;
	*out = *r;
	return out;
}

int usleep(unsigned int usec)
{
	/* the kernel's waits count 100 ns, negative for "from now" */
	LARGE_INTEGER d;
	d.QuadPart = -(LONGLONG)usec * 10;
	KeDelayExecutionThread(UserMode, FALSE, &d);
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	LARGE_INTEGER d;
	d.QuadPart = -((LONGLONG)req->tv_sec * 10000000 + req->tv_nsec / 100);
	KeDelayExecutionThread(UserMode, FALSE, &d);
	if (rem)
		rem->tv_sec = 0, rem->tv_nsec = 0;
	return 0;
}

/* no /proc: the engine falls back to its other ways of finding itself */
ssize_t readlink(const char *path, char *buf, size_t size)
{
	(void)path, (void)buf, (void)size;
	errno = ENOENT;
	return -1;
}

unsigned int sleep(unsigned int sec)
{
	Sleep(sec * 1000);
	return 0;
}

long sysconf(int name) { return name == _SC_NPROCESSORS_ONLN ? 1 : -1; }
int getpid(void) { return 1; }
int isatty(int fd) { (void)fd; return 0; }

int setenv(const char *name, const char *value, int overwrite)
{
	(void)name, (void)value, (void)overwrite;
	return 0;
}

/* --- strings --------------------------------------------------------------------- */

size_t strnlen(const char *s, size_t n)
{
	size_t i = 0;
	while (i < n && s[i])
		i++;
	return i;
}

char *strndup(const char *s, size_t n)
{
	size_t len = strnlen(s, n);
	char *r = malloc(len + 1);
	if (!r)
		return NULL;
	memcpy(r, s, len);
	r[len] = 0;
	return r;
}

char *strtok_r(char *s, const char *delim, char **save)
{
	char *end;
	if (!s)
		s = *save;
	if (!s)
		return NULL;
	s += strspn(s, delim);
	if (!*s) {
		*save = s;
		return NULL;
	}
	end = s + strcspn(s, delim);
	if (*end)
		*end++ = 0;
	*save = end;
	return s;
}

char *strsep(char **sp, const char *delim)
{
	char *s = *sp, *e;
	if (!s)
		return NULL;
	e = s + strcspn(s, delim);
	if (*e)
		*e++ = 0, *sp = e;
	else
		*sp = NULL;
	return s;
}

char *strcasestr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return (char *)hay;
	return n ? NULL : (char *)hay;
}

/* --- threads ------------------------------------------------------------------------ */

typedef struct {
	void *(*fn)(void *);
	void *arg;
} thread_start_t;

static DWORD WINAPI thread_main(LPVOID p)
{
	thread_start_t s = *(thread_start_t *)p;
	free(p);
	s.fn(s.arg);
	return 0;
}

int pthread_attr_init(pthread_attr_t *a) { a->detach = PTHREAD_CREATE_JOINABLE; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { (void)a, (void)size; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) { a->detach = state; return 0; }

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
	thread_start_t *s = malloc(sizeof(*s));
	HANDLE h;
	(void)attr;
	if (!s)
		return ENOMEM;
	s->fn = fn, s->arg = arg;
	/* the engine's workers keep big buffers on their stacks too */
	h = CreateThread(NULL, 256 * 1024, thread_main, s, 0, NULL);
	if (!h) {
		free(s);
		return EAGAIN;
	}
	if (attr && attr->detach == PTHREAD_CREATE_DETACHED) {
		CloseHandle(h);
		h = NULL;
	}
	*t = h;
	return 0;
}

int pthread_detach(pthread_t t)
{
	CloseHandle((HANDLE)t);
	return 0;
}

int pthread_join(pthread_t t, void **ret)
{
	WaitForSingleObject((HANDLE)t, INFINITE);
	CloseHandle((HANDLE)t);
	if (ret)
		*ret = NULL;
	return 0;
}

pthread_t pthread_self(void) { return (pthread_t)(uintptr_t)GetCurrentThreadId(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static void mutex_ready(pthread_mutex_t *m)
{
	if (m->init == 2)
		return;
	if (InterlockedCompareExchange(&m->init, 1, 0) == 0) {
		CRITICAL_SECTION *cs = malloc(sizeof(*cs));
		InitializeCriticalSection(cs);
		m->cs = cs;
		InterlockedExchange(&m->init, 2);
	} else {
		while (m->init != 2)
			SwitchToThread();
	}
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	(void)a;
	m->init = 0, m->cs = NULL;
	mutex_ready(m);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
	if (m->init == 2) {
		DeleteCriticalSection((CRITICAL_SECTION *)m->cs);
		free(m->cs);
	}
	m->init = 0, m->cs = NULL;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
	mutex_ready(m);
	EnterCriticalSection((CRITICAL_SECTION *)m->cs);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
	mutex_ready(m);
	return TryEnterCriticalSection((CRITICAL_SECTION *)m->cs) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
	LeaveCriticalSection((CRITICAL_SECTION *)m->cs);
	return 0;
}

static void cond_ready(pthread_cond_t *c)
{
	if (c->init == 2)
		return;
	if (InterlockedCompareExchange(&c->init, 1, 0) == 0) {
		c->sem = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
		InterlockedExchange(&c->init, 2);
	} else {
		while (c->init != 2)
			SwitchToThread();
	}
}

int pthread_condattr_init(pthread_condattr_t *a) { a->clock = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, int clock) { a->clock = clock; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
	c->init = 0, c->sem = NULL, c->waiters = 0;
	c->clock = a ? a->clock : CLOCK_REALTIME;
	cond_ready(c);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
	if (c->init == 2)
		CloseHandle(c->sem);
	c->init = 0;
	return 0;
}

/* Waiters are counted under the mutex; a signal releases the semaphore once
 * per waiter it wakes. A wake-up can be spurious, as POSIX allows: callers
 * re-check their condition in a loop. */
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	cond_ready(c);
	InterlockedIncrement(&c->waiters);
	pthread_mutex_unlock(m);
	WaitForSingleObject(c->sem, INFINITE);
	pthread_mutex_lock(m);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
	struct timespec now;
	int64_t ms;
	DWORD r;
	cond_ready(c);
	clock_gettime(c->clock, &now);
	ms = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
	if (ms < 0)
		ms = 0;
	InterlockedIncrement(&c->waiters);
	pthread_mutex_unlock(m);
	r = WaitForSingleObject(c->sem, ms > 0x7ffffffe ? 0x7ffffffe : (DWORD)ms);
	if (r != WAIT_OBJECT_0) {
		/* take this waiter back off the count; when a signal has already
		 * counted it, its release stays in the semaphore and wakes a later
		 * waiter early, which POSIX allows */
		for (;;) {
			long w = c->waiters;
			if (w <= 0 || InterlockedCompareExchange(&c->waiters, w - 1, w) == w)
				break;
		}
	}
	pthread_mutex_lock(m);
	return r == WAIT_OBJECT_0 ? 0 : ETIMEDOUT;
}

int pthread_cond_signal(pthread_cond_t *c)
{
	cond_ready(c);
	for (;;) {
		long w = c->waiters;
		if (w <= 0)
			return 0;
		if (InterlockedCompareExchange(&c->waiters, w - 1, w) == w) {
			ReleaseSemaphore(c->sem, 1, NULL);
			return 0;
		}
	}
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
	long w;
	cond_ready(c);
	w = InterlockedExchange(&c->waiters, 0);
	if (w > 0)
		ReleaseSemaphore(c->sem, w, NULL);
	return 0;
}

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
	if (InterlockedCompareExchange(&o->done, 1, 0) == 0)
		fn();
	return 0;
}

/* --- network --------------------------------------------------------------------- */

/* 0 not started, 1 starting (in its own thread: DHCP can take seconds), 2
 * up, -1 failed or turned off */
static volatile long net_state;

static DWORD WINAPI net_thread(LPVOID p)
{
	(void)p;
	if (nxNetInit(NULL) == 0)
		InterlockedExchange(&net_state, 2);
	else
		InterlockedExchange(&net_state, -1);
	return 0;
}

/* called at start-up (sys_xbox.c) unless -nonet is given */
void bwx_net_start(void)
{
	if (InterlockedCompareExchange(&net_state, 1, 0) == 0) {
		HANDLE h = CreateThread(NULL, 64 * 1024, net_thread, NULL, 0, NULL);
		if (h)
			CloseHandle(h);
		else
			net_state = -1;
	}
}

void bwx_net_disable(void) { InterlockedCompareExchange(&net_state, -1, 0); }

int bwx_net_up(void) { return net_state == 2; }

#undef socket
int bwx_socket(int domain, int type, int proto)
{
	if (!bwx_net_up()) {
		errno = ENETDOWN;
		return -1;
	}
	return lwip_socket(domain, type, proto);
}

