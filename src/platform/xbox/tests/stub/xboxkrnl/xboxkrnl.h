/* Bulwark engine - the kernel calls gl_xbox.c makes, for the host test
 * (gltextest.c): contiguous memory comes from the C heap. ISC licence,
 * see LICENSE. */
#ifndef BW_STUB_XBOXKRNL_H
#define BW_STUB_XBOXKRNL_H
#include <stddef.h>
#include "../windows.h"
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOMBINE 0x400
void *MmAllocateContiguousMemoryEx(size_t size, uintptr_t lo, uintptr_t hi, size_t align, DWORD protect);
void MmFreeContiguousMemory(void *p);
ULONGLONG KeQueryPerformanceCounter(void);
ULONGLONG KeQueryPerformanceFrequency(void);
#endif
