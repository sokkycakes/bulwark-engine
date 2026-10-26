# FTEW worlds

FTEW v1 is a compiled world format: a replacement for a BSP, made from a
Godot scene by the Stiletto exporter in sokky's Nuclide fork
(`worldsrc/addons/stiletto_tools/exporter.gd` on the `feat/juliet-hero-kit`
branch). Its spec is `worldsrc/FORMAT.md` in that branch. Bulwark's reader
(`src/common/ftew.c`) is written from that spec. The test world
`tests/ftew/maps/editor_lab.ftew` comes from the same branch.

## Layout

Everything is little-endian. Integers are u32 and floats are float32,
packed with no padding.

Header, 96 bytes:

| Offset | Meaning |
|---|---|
| 0 | `FTEW` |
| 4 | version, 1 |
| 8 | total file length, exact |
| 12 | model count, including world model 0 |
| 16 | five directory records: tag[4], offset, byte length, record count |

The sections come in this order: `MATL`, `MESH`, `INST`, `COLL`, `ENTS`.
They sit back to back from byte 96 to the end of the file, with no gaps.

| Section | Records |
|---|---|
| `MATL` | u32 length, then a path with no terminator. Paths are printable ASCII, game-relative, under 64 bytes, with no `..`, quotes, backslashes, `:` or `;`. Each names a material (`.mat`) |
| `MESH` | material index, vertex count, index count, then vertices (9 floats: xyz, st, rgba) and u32 indices. Front faces are clockwise. No normals |
| `INST` | 60 bytes each: mesh, model, flags (bit 0 visible, bit 1 collidable, at least one set), 3 basis columns, then the translation |
| `COLL` | model, contents (must be 1, solid), mins[3], maxs[3], plane count (4 to 64), planes (normal xyz, dist). The inside is where `dot(n, p) <= dist` |
| `ENTS` | one NUL-terminated entity lump, record count 1, worldspawn first |

Positions are already in Quake units. The exporter converts Godot's Y-up
metres with `(x, y, z) -> (x, -z, y) * 32`. Entity models (`*N`) use
entity-local coordinates.

The reader rejects files that break these limits: 128 MiB per file, 4 MiB of
entity text, 4096 materials and meshes, 65535 vertices per mesh, 16384
instances and colliders, 1024 models, and 50000 triangles after instancing.
It also rejects non-finite floats or floats above 1e8, singular instance
transforms, collider planes whose normals are not unit length, and empty
collider bounds.

## In Bulwark

`BSP_Load` recognises the `FTEW` magic, so any place that takes a map takes an
FTEW world. That includes `map <name>`, which tries `maps/<name>.scene`, then
`.bsp`, then `.ftew`, and a scene's `MapInstance` naming `foo.ftew`.

**Collision** (`src/server/bspfile.c`). Convex colliders are already
brushes. Each collidable triangle becomes a thin brush made of:

- its face,
- a back face 2 units behind it,
- three edge planes,
- six axial bevels, so box sweeps don't catch on corners.

Brushes are grouped by model, so traces run through the same Q3 brush code
as BSP maps. This gives a surface shell, as the spec describes, and not a
solid volume.

**Drawing** (`src/render/r_world.c`). Instances are expanded into one surface
per model per material. Each triangle gets a flat normal, and the vertex RGBA
lights it, because there is no lightmap. A mirrored instance (negative
determinant) has its triangle order flipped.

FTEW has no sky faces, so a box around the world carries the sky. The
`skyname` the box uses is looked up as a cubemap. Stiletto writes panoramas
(`env/fteworld/<hash>.png`), which Bulwark can't draw yet, so the sky falls
back to a flat colour.

**Checked:**

- `editor_lab.ftew` and the larger `neden_1.ftew` (176 meshes, 44000
  collision brushes) both load in the dedicated server and run Nuclide's
  progs.
- The client draws them with their textures.
- In the lab, the player stands on the floor at the spawn point.
- `tests/ftew` covers parsing, rejection of every truncated length and of
  corrupt files, floor and underside traces, and the entity-local trigger
  collider.

## Not done yet

- Panorama skies.
- Smooth normals.
- A faster trace structure than a linear brush list. FTE builds a BIH. Big
  FTEW worlds trace slower than BSPs of the same size.
