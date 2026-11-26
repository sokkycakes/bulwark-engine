/* Bulwark engine - the few Windows names gl_xbox.c uses, for the host test
 * (gltextest.c). ISC licence, see LICENSE. */
#ifndef BW_STUB_WINDOWS_H
#define BW_STUB_WINDOWS_H
#include <stdint.h>
typedef uint32_t DWORD;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
DWORD GetTickCount(void);
#endif
