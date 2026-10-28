# Studio models (IDST v10) and CSO `.seq` files

Read by `src/common/studio.c`. All values are little-endian. Offsets are
from the start of the file unless a field says otherwise.

## How this was established

The general shape of the format (header of counts and offsets, fixed-size
tables, per-bone animation channels with run-length coded values) is from
public descriptions of the GoldSrc model format. No engine or SDK source was
read for this file. Each size and field position below was then checked
against real content: Counter-Strike Online models taken from the game's
archives for analysis (never committed here). These are five IDST v10
models and nine `.seq` files from four archives:

- `scvip-shop`, `rb-shop` (cstrike)
- `henry-shop` (dstrike)
- `zombi_host` (estrike)
- `y20s1leechang-shop` (fstrike)

A layout counts as confirmed when every table fits inside the file, every
index points at an existing entry, and the animation runs of every
sequence tile their data region with no gaps. Run `tools/mdlcheck` to
repeat the check. Where a field's meaning is still a guess, it says so.

## Header (244 bytes)

| Offset | Type | Field |
| --- | --- | --- |
| 0 | char[4] | `IDST` |
| 4 | i32 | version, 10 (v20 and v21 files exist in CSO but are encrypted; not handled) |
| 8 | char[64] | name |
| 72 | i32 | file length; must equal the file size |
| 76 | vec3 ×5 | eye position, mins, maxs, bbmins, bbmaxs |
| 136 | i32 | flags |
| 140 | i32, i32 | bone count, offset |
| 148 | i32, i32 | bone controller count, offset |
| 156 | i32, i32 | hitbox count, offset |
| 164 | i32, i32 | sequence count, offset |
| 172 | i32, i32 | sequence group count, offset |
| 180 | i32, i32, i32 | texture count, offset, texture data offset |
| 192 | i32, i32, i32 | skin refs per family, skin families, skin table offset |
| 204 | i32, i32 | body part count, offset |
| 212 | i32, i32 | attachment count, offset |
| 220 | i32 ×4 | sound table/index/groups/group index (unused here) |
| 236 | i32, i32 | transition count, offset |

## Tables

| Record | Size | Fields |
| --- | --- | --- |
| bone | 112 | name[32], parent i32 (-1 root, otherwise an earlier bone), flags, controller i32[6], value f32[6] (x y z position, x y z euler rotation in radians), scale f32[6] |
| bone controller | 24 | bone, type, start f32, end f32, rest, index |
| hitbox | 32 | bone, group, mins vec3, maxs vec3 |
| sequence | 176 | see below |
| event | 76 | frame, event, type, options char[64] |
| sequence group | 104 | label[32], name[64], two unused i32 |
| texture | 80 | name[64], flags, width, height, pixel offset; the pixels (width × height palette indices) are followed by a 256-entry RGB palette |
| skin table | 2 per entry | i16 texture index, families × refs |
| body part | 76 | name[64], model count, base, model offset |
| sub-model | 112 | name[64], type, bounding radius f32, mesh count, mesh offset, vertex count, vertex-bone offset (u8 each), vertex offset (vec3 each), normal count, normal-bone offset, normal offset, group count, group offset |
| mesh | 20 | triangle count, triangle command offset, skin ref, normal count, normal offset |
| attachment | 88 | name[32], type, bone, origin vec3, vectors vec3[3] |

Sequence descriptor (176 bytes):

| Offset | Field |
| --- | --- |
| 0 | label char[32] |
| 32 | fps f32 |
| 36 | flags (bit 0 set on looping CSO shop sequences; meaning inferred) |
| 40, 44 | activity, activity weight |
| 48, 52 | event count, event offset |
| 56 | frame count |
| 60, 64 | pivot count, pivot offset |
| 68, 72 | motion type, motion bone |
| 76 | linear movement vec3 |
| 88, 92 | automove position/angle offsets |
| 96, 108 | mins, maxs |
| 120, 124 | blend count, animation offset |
| 128 | blend type i32[2], start f32[2], end f32[2], parent |
| 156 | sequence group |
| 160–172 | entry node, exit node, node flags, next sequence |

Triangle commands are a stream of i16. A count n > 0 starts a strip of n
vertices and n < 0 starts a fan of −n. Each vertex is four i16: vertex
index, normal index, s and t (texels). A count of 0 ends the list.

## Animation data

At a sequence's animation offset there are `blends × bones` blocks of six
u16, one per channel (x, y, z, rot x, rot y, rot z). Each value is an
offset from the start of that block, and 0 means the channel has no
animation. A channel's data is a list of runs. A run is a byte `valid`, a
byte `total`, and then `valid` i16 values covering `total` frames. For
frames at or past `valid`, the last value repeats. The next run follows the
values. The channel value is `bone.value[k] + raw × bone.scale[k]`.

Confirmed: in all five models, the runs of every channel of every sequence
cover exactly `numframes`. The rule `total ≠ 0`, `valid ≤ total` holds
everywhere.

Rotation is converted to a quaternion as rotations about x, then y, then z
(standard Euler composition). The decoded pose has not yet been compared
against the original client, so treat the axis order as inferred until a
rendering check is done.

Bone controllers and motion extraction (removing linear movement) are not
applied yet.

## CSO `.seq` files (appended sequences)

CSO ships `name1.seq`, `name2.seq`, … next to `name.mdl`. They are not
standard sequence groups. The probed models have one group ("default"),
every sequence is in group 0, and no `.seq` has an `IDSQ` header.

| Offset | Field |
| --- | --- |
| 0 | i32 sequence count n |
| 4 | i32 descriptor table offset `seqindex` |
| 8 | animation data, same encoding as above |
| `seqindex` | n × 176-byte sequence descriptors; `seqindex + 176 n` equals the file size |

Confirmed: the size rule holds for all 2,727 `.seq` entries in the four
archives, using decoded sizes from the archive directories and the first
16 decoded bytes of each file.

Each descriptor's animation offset is not a file offset. Subtracting the
first descriptor's value and adding 8 gives the file offset. Under that
relocation, the sequences' runs tile the data region back to back with no
gaps. This was confirmed for all nine `.seq` files.

Between the end of the animation data and `seqindex` there is a copy of the
combined model's tables:

- 0 to 3 alignment bytes
- the full sequence descriptor table: the model's sequences, then those of
  every lower-numbered `.seq`, then this file's own
- the combined event table (`zombi_host`: 34 events, byte-identical to
  the model's)
- one 104-byte sequence group

In that copy, the model's descriptors match the `.mdl` in label and all
other fields except the event, pivot and (in `rb-shop`) animation offsets.
That makes the `.mdl`/`.seq` link structural, not only by name. The copies
are not always consistent across files. `rb-shop2.seq` lists `rb-shop1.seq`'s
two rocket sequences with 101 frames and different bounds, while
`rb-shop1.seq`'s own descriptors say 91, and 91 is what its animation data
covers. The loader uses each file's own trailing descriptors and ignores
this region.

Inferred, not yet observed in a running client:

- appended sequences take indices after the model's own, in `.seq` number
  order
- the 35 `dummy` sequences in `zombi_host1.seq` reserve indices

No probed `.seq` descriptor has events. The loader rejects one that does,
because where its event offset points is unknown.

## Drawing

The renderer (`load_studio` in `src/render/r_model.c`) draws the first
sub-model of every body part:

- Vertices and normals are bone-local. They're placed by each bone's rest
  value (position, then rotation from the euler angles in the same order
  as animation frames) chained through the parents, and skinned to that one
  bone.
- Triangle commands: a positive count is a strip, a negative one a fan,
  zero ends the list; each vertex is four int16s (vertex, normal, s, t), with
  s and t in texture pixels. The triangles are turned to face the way most
  of them agree with their normals, so the file's winding convention
  doesn't have to be assumed.
- Textures are 8-bit with a 768-byte palette after the pixels. Flag 0x40
  (masked) makes index 255 see-through, 0x20 draws additively and 0x04
  unlit. Flag 0x02 (chrome) is drawn as a plain texture. A picture or
  `.mat` named `<model>_<texture name without extension>` replaces one.
- A model without textures reads them, and the skin table, from
  `nameT.mdl` (or `namet.mdl`) next to it.
- The skin table picks the material per family; an entity's `skin`
  chooses the family.
- `Skel_LoadModel` gives the model a skeleton: the bones, each sequence's
  first blend as an animation named by its label, with its fps and
  looping flag. An entity's `frame` is the sequence, as for IQM models,
  and `frame1time` the time into it. Sequences kept in other files stand
  in the rest pose.

`tools/mkstudio_test.py` writes a two-bone model with a bending sequence,
two skin families, a masked texture and a companion-file variant; these
were checked in `modelviewer`. No game model was available here, so the
texture row order and the face test haven't been compared against a
shipped model yet.

## Not covered

- Encrypted v20/v21 models
- External `IDSQ` sequence groups
- Blending between sequences, blend axes, other body part sub-models
- Bone controllers and hitbox tracing
