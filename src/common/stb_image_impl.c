/* Bulwark engine - the stb_image implementation (public domain), built once
 * for the renderer's image decoding and the file system's zip inflate.
 * ISC licence, see LICENSE. */
#include "common.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_SUPPORT_ZLIB
#define STBI_MALLOC(sz) Mem_Alloc(sz)
#define STBI_REALLOC(p, sz) Mem_Realloc(p, sz)
#define STBI_FREE(p) Mem_Free(p)
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "../thirdparty/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
