/* Bulwark engine - fcntl.h for the Xbox build: the engine only sets
 * sockets non-blocking, which lwIP's fcntl (sys/socket.h) does.
 * ISC licence, see LICENSE. */
#ifndef BW_XBOX_FCNTL_H
#define BW_XBOX_FCNTL_H
#include <sys/socket.h>
#endif
