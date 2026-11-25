/* Bulwark engine - gettimeofday on the Xbox (posix_xbox.c). ISC licence,
 * see LICENSE. */
#ifndef BW_XBOX_SYS_TIME_H
#define BW_XBOX_SYS_TIME_H
#include <sys/types.h>
#include <time.h>
#include <sys/socket.h> /* struct timeval comes from lwIP */
int gettimeofday(struct timeval *tv, void *tz);
#endif
