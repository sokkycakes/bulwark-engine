/* Bulwark engine - the unistd.h subset the engine uses, on the Xbox
 * (posix_xbox.c). Sockets close through lwIP's names (sys/socket.h).
 * ISC licence, see LICENSE. */
#ifndef BW_XBOX_UNISTD_H
#define BW_XBOX_UNISTD_H

#include <sys/types.h>

#ifndef R_OK
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif

int usleep(unsigned int usec);
unsigned int sleep(unsigned int sec);
int access(const char *path, int mode);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);
long sysconf(int name);
int getpid(void);
int isatty(int fd);
#define _SC_NPROCESSORS_ONLN 84

#endif
