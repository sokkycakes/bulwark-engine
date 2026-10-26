# Materials and skies

`src/render/r_material.c` finds a material for a name in this order:

1. `<name>.mat` next to the texture (Nuclide's layout),
2. a shader of that name in any `scripts/*.shader`,
3. the plain image `<name>` with a default material made up for it.

## Syntax

Both kinds of file use the Quake 3 shader language as described in the
Q3Radiant shader manual: a body in braces, with stages in nested braces.
Nuclide's `.mat` files add these, learned by reading the files themselves:

- `program <name>`, choosing a GLSL program. Seen values: defaultwall,
  lightmapped, terrain, defaultskin, unlit, skybox, water. A `#DEFINE=value`
  suffix selects a variant and is ignored here.
- `diffusemap`, `normalmap`, `specularmap`, `fullbrightmap` images.
- `if <condition>` / `elif` / `else` / `endif` blocks. `$programs` counts as
  true (this renderer always has GLSL); any other name is read as a cvar.

Stage keys handled: `map`, `clampmap`, `animmap` (first frame only for now),
`$lightmap`, `$whiteimage`, `blendFunc` (add, blend, filter or two
GL_* factors), `rgbGen const ( r g b )` and `vertex`, `alphaGen const`,
`tcMod scroll` and `scale`, `alphaFunc`. Body keys: `surfaceparm nodraw`,
`sky`, `trans`, `skyparms`, `cull none/disable/twosided`, `polygonOffset`.

## Sky boxes

A sky surface (the shader's `surfaceparm sky`, or the BSP surface flag 4)
shows the map's sky box. The box comes from the shader's `skyparms` if it
names one, otherwise from the `sky` console command (Nuclide's client game
runs `sky "<name>"` from the map's `skyname` server key) or worldspawn's
`skyname`. The name is tried as a path, then under `env/`.

The six files are `<name>_rt`, `_lf`, `_up`, `_dn`, `_bk`, `_ft`. They are
uploaded as the cube faces +X, -X, +Y, -Y, +Z, -Z, and the sky shader looks
the cube up with (-y, z, -x) of the Quake-space view direction.

The up face is turned a quarter counter-clockwise and the down face a
quarter clockwise before upload. This was measured rather than taken from a
document: a script rendered both of Nuclide's sky sets (dune, smudge) through
the same mapping and compared pixels across every face edge. The side faces
meet with a mean error under 1 (of 255) as stored. The up face only meets
them when turned counter-clockwise (error 0.3 and 0.5, against 33 to 61 for
the other turns), and dune's down face only when turned clockwise (3.3,
against 14 to 18). Which way the whole box faces about the vertical axis
can't be told from the seams. It is a guess until checked against a map
whose lighting gives the sun's direction.
