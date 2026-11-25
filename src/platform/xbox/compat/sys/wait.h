/* Bulwark engine - sys/wait.h for the Xbox build: there are no child
 * processes, only the names. ISC licence, see LICENSE. */
#ifndef BW_XBOX_SYS_WAIT_H
#define BW_XBOX_SYS_WAIT_H

#include <sys/types.h>

#define WIFEXITED(s) 1
#define WEXITSTATUS(s) ((s) & 0xFF)
#define WNOHANG 1
static inline pid_t waitpid(pid_t pid, int *status, int options)
{
	(void)pid, (void)status, (void)options;
	return -1;
}

#endif
