# Source BSP (VBSP)

`src/common/vbsp.c` reads the container, `src/server/bsp_source.c` builds
collision and entities from it, `src/server/bsp_props.c` turns props into
collision, and `src/render/r_srcworld.c` draws the world. Layouts come from
the public write-up of the format on the Valve Developer Community wiki
("BSP (Source)") and were checked against a real map (pl_circle_a9a, version
21) with the probes described at the end.

## Container

Header: "VBSP", int version (19 to 21), 64 lump entries of 16 bytes
(offset, length, version, fourCC), int map revision.

A lump whose data starts with "LZMA" is compressed: "LZMA", uint actual
size, uint compressed size, 5 bytes of LZMA properties, then the raw LZMA
stream (no 13-byte .lzma header). It is decoded with the public domain LZMA
SDK decoder in `src/thirdparty/lzma/`.

### Game lumps (lump 35)

int count, then per entry: 4-byte id, ushort flags, ushort version, int
offset (from the start of the file), int length. The id is stored
little-endian, so static props ("sprp") read as the bytes "prps".

When a game lump entry is compressed, its length field can't be trusted:
the data runs to the next entry's offset. Maps with compressed game lumps
end the directory with an empty entry whose offset marks the end.

### Static props (game lump "sprp")

int name count, names of 128 bytes; int leaf count, ushort leaves; int prop
count, then records. Version 10 records are 72 bytes:

| offset | field |
|---|---|
| 0 | origin float[3] |
| 12 | angles float[3] (pitch, yaw, roll) |
| 24 | model name index ushort |
| 26 | first leaf ushort, leaf count ushort |
| 30 | solid byte (0 none, 2 bounding box, 6 VPhysics) |

Record size is checked as (lump size - header) / count, so other versions
of the same layout prefix are read too.

### Entities (lump 0)

Text, as in Q1/Q3. Outputs are "target,input,parameter,delay,times";
version 21 maps (seen in pl_circle_a9a) use ESC (0x1b) instead of the
commas. The loader turns ESC into commas so game code sees one format.

## Records used

| lump | record | size |
|---|---|---|
| 1 planes | normal float[3], dist, type | 20 |
| 2 texdata | reflectivity float[3], name string id, width, height, view width, view height | 32 |
| 3 vertexes | float[3] | 12 |
| 4 visibility | cluster count, then (PVS offset, PAS offset) per cluster; rows run-length coded like Q1 | |
| 5 nodes | plane, children int[2], mins/maxs short[6], first face, face count, area, pad | 32 |
| 6 texinfo | texture vecs float[2][4], lightmap vecs float[2][4], flags, texdata | 72 |
| 7 faces | plane ushort, side, on node, first edge int, edge count short, texinfo, dispinfo, fog volume, styles[4], light offset, area, lightmap mins int[2], size int[2], original face, primitives, first primitive, smoothing groups | 56 |
| 8 lighting | RGBE: 4 bytes per luxel, colour = c * 2^e / 255 | 4 |
| 10 leaves (v1) | contents, cluster, area/flags, mins/maxs short[6], first face, face count, first brush, brush count, water id, pad | 32 |
| 12 edges | ushort[2] | 4 |
| 13 surfedges | int | 4 |
| 14 models | mins, maxs, origin float[3] each, head node, first face, face count | 48 |
| 16 leaffaces | ushort | 2 |
| 17 leafbrushes | ushort | 2 |
| 18 brushes | first side, side count, contents | 12 |
| 19 brushsides | plane ushort, texinfo short, dispinfo short, bevel short | 8 |
| 26 dispinfo | see below | 176 |
| 33 dispverts | vector float[3], distance, alpha | 20 |
| 43/44 texdata string data/table | names, int offsets | |

- Leaf children are -(leaf + 1) as in the Q1 node tree.
- Brushes aren't listed per model. A model's brushes are the brushes of the
  leaves under its head node; brush model brushes are stored in the model's
  own space (relative to its "origin" key).
- Bump-mapped faces (texinfo flag 0x800) store four lightmaps per style; the
  first is the flat one.

### Displacements

dispinfo: start position float[3], first vert, first tri, power, min tess
(int), smoothing angle, contents, map face ushort, then neighbour data. The
vertex grid is (2^power + 1) squared, built over the 4-corner base face
starting at the corner nearest to the start position. The triangles
alternate their diagonal.

When the high bit of min tess is set, its low bits are Hammer's flags; 0x4
means "no hull collision". Displacements with that flag are drawn but don't
collide. The others are turned into one 8-unit thick prism brush per
triangle (top face at the surface, bottom 8 units below). This is close
but not identical to Source, which collides with the triangle mesh itself.

## Contents and surface flags

Brush contents are mapped to Bulwark's Q3 values: solid, window and grate
are solid; water, slime, player clip, monster clip and ladder keep their
meaning; everything else (detail, translucent, origin, areaportal) is
dropped. Texinfo flags: sky and sky2d become Q3's sky and nodraw; nodraw
becomes nodraw. Collision needs bevel planes. VBSP's brush sides include
them (the bevel field marks them).

## Props

A Source map doesn't carry its props' collision; each model's `.phy` holds
it. Props are read from two places: the static prop lump, and entity props
(`prop_static`, `prop_dynamic`, `prop_dynamic_override`, `prop_physics`,
`prop_physics_override`, `prop_physics_multiplayer`, `prop_door_rotating`).
Non-physics props whose "solid" key is 0 are skipped; so are `prop_physics*`
with spawnflag 4 (debris). A prop with a "parentname", or tied to a brush entity by a
`phys_constraint` (attach1/attach2), is placed into that brush model's space
so its collision moves with it.

For each solid prop, in order:

1. **`.phy` (exact).** "VPHY" header (28 bytes, optional in old files), then
   per solid: size, "IVPS" compact surface; its ledge tree (28-byte nodes)
   leads to ledges (16 bytes) whose triangles (16 bytes) index the shared
   point list. Each ledge becomes a convex brush of its points. IVP space
   is metres with y down; Source = (x, z, -y) / 0.0254. *Not yet checked
   against a real file: no TF2 content was available.*
2. **Prop clip table (approximate).** `maps/<map>.propclip` first, then
   `scripts/propclip.txt`. One model per line:
   `"models/x.mdl" box minx miny minz maxx maxy maxz` (several `box` lines
   for one model add up), or `"models/x.mdl" none` for props that shouldn't
   collide. Boxes are in model space and rotate with the prop.
3. **The .mdl's hull box (approximate).** "IDST" header, hull mins/maxs at
   bytes 104 and 116.
4. **Missing.** Logged every time.

Every prop's hull is one brush per convex piece, flagged as added so
`r_showclip 1` draws them (orange); `r_showclip 2` draws every brush. The
load prints `prop collision: N exact, M approximate, K listed as none, J
missing`, and `developer 1` lists each prop and its source.

## How this was checked

- `probe.c`: loads pl_circle_a9a through `BSP_LoadSource` and traces down
  from every spawn point. All 12 land on the floor (z 18 and -312).
- Model and brush counts match the lumps: 37 models, 508 brushes in the
  file, plus 18 hull brushes.
- Screenshots through `r_debugview` and `r_showclip` show the prop boxes in
  the baked prop shadows of the lightmap.
- `tests/bsptest` still passes (21 checks).
