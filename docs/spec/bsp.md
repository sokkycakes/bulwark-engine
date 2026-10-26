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

## Rendering data (Q3 family)

`src/render/r_world.c` reads these lumps as well.

| lump | content | record size |
|---|---|---|
| 10 | draw vertices | 44 (IBSP), 80 (RBSP/FBSP) |
| 11 | indices, int32 | 4 |
| 13 | draw surfaces | 104 (IBSP), 148 (RBSP/FBSP) |
| 14 | lightmap pages, RGB | 128×128×3 (IBSP/RBSP), 512×512×3 (FBSP) |
| 15 | light grid points | 8 (IBSP), 30 (RBSP/FBSP) |
| 17 | light grid indices, uint16 (RBSP/FBSP only) | 2 |

Vertex: position[3], texture st[2], lightmap st[2] (all floats), then in
IBSP normal[3] and colour RGBA8 at byte 40. In RBSP/FBSP the extra lightmap
coordinate sets for light styles sit between, which puts the normal at byte
52. Surface: shader, fog, type (1 planar, 2 patch, 3 triangle soup, 4 flare),
first vertex, vertex count, first index, index count, then the lightmap
index at byte 28 (IBSP) or 36 (RBSP/FBSP); the patch width and height are
the last two ints of the record. Triangle winding is clockwise seen from the
front, so the renderer swaps two indices for GL's counter-clockwise faces.

Established on Nuclide's maps (ai.bsp, demo.bsp):

- FBSP lightmap pages are 512×512. The surfaces' own lightmap x/y/size
  fields are all 0; the vertices' lightmap coordinates are already
  normalised over the whole page.
- Surfaces name pages directly. Maps lit with deluxemaps store twice as
  many pages, alternating light and direction, so surfaces only reference
  even pages (demo.bsp: indices 0 and 2 out of 4 pages). The renderer
  uploads every page and keeps the indices; a map whose pages are even in
  number and whose surfaces use only even ones is taken to have deluxemaps.
- The FBSP light grid is lump 15 holding 30-byte points and lump 17 holding
  one uint16 point index per grid cell (ai.bsp: 22 points, 4590 indices,
  which matches the cell count for the default 64×64×128 grid over the
  world bounds). The renderer reads bytes 0-2 as the style-0 ambient colour,
  12-14 as the style-0 directed colour and 28-29 as the direction. The
  middle bytes are taken to be the other three styles' colours. That
  layout and the direction byte order are guesses that look right in game,
  not something a spec states.
- The grid starts at the world bounds rounded inward to the grid size, and
  worldspawn's `gridsize` key overrides 64 64 128.
