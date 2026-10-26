# BSP formats

`src/server/bspfile.c` reads what the server needs for spawning and collision.

## Q3 family (IBSP, RBSP, FBSP)

Header: 4-byte ident, int version, then a lump directory of (offset, length)
pairs. IBSP has 17 lumps; RBSP and FBSP (what Nuclide's tools write) have 18.

| lump | content | record size |
|---|---|---|
| 0 | entities (text) | — |
| 1 | shaders: name[64], surface flags, content flags | 72 |
| 2 | planes: normal[3], dist | 16 |
| 7 | models: mins[3], maxs[3], first surface, num surfaces, first brush, num brushes | 40 |
| 8 | brushes: first side, num sides, shader | 12 |
| 9 | brush sides: plane, shader (IBSP) | 8 |
| 9 | brush sides: plane, shader, draw surface (RBSP/FBSP) | 12 |

The FBSP brush-side size was confirmed on Nuclide's maps: with 12-byte sides
the brushes' side counts sum exactly to the lump's record count.

Collision sweeps an axis-aligned box through each brush by clipping against
its planes (offset by the box's extents), with a 1/32 unit epsilon.

## Q1 family (29, 30, BSP2)

Version int, then 15 lumps. Only the entity lump (0) and the model lump (14,
64-byte records starting with mins and maxs) are read. Q1 clipping hulls are
not loaded yet, so Q1 maps currently have no world collision.

## Contents

Brush contents are Q3 bit flags (SOLID 1, LAVA 8, SLIME 0x10, WATER 0x20,
PLAYERCLIP 0x10000, BODY 0x2000000, CORPSE 0x4000000). `pointcontents`
converts them to the Quake negative CONTENT_* values that QC expects.
