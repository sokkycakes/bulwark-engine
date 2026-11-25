/* Bulwark engine - sys/statvfs.h for the Xbox build: free space on the
 * drive a path is on (posix_xbox.c). ISC licence, see LICENSE. */
#ifndef BW_XBOX_SYS_STATVFS_H
#define BW_XBOX_SYS_STATVFS_H

struct statvfs {
	unsigned long f_bsize, f_frsize;
	unsigned long long f_blocks, f_bfree, f_bavail;
};
int statvfs(const char *path, struct statvfs *out);

#endif
