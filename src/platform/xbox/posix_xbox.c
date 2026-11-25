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

FILE *bwx_fopen(const char *path, const char *mode)
{
	char os[MAX_PATH];
	return fopen(bwx_ospath(path, os, sizeof(os)), mode);
}

int bwx_remove(const char *path)
{
	char os[MAX_PATH];
	bwx_ospath(path, os, sizeof(os));
	if (DeleteFileA(os) || RemoveDirectoryA(os))
		return 0;
	errno = ENOENT;
	return -1;
}

int bwx_rename(const char *from, const char *to)
{
	char a[MAX_PATH], b[MAX_PATH];
	bwx_ospath(from, a, sizeof(a));
	bwx_ospath(to, b, sizeof(b));
	DeleteFileA(b); /* POSIX rename replaces the target */
	if (MoveFileA(a, b))
		return 0;
	errno = ENOENT;
	return -1;
}

int unlink(const char *path) { return bwx_remove(path); }

int rmdir(const char *path)
{
	char os[MAX_PATH];
	return RemoveDirectoryA(bwx_ospath(path, os, sizeof(os))) ? 0 : -1;
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
	if (!GetFileAttributesExA(os, GetFileExInfoStandard, &d)) {
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
	(void)mode;
	bwx_ospath(path, os, sizeof(os));
	if (CreateDirectoryA(os, NULL))
		return 0;
	errno = GetLastError() == ERROR_ALREADY_EXISTS ? EEXIST : ENOENT;
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

DIR *opendir(const char *path)
{
	char os[MAX_PATH];
	size_t n;
	DIR *d;
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attr;
	IO_STATUS_BLOCK io;
	NTSTATUS st;
	bwx_ospath(path, os, sizeof(os));
	n = strlen(os);
	while (n > 3 && os[n - 1] == '\\')
		os[--n] = 0;
	if (n == 2) /* "D:" is the drive's root */
		os[n++] = '\\', os[n] = 0;
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	RtlInitAnsiString(&name, os);
	InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, ObDosDevicesDirectory(), NULL);
	st = NtOpenFile(&d->h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
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

int pthread_attr_init(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { (void)a, (void)size; return 0; }

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

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
	(void)a;
	c->init = 0, c->sem = NULL, c->waiters = 0;
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

