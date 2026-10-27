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

## Q1 family (29, BSP2, GoldSrc 30)

Version int (29, or 30 for GoldSrc) or the magic "BSP2", then 15 lumps: entities, planes,
textures, vertexes, visibility, nodes, texinfo, faces, lighting, clipnodes,
leafs, marksurfaces, edges, surfedges, models. Layouts follow the public
"Unofficial Quake Specs"; BSP2 widens the 16-bit fields to 32 bits.

| record | BSP29 | BSP2 |
| --- | --- | --- |
| plane | 20 (normal, dist, type) | same |
| node | 24: plane int, children short[2], bounds short[6], firstface ushort, numfaces ushort | 44: children int, bounds float, face fields int |
| leaf | 28: contents, visofs, bounds short[6], firstmark ushort, nummarks ushort, ambient[4] | 44 |
| clipnode | 8: plane int, children short[2] | 12 |
| marksurface | ushort | uint |
| face | 20: plane ushort, side short, firstedge int, numedges short, texinfo short, styles[4], lightofs int | 28 |
| edge | ushort[2] | uint[2] |
| texinfo | 40: s[4], t[4], miptex, flags (1 = special: sky and liquids, unlit) | same |
| model | 64: mins, maxs, origin, headnode[4], visleafs, firstface, numfaces | same |

- Children >= 0 are nodes; negative children are leaf -(child + 1) in the
  node tree, or Q1 contents (-1 empty, -2 solid, -3 water, -4 slime, -5 lava,
  -6 sky) in clip hulls.
- Leaf 0 is the shared solid leaf. Leaf n's visibility row is cluster n - 1.
- Visibility rows are run-length coded: a zero byte is followed by a count of
  zero bytes.
- Collision uses the hulls: hull 0 is the node tree with leaf children turned
  into contents, hull 1 is (-16,-16,-24)..(16,16,32) and hull 2
  (-32,-32,-24)..(32,32,64). A box picks hull 0 when it is under 3 units
  wide, hull 1 when up to 32, else hull 2. The box's offset into the hull is
  hull_mins - box_mins.
- Lightmaps: s and t from texinfo, divided by 16; extents are floor(min/16)
  to ceil(max/16), inclusive. Up to four styles follow each other. Light
  values are overbright: 128 is full brightness, so they are drawn with
  `r_lightmapscale` 2 like Q3 maps. A `.lit` file next to the map ("QLIT",
  version 1, then RGB triples) gives coloured light.
- Textures come from the map's miptex lump through the Quake palette.
  Names starting `sky` are sky, `*` (or `!`) liquids, `{` fences (index 255
  transparent). Palette entries 224 to 254 are fullbright. A material file or
  image with the texture's name replaces the map's pixels.
- `tools/mkq1map_test.py` writes `tests/data/maps/q1test.bsp`, a two-room
  map checked by `tests/bsp/bsptest.c`.

### GoldSrc (version 30)

Same lumps and record sizes as BSP29. The differences, from the public
format descriptions and checked on `tests/data/maps/hltest.bsp`:

- Four hulls: 1 standing (-16,-16,-36)..(16,16,36), 2 large
  (-32,-32,-32)..(32,32,32), 3 crouching (-16,-16,-18)..(16,16,18); the
  model's headnode[3] is hull 3. A box up to 8 units wide uses hull 0; up to
  36 wide, hull 3 when it is at most 36 tall, else hull 1; wider, hull 2.
- Each miptex carries its own palette after the smallest mip: a short count
  (256), then 768 bytes of RGB. No fullbright range.
- A miptex whose offsets are 0 has its pixels in a WAD. Worldspawn's `wad`
  key lists them by the mapper's paths, split by `;`; only the file name is
  used, looked up in the game directories and then `wads/`. WAD3 (and WAD2):
  "WAD3", lump count, directory offset; 32-byte entries of file offset, disk
  size, size, type (0x43 miptex, 0x44 in WAD2), compression, 2 pad bytes,
  name[16]. A miptex lump is laid out as in the map, palette included.
- Lighting is RGB: lightofs counts bytes, and each style's block is
  w*h*3. 255 is full brightness, so values are halved on load to share Q1's
  overbright scale. `.lit` files are ignored.
- Sky comes from worldspawn's `skyname`, faces named without the underscore
  under `gfx/env/` (`gfx/env/desertrt.tga`); either spelling is accepted.
- `aaatrigger`, `null` and `origin` are not drawn.
- `tools/mkq1map_test.py --hl` writes the BSP30 twin of the test map and
  `tests/data/hltest.wad` with its floor texture.

## Visibility

Both families load the node tree, leafs (with bounding boxes and their
face lists) and uncompressed visibility rows. Each frame the renderer finds
the view's leaf, takes its cluster's row and marks the faces of visible
leafs whose boxes are in the frustum; other world surfaces are skipped.
`r_novis 1` turns this off. A view outside the map (cluster -1) draws
everything.

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
