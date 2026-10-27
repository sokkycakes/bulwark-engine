/* Bulwark engine - BSP file tools (BSPX lumps, realigning, entity search).
 * ISC licence, see LICENSE. */
#ifndef BW_BSPTOOL_H
#define BW_BSPTOOL_H

#include "common.h"

/* adds or replaces BSPX lumps in a map file in the writable game directory
 * (the old file is kept as .bak) */
bool BSPX_SetLumps(const char *bspname, const char *const *names, const void *const *datas, const size_t *lens, int count);
void BSP_InitTools(void);

#endif
