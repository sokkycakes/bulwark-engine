# Particles and decals

Effects use the `r_part` script format described in
`fteqw-bulwark/specs/particles.txt`, which Nuclide's `particles/*.cfg` files
are written in. This page records how Bulwark reads the parts that document
leaves open. It was written from the format description and Nuclide's
scripts only.

## Names and loading

- `particleeffectnum("file.effect")` names effect `effect` in
  `particles/file.cfg`. The file is read the first time any of its effects is
  named. A name without a dot matches an effect of that name in any file
  read so far. An empty name is effect 0, meaning none.
- `r_part name { ... }` starts an effect; `r_part +name { ... }` adds another
  particle type to the effect named just before it. A later `r_part name`
  replaces an earlier one.
- Each property is one line: a keyword and its arguments. A `shader` line
  may be followed by a braced body, which is skipped.
- `r_partreload` forgets every script so they are read again. `r_partlist`
  lists the effects read so far. `r_effect file.effect [distance]` spawns an
  effect on the wall in front of the view, or at a distance.

## Spawning

- Server `pointparticles` and `trailparticles` are sent to the clients
  whose PVS holds the point (svc_pointparticles, svc_trailparticles). The
  client calls go straight to the renderer.
- `count min max`: particles per spawn, a random value between the two,
  times the count passed to pointparticles and `r_part_density`.
- `die max min`: lifetime, random between the two.
- `scale min max`: diameter in units. `scalefactor` 1 keeps a world size;
  values under 1 move towards a constant screen size, matching the world
  size at 256 units.
- Spawn modes: `box` (the default) places within `spawnorg` (horizontal,
  vertical) and adds velocity within `spawnvel`. `ball` places within a
  ball scaled by `spawnorg` and moves outwards by `spawnvel`. `circle` and
  `uniformcircle` place on a ring (arg 1 is the number of turns). Trails
  step along the line by `step`; with no step, `count` particles are spread
  along it. `spiral` turns once per arg-1 units; `tracer` alternates sides.
- Then `veladd` times the effect's direction is added to the velocity,
  `orgadd` times the direction to the origin, plus `orgbias`, `velbias`,
  `orgwrand`, `velwrand` and `randomvel` (horizontal, vertical).
- `assoc` spawns another effect at the same time. `emit` spawns an effect
  from each particle every `emitinterval` (+ random `emitintervalrand`)
  seconds after `emitstart`.
- `lightradius` adds a dynamic light that shrinks by `lightradiusfade` a
  second and lasts `lighttime`.
- `sound name vol attn pitch` (or `vol=`, `attn=`, `pitch=`) plays once per
  effect.

## Motion and looks

- Each frame: gravity pulls velocity down, `friction` (per axis) scales it
  by 1 - friction * time, colours change by `rgbdelta` per second (for
  `rgbdeltatime` seconds if set), alpha by `alphadelta`, size by
  `scaledelta`, rotation by its speed.
- `rampmode nearest` or `lerp` takes colour, alpha and size from the ramp by
  age; `delta` subtracts the ramp entry from colour and alpha each second.
- Particles with a `cliptype` (or a changed `clipbounce`) are traced against
  the world. `cliptype bounce`, or the effect's own name, reflects the
  velocity scaled by `clipbounce`; another effect name replaces the particle
  with that effect at the hit point, facing out of the wall.
- Types: `normal` faces the camera and rotates. `spark` and
  `texturedspark` are quads along the motion, as long as the speed times
  `stretchfactor` / 20 (a negative stretch is a fixed length). `beam` joins
  a trail's start and end, with the texture repeating every `beamtexstep`
  units and scrolling at `beamtexspeed`.
- `decal`, `cdecal` and `udecal` are clipped onto the world as decals.
- Blend modes: `add`/`adda` (src alpha, one), `addc` (src colour, one),
  `subtract` and `invmodc` (zero, 1 - src colour), `invmoda` (zero, 1 - src
  alpha), `blendcolour` (src colour, 1 - src colour), `blendalpha` (the
  default), `premul_add` (one, one), `premul_blend` (one, 1 - src alpha),
  `premul_subtract` (zero, 1 - src colour). A material file's own blend
  wins over the script's. In blends that ignore source alpha, colour is
  multiplied by alpha so particles still fade.
- `tcoords s1 t1 s2 t2 [scale] [count] [step]` picks a picture from the
  texture, one of `count` along a row. `atlas n first [last]` picks a cell
  of an n by n grid. When the texture is missing (the built-in names such as
  `ball` and `classicparticle`, or FTE's own particle font), a soft round dot
  is used whole.
- Not done yet: `model`, stains, rain, `flurry`, `viewspace`, the
  underwater switches, and light cubemaps and coronas.

## Decals

- `adddecal(shader, origin, up, side, rgb, alpha)`: up and side are the
  decal's axes, each scaled by one over its size (Nuclide passes
  `v_up / width`). Texture s runs along up and t along side, from the
  centre. It lasts until the next clearscene, but its clipped geometry is
  cached by placement so a decal redrawn each frame is clipped once.
- Effect decals face the effect's direction (or away from the nearest wall
  when it has none), are `scale` units across, turn by `rotation`, and fade
  out over their last second. `r_decal_max` (1024) are kept; the oldest
  goes first.
- Decals are clipped against the world's triangles that face the decal
  (within 0.1 of its normal; adddecal accepts both sides), within a box as
  deep as the decal is wide, and are drawn after the opaque world with a
  polygon offset.
- `getsurfacenearpoint`, `getsurfacetexture`, `getsurfacenormal`,
  `getsurfacenumpoints`, `getsurfacepoint`, `getsurfacepointattribute`,
  `getsurfacenumtriangles`, `getsurfacetriangle` and
  `getsurfaceclippedpoint` answer from the drawn world surfaces. A surface
  number is an index into the model's surfaces; the nearest is found by
  distance to its triangles.
