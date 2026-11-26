/* Bulwark engine - pbkit's calls for the host test (gltextest.c), which
 * runs gl_xbox.c's texture code without a GPU; the register names come
 * from nxdk's own nv_regs.h and outer.h ($NXDK_DIR/lib/pbkit on the
 * include path).
 * ISC licence, see LICENSE. */
#ifndef BW_STUB_PBKIT_H
#define BW_STUB_PBKIT_H
#include <stdint.h>
#include "../windows.h"
#include "nv_regs.h"
/* the register offsets, as the real pbkit.h gets them; the registers
 * themselves are a host array (gltextest.c), not the GPU's MMIO window */
#include "outer.h"
extern uint32_t bw_stub_videoregs[];
#undef VIDEO_BASE
#define VIDEO_BASE ((uintptr_t)bw_stub_videoregs)
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
