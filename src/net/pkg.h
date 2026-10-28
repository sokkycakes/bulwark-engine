/* Bulwark engine - the package manager. ISC licence, see LICENSE. */
#ifndef BW_PKG_H
#define BW_PKG_H

#include <stdbool.h>
#include <stddef.h>

void PKG_Init(void);
void PKG_Frame(void); /* finishes downloads; the first one runs the pkg_autoupdate check */
/* a line about running downloads; false when there are none */
bool PKG_Progress(char *out, size_t outsize);
/* the menus' getpackagemanagerinfo, by its GPMI_ numbers; NULL past the end */
const char *PKG_Info(int n, int prop);

#endif
