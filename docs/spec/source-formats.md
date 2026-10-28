# Source file formats (the `source` plugin)

`plugins/source/` builds `bwplug_source.so`. Every `bwplug_*.so` next to
the engine loads at startup, so a build that ships the file reads Source
textures, materials and models with nothing to set up. It uses the file
format hooks in plugin interface version 2 (`src/common/bwplug.h`):

| File | Becomes | Engine side |
|---|---|---|
| `.vtf` | RGBA pixels | `R_LoadPixels` and `R_ImageExists` try plugin extensions after their own |
| `.vmt` | a `bwplug_material_t` | `R_FindMaterial` after `.mat` files and shader scripts, before a bare image |
| `.mdl` (version 44 to 49) | an IQM version 2 file in memory | `R_LoadModel` for files it can't read itself |

Nothing here came from Valve's code. The layouts were written from the
public descriptions of the formats (the Valve Developer Community pages
on VTF, MDL, VVD and VTX), then checked against real files: two VTFs (a
v7.2 BGR888 from Galaco/vtf's test data, public domain, and a v7.6 DXT1
from ChaosInitiative/vtex2's tests, MIT) and two models (barrel01, MDL
v46, from icewind1991/vmdl's test data, and candles, MDL v49, from
LordVonAdel/source-mdl's tests, both MIT). The test files are not in this
repository. Their parsers weren't read.

## VTF

Header, little-endian: `"VTF\0"`, uint32 major (7) and minor version,
uint32 header size, uint16 width and height, uint32 flags, uint16 frame
count and first frame, 4 bytes, float reflectivity[3], 4 bytes, float bump
scale, int32 high-resolution format (offset 52), uint8 mipmap count (56),
int32 low-resolution format (57), uint8 low-resolution width and height
(61, 62), and from 7.2 uint16 depth (63). From 7.3 there is a resource
count at 68 and the resource list at 80: three tag bytes, a flags byte and
a uint32 offset each. Tag `30 00 00` is the high-resolution image.

Before 7.3 the low-resolution thumbnail follows the header and the image
follows that. The image data runs from the smallest mipmap to the largest,
and inside each mipmap through the frames, then the faces (6 for an
environment map, flag 0x4000; 7 before 7.5 unless the first frame is
0xffff), then the depth slices. Only the largest mipmap's first frame,
face and slice is decoded.

Formats by number: 0 RGBA8888, 1 ABGR8888, 2 RGB888, 3 BGR888, 4 RGB565,
5 I8, 6 IA88, 7 P8 (not supported), 8 A8, 9 RGB888 bluescreen, 10 BGR888
bluescreen, 11 ARGB8888, 12 BGRA8888, 13 DXT1, 14 DXT3, 15 DXT5, 16
BGRX8888, 17 BGR565, 18 BGRX5551, 19 BGRA4444, 20 DXT1 with one-bit
alpha, 21 BGRA5551, 22 UV88, 23 UVWQ8888, 24 RGBA16161616F, 25
RGBA16161616, 26 UVLX8888. The letters give the byte order from the lowest
address, and for the 16-bit formats from the lowest bit. That reading of
RGB565 against BGR565 and of ARGB8888 is a guess: none of the test files
use them. Half-float images are clipped at 1.

The DXT decoder was checked against Pillow's DDS decoder: random DXT1,
DXT3 and DXT5 blocks and the v7.6 file's DXT1 image differ by at most 1
in any channel (rounding of the interpolated colours).

Strata Source's v7.6 adds an `AXC` resource whose second word is a
compression level. Level 0 (the test file) is stored plain and works;
compressed files are refused with a message.

## VMT

KeyValues text: the shader name, then a braced block of `"key" "value"`
pairs. `//` comments and `[$condition]` suffixes are skipped. Nested
blocks named `>=dx90` and the like hold the settings for current hardware
and are merged in; other nested blocks (`Proxies`, `<dx90` fallbacks)
are skipped. A `patch` material loads its `include` and then applies its
`replace` and `insert` blocks.

| Key or shader | Effect |
|---|---|
| `$basetexture` | the diffuse image, under `materials/` |
| `$bumpmap`, `$normalmap` | the normal map |
| `$translucent`, `$additive`, `$alphatest`, `$nocull`, `$alpha` | blending, alpha test, two sides |
| `UnlitGeneric`, `UnlitTwoTexture`, `Sprite` | unlit |
| `Water` | the water program; its normal map stands in for a base texture |
| `Refract` | translucent |
| `%compilenodraw`, `%compileclip` and the other tool flags, `tools/tools*` names | not drawn |
| `%compilesky`, `%compile2dsky`, `tools/toolsskybox` | sky |

Paths are lower-cased with `\` turned into `/` (doubled separators count
once), and a `.vtf` ending is dropped.

## MDL, VVD and VTX

The model is drawn in its reference pose, which is what static props use.
Animation isn't read yet: characters stand in their bind pose.

**MDL** header offsets used: `"IDST"`, version at 4 (44 to 49 here;
GoldSrc's 10 is the engine's own), hull box at 104 and 116, texture count
and offset at 204 and 208, material folder count and offset at 212 and
216, skin reference count at 220, skin family count at 224, skin table
offset at 228, body part count and offset at 232 and 236.

- Texture entries are 64 bytes; the first int is the name's offset from
  the entry.
- Material folders are an array of file offsets to strings. A texture's
  material is the first `materials/<folder><name>.vmt` that exists.
- The skin table is int16 `[families][references]`; family 0 is used. A
  mesh's material number is a skin reference.
- Body parts are 16 bytes (name offset, model count, base, model offset
  from the body part). The first model of each part is drawn.
- Models are 148 bytes: name[64], type, bounding radius, mesh count (72),
  mesh offset from the model (76), vertex count, vertex byte offset into
  the VVD's vertices (84).
- Meshes are 116 bytes: material (0), model offset, vertex count, vertex
  offset within the model (12).

**VVD**: `"IDSV"`, version 4, checksum, LOD count, LOD vertex counts[8]
(LOD 0 at 16), fixup count (48), fixup offset (52), vertex data offset
(56), tangent offset (60). Vertices are 48 bytes: bone weights 16,
position 12, normal 12, texture coordinates 8. With fixups (lod, first
vertex, count, each 4 bytes), LOD 0's vertex list is every fixup with
lod >= 0 in order.

**VTX** (`.dx90.vtx`, else `.dx80.vtx`, `.sw.vtx`, `.vtx`): version 7,
then body part count and offset at 28 and 32. Body parts (8 bytes: model
count, offset), models (8: LOD count, offset), LODs (12: mesh count,
offset, switch distance), meshes (9: strip group count, offset, flags).
Strip groups are 25 bytes (vertex count and offset, index count and
offset, strip count and offset, flags), or 33 in later games; the size is
chosen by which one keeps every group's arrays inside the file. VTX
vertices are 9 bytes with the mesh-relative vertex number at 4. Indices
are uint16 into the strip group's vertices. Strips are 27 bytes (35):
index count and offset, vertex count and offset, bone count, flags at 18
(value 2 marks a triangle strip, otherwise it's a list).

A triangle's vertex is the model's vertex offset / 48 + the mesh's vertex
offset + the VTX vertex's number. Triangles are written to the IQM in the
order the VTX gives them: in both test models every triangle's winding
runs against its vertex normals, which is IQM's own order (the IQM loader
turns them for GL), and both draw right side out in `modelviewer`.
