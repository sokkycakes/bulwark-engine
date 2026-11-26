/* Bulwark engine - directory listing on the Xbox (posix_xbox.c).
 * ISC licence, see LICENSE. */
#ifndef BW_XBOX_DIRENT_H
#define BW_XBOX_DIRENT_H

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
	unsigned char d_type;
	char d_name[256];
};

typedef struct bwx_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
