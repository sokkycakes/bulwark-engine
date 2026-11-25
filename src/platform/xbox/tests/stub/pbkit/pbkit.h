/* Bulwark engine - pbkit's calls for the host test (gltextest.c), which
 * runs gl_xbox.c's texture code without a GPU; the register names come
 * from nxdk's own nv_regs.h ($NXDK_DIR/lib/pbkit on the include path).
 * ISC licence, see LICENSE. */
#ifndef BW_STUB_PBKIT_H
#define BW_STUB_PBKIT_H
#include <stdint.h>
#include "../windows.h"
#include "nv_regs.h"
uint32_t *pb_begin(void);
void pb_end(uint32_t *p);
void pb_reset(void);
int pb_busy(void);
int pb_finished(void);
DWORD *pb_back_buffer(void);
DWORD pb_back_buffer_width(void);
DWORD pb_back_buffer_height(void);
DWORD pb_back_buffer_pitch(void);
void pb_size(DWORD size);
int pb_init(void);
void pb_kill(void);
void pb_show_front_screen(void);
void pb_target_back_buffer(void);
DWORD pb_get_vbl_counter(void);
DWORD pb_wait_for_vbl(void);
#endif
