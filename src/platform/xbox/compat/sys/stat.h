/* Bulwark engine - file status on the Xbox (posix_xbox.c). ISC licence,
 * see LICENSE. */
#ifndef BW_XBOX_SYS_STAT_H
#define BW_XBOX_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

struct stat {
	mode_t st_mode;
	off_t st_size;
	time_t st_mtime;
	time_t st_atime;
	time_t st_ctime;
};

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);

#endif
