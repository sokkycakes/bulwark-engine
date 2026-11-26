/* Bulwark engine - S3TC (DXT1 and DXT5) block compression for the Xbox
 * GPU's textures. ISC licence, see LICENSE.
 *
 * Plain C with no Xbox dependencies, so the host tests can run it
 * (src/platform/xbox/tests/dxttest.c). A block is 4x4 texels; pixels are
 * RGBA bytes, rows `pitch` texels apart. */
#ifndef DXT_XBOX_H
#define DXT_XBOX_H

#include <stddef.h>
#include <stdint.h>

#define DXT1_BLOCK_BYTES 8
#define DXT5_BLOCK_BYTES 16

/* one block: 16 RGBA texels, row by row */
/* DXT1: four colours; texels with alpha below 128 become transparent
 * (the three-colour mode) when `punch` is set, otherwise alpha is ignored */
void DXT_EncodeBlock1(uint8_t out[8], const uint8_t rgba[64], int punch);
/* DXT5: interpolated alpha and four colours */
void DXT_EncodeBlock5(uint8_t out[16], const uint8_t rgba[64]);
void DXT_DecodeBlock1(const uint8_t in[8], uint8_t rgba[64]);
void DXT_DecodeBlock5(const uint8_t in[16], uint8_t rgba[64]);

/* the bytes of a w x h level: whole blocks, at least one */
size_t DXT_LevelBytes(int w, int h, int blockbytes);

/* a whole w x h RGBA image into blocks, row by row (the layout the GPU
 * reads); sides under 4 are padded by repeating the edge */
void DXT_EncodeImage(uint8_t *out, const uint8_t *rgba, int w, int h, int blockbytes, int punch);
void DXT_DecodeImage(const uint8_t *in, uint8_t *rgba, int w, int h, int blockbytes);

/* the 16 texels of the block at bx,by (block units) of a w x h image,
 * padded at the edges */
void DXT_GatherBlock(uint8_t block[64], const uint8_t *rgba, int w, int h, int bx, int by);

#endif
