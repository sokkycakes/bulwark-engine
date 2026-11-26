/* Bulwark engine - POSIX types for the Xbox build. ISC licence, see LICENSE. */
#ifndef BW_XBOX_SYS_TYPES_H
#define BW_XBOX_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include <limits.h>
/* as lwIP defines it when SSIZE_MAX is missing */
#ifndef SSIZE_MAX
typedef int ssize_t;
#define SSIZE_MAX INT_MAX
#endif
typedef uint32_t mode_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long suseconds_t;

#endif
