# Voxel worlds

Bulwark can hold one mutable block grid per map next to (or instead of) the
BSP: a box of cells, each holding a block type and a rotation. It exists for
"Studio" style games where players build and play their own maps. The engine
knows no game's block list; the server QC defines the types at run time.

Code: `src/common/voxel.[ch]` (storage, collision, files, chunk encoding),
`src/server/sv_voxel.c` (QC builtins, files, replication),
`src/client/cl_main.c` (`parse_voxel`), `src/render/r_voxel.c` (meshes),
`src/server/sv_world.c` (traces). ISC licence.

## Model

| Thing | Value |
|---|---|
| Cell address | integer `x y z`, 0-based, `(0,0,0)` is the minimum corner |
| World size | 1..1024 cells per edge, at most 64M cells |
| Block size | any positive number of world units per cell edge (set at create) |
| Origin | world position of cell `(0,0,0)`'s minimum corner |
| Types | ids 1..4095, 0 is empty; each has a name used in files |
| Rotation | 0..3, quarter turns about +Z |
| Storage | 16x16x16 chunks, allocated on first write, freed when empty |

A type definition has a collision shape, Q3 content bits, an RGBA colour, a
texture name (or none for flat colour) and flags:

| Shape | Solid part of a unit cell at rotation 0 |
|---|---|
| `VXS_BOX` (1) | whole cell |
| `VXS_SLOPE` (2) | wedge rising towards +X |
| `VXS_RSLOPE` (3) | upside-down wedge |
| `VXS_SLAB_LOW` (4) / `VXS_SLAB_HIGH` (5) | lower / upper half |
| `VXS_THIN` (6) | panel at the +X side (fences, doors) |
| `VXS_FIELD` (7) | whole cell, contents only (water, ladders, volumes) |

Flags: `VXF_NODRAW` 1 (collides, not drawn), `VXF_FULLBRIGHT` 2.

## Collision

`Vox_Trace` sweeps a box through the cells whose contents meet the trace
mask, using each shape's planes. The server merges it with the BSP and entity
traces in `sv_world.c`: whichever hit is nearer wins, and the cell and type
hit are kept for `voxel_tracecell()` / `voxel_tracetype()` until the next
trace. Point contents include the cells' content bits, so water and ladder
cells behave as brush water and ladders.

## QC builtins

Bound by name (`= #0:voxel_set`). The header games include is
`voxel_api.h` (in the CSNZ workspace: `mod/csnz/src/shared/voxel_api.h`).

Server only:

| Builtin | Does |
|---|---|
| `float voxel_create(vector cells, float blocksize, vector origin)` | new empty world; replaces any old one |
| `void voxel_free(void)` | removes the world |
| `float voxel_define(float type, string name, float shape, float contents, vector rgb, float alpha, string texture, float flags)` | defines or redefines a type |
| `float voxel_set(vector cell, float type, float rot)` | one cell; 0 on a bad cell or undefined type |
| `float voxel_fill(vector mincell, vector maxcell, float type, float rot)` | a box of cells; returns how many changed |
| `float voxel_save(string name, string meta)` | writes `data/<name>`, keeps `<name>.bak` |
| `float voxel_load(string name)` | reads `data/<name>`; replaces the world only on success |
| `string voxel_meta(void)` | metadata block of the last load |
| `string voxel_error(void)` | why the last save or load failed |
| `float voxel_unknown(void)` | cells of the last load whose type name is not defined now |

Server and client (the client reads its mirror):

| Builtin | Does |
|---|---|
| `float voxel_get(vector cell)` / `float voxel_getrot(vector cell)` | cell contents |
| `vector voxel_cellforpoint(vector p)` / `vector voxel_cellorigin(vector cell)` | coordinate conversion (origin = minimum corner) |
| `vector voxel_size(void)` | cells per edge, `'0 0 0'` without a world |
| `float voxel_blocksize(void)`, `float voxel_count(void)` | block size, non-empty cells |
| `float voxel_typeforname(string)` / `string voxel_typename(float)` | -1 / "" when unknown |
| `vector voxel_tracecell(void)` / `float voxel_tracetype(void)` | what the last trace hit, `'-1 -1 -1'` / -1 if no cell |

File names given to save and load are relative to `data/`; names with `..`,
a leading `/`, `\` or `:` are refused ("bad file name"), the same sandbox QC
`fopen` writes use.

## File format (BWVX, version 1)

Little endian throughout.

```
"BWVX"            4 bytes
version           u32 (1)
size              3 x u32 (cells)
blocksize         f32
origin            3 x f32
ntypes            u32
  type id         u16      } ntypes times: the type table by name, so a file
  name length     u8       } survives renumbering; unknown names load as
  name            bytes    } empty cells and are counted (voxel_unknown)
nchunks           u32 (non-empty chunks)
  cx, cy, cz      3 x u16  } nchunks times
  length          u32      }
  runs            bytes    } chunk runs, below
metalen           u32
meta              bytes    opaque to the engine (the game's records)
crc32             u32      of everything before it
```

Chunk runs cover the 4096 cells of a chunk in x-fastest, then y, then z
order: `varuint count, u16 type, u8 rot` repeated until 4096 cells are
covered. A run of 0, a run past the end, a rotation above 3 or a type outside
the table makes the file invalid.

Loading checks the magic, version, sizes against the limits, every length
against the bytes left and the CRC before anything replaces the current
world; a damaged file gives "checksum mismatch (damaged file)" and leaves
the world as it was. Saving writes `<name>.tmp` first, then moves the old
file to `<name>.bak` and renames the new one into place, so a failed write
never replaces a good file.

## Network (svc_voxel)

The server is the only writer. Everything goes on the reliable stream, so
order is kept and nothing is lost.

| Sub-message | Payload | Sent |
|---|---|---|
| `VXN_INIT` | 3 varuint size, f32 blocksize, 3 f32 origin | world created, signon |
| `VXN_FREE` | - | world removed |
| `VXN_DEF` | varuint type, u8 shape, i32 contents, 4 u8 colour, u8 flags, string name, string texture | type defined, signon |
| `VXN_CHUNK` | 3 varuint chunk, varuint length, chunk runs | `voxel_fill`, `voxel_load`, signon |
| `VXN_SET` | 3 varuint cell, u16 type, u8 rot | `voxel_set` |
| `VXN_LIGHT` | varuint type, u8 light type, 3 u8 colour (0..15) | `voxel_setlight`, signon |

A client that connects later gets INIT, every DEF and every non-empty chunk
during signon (`SV_VoxelSignon`). The client checks every message: a world
size out of range, a chunk length past the message or a malformed run stream
marks the message bad, which ends parsing of it as for any malformed svc,
and never writes outside the mirror; a bad cell or definition is ignored
with a developer message.

## Rendering

`r_voxel.c` builds one vertex buffer per chunk from its cells, skipping
faces hidden by an opaque full neighbour, and rebuilds only chunks that
changed. Faces are grouped by image; translucent types (alpha below 255) are
drawn in the translucent pass after opaque ones. Textured types use the
named image on every face, others the flat colour, both shaded by the light
below.

## Light

`voxel_setlight(type, lighttype, '<r g b>')` gives a block type a light
type and an emitted colour (0..15 per channel). Light types: 0 none,
1 blocked, 2 slope-blocked, 3 point light, 4 sky light, 5 water-blocked,
6 slab-blocked. A type with no call has light type none, so light passes
through it (as in CSO, where a block with no light type is not an occluder).

The client keeps one light cell per voxel cell and rebuilds it when the
world changes:

1. Sky: every column is walked from the top down; open cells get sky level
   7 until the first blocked, slope-blocked, water-blocked or slab-blocked
   cell.
2. Spread: point-light cells (their colour) and sky cells are seeds. Light
   flows to the six neighbours, losing 1 per channel per step, keeping the
   per-channel maximum. It never enters blocked cells; slope-blocked light
   only flows into slope-blocked cells; slab-blocked light does not flow up
   or down.
3. Vertex: each face vertex averages the light of the cell in front of the
   face and of the side and corner cells around the vertex that are not
   blocked (the corner only when a side is open), clamped to 7. Each vertex
   gets two colours: night, darkened by `(7 - block) * k` per channel, and
   day, darkened by `(7 - max(block, sky)) * k`, with k = 0.6/7 (1/7 when
   `r_voxel_fulldark` is 1, the map's "remove default key light"). Corner
   darkening subtracts 0.1 per blocked side cell, 0.3 when both are
   blocked. The drawn colour is `night * (1 - d) + day * d`, with d the
   first value of `r_voxel_daylight` / 255 ("r g b", default
   "255 255 255", the map's day light).
4. Face shade: top 1.0, bottom 0.7, +y 0.925, -y 0.775, ±x 0.85, slope top
   0.97, slope underside 0.73, each tinted by `r_voxel_invlight` ("r g b",
   default "255 255 255", the map's back colour) as
   `1 + (v / 255 * 0.2 - 0.2) * w / 7`, where w is 0, 7, 2, 5, 3, 4, 1, 6
   for the faces in that order.

Models (entities and the view model) inside a voxel world are lit from the
light cell at their origin: with base 50 (0 when `r_voxel_fulldark` is 1)
and s = (256 - base) / 7, each channel is
`max(sky, c) * s * d + c * s * (1 - d) + base` out of 255, split 70/30
between ambient and a light from above (the split is ours).

A voxel world with no BSP world under it draws the sky named by `sky`
(`env/`, `gfx/env/` or `materials/skybox/` faces) as a background cube
before the voxels.

`r_voxel_light 0` turns this off (face shade only). Fullbright types are
never darkened. The behaviour is matched to Counter-Strike Online's Studio
lighting from its hw.dll (vertex colours, day mix, face shade and model
light); see the CSNZ studio trace notes.

## Void maps

`maps/<name>.vxmap` is a map with no BSP: the file is just an entity lump
(the same text as a BSP's entity lump). The server loads it when no
`maps/<name>.bsp` exists, so a game can build the whole level from voxels and
point entities (spawns, objectives, `info_target` markers its QC reads).

## Limits

| Limit | Value | Why |
|---|---|---|
| Types | 4096 | u16 type id; leaves room for game-side variants above its catalogue |
| Edge | 1024 cells | keeps chunk indices in u16 and memory bounded |
| Cells | 64M | about 200 MB worst case with every chunk allocated |
| Chunk message | 32 KB | a chunk of all-different cells fits; real chunks are far smaller |
