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

- `adddecal(shader, origin, up, side, rgb, alpha)`: Nuclide's decals and
  sprays pass `up` along the surface normal (its sign varies) scaled by one
  over the width, and `side` along the picture's downward axis scaled by
  one over the height. Texture t runs along side and s along side x up,
  from the centre. Both faces of the normal are accepted. The decal lasts
  until the next clearscene, but its clipped geometry is cached by
  placement so a decal redrawn each frame is clipped once. This reading
  makes Nuclide's sprays upright; it has not been checked against FTE.
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

## Sprays

- `setinfoblob key file` reads a file (at most 64 KiB) into a binary user
  info key; `setinfo key value` sets a text one. Blobs go to the server as
  clc_userblob once connected and to every client as svc_playerblob, and
  new clients get everyone's blobs with the player info. Keys starting with
  `_` stay on the server.
- `getplayerkeyblob`, `getlocaluserinfoblob` and `setlocaluserinfoblob`
  read and write them from QC memory.
- `r_uploadimage(name, w, h, data, size)` decodes an image file when given
  a size, otherwise takes raw RGBA. Materials name such images as
  `$rt:name`.
- `shaderforname(name, body)` makes the material from the body text when
  no material of that name exists yet.
- `getlight` answers in Quake light units: 255 is a texture at its own
  brightness.

## Source particle systems (.pcf)

`src/render/r_pcf.c` runs Valve's particle system files beside the r_part
effects. Everything here was read from the files themselves, which store
each function's name and its attributes by name, and from how the result
looks. No Source particle code was read. The rules marked "guess" are
choices that looked right on TF2C's `explosion.pcf` (TF2 explosions) and
may need changing when other content shows otherwise.

- Files: binary DMX version 2 (`<!-- dmx encoding binary 2 format pcf 1 -->`)
  only. A string table (int16 count), elements (int16 type index, inline
  name, 16-byte id), then each element's attributes (int16 name index, a
  type byte, the value; types above 14 are arrays with an int32 count).
  Element references are int32 indices, -1 for none; -2 (another file)
  is read past and treated as none.
- Names: `particleeffectnum("file.system")` runs system `system` from
  `particles/file.pcf` when no r_part effect has that name. A bare name is
  looked for in every .pcf read so far and the ones
  `particles/particles_manifest.txt` lists. `r_effect` spawns either kind.
  `r_pcfinfo [system]` lists files or what one system runs and skips;
  `r_pcfreload` forgets them. `r_drawpcf 0` hides them.
- Control point 0 is the spawn point. Its forward axis is the effect's
  direction (a wall's normal for Nuclide's projectiles), left and up
  follow, so "local coordinate system" speeds turn with it. No direction
  means the world's axes.
- Children start with the parent at its point after their `delay`.
- `emit_instantaneously` emits `num_to_emit` once `emission_start_time`
  has passed, up to `max_particles`; `initial_particles` come at once.
- Initializers set lifetime, position within a sphere (`distance_bias`
  scales each axis; a nonzero `distance_bias_absolute_value` keeps that
  axis on one side), velocity (radial speed plus a random local vector),
  rotation in degrees, rotation speed, yaw flip (the picture is mirrored),
  sheet sequence, colour (a random point between `color1` and `color2`),
  alpha (0-255), radius and trail length. The definition's own `radius`,
  `color`, `rotation` and `sequence_number` are the defaults.
- Every particle dies when its lifetime ends.
- `Alpha Fade and Decay`: times are fractions of the lifetime. Alpha rises
  from `start_alpha` to full between the fade-in times, falls from full to
  `end_alpha` between the fade-out times, and the particle dies when the
  fade-out ends. (Read from the core fireball, whose start and end alpha
  are both 0 yet which must show.)
- `Color Fade`, `Radius Scale`: from the initial colour or scale to the
  target between two lifetime fractions; `ease_in_and_out` smooths with
  3t^2 - 2t^3, `scale_bias` bends the curve as t^(log b / log 0.5).
- `Movement Basic`: gravity is a vector in units/s^2. Guess: `drag` removes
  that fraction of the velocity every thirtieth of a second.
- `Alpha Fade Out Random`, `Oscillate Scalar` (rotation, radius or alpha),
  `Rotation Basic`, `Rotation Spin Roll`, `random force` and `Collision via
  traces` (world only; bounce and slide) are run as their names and
  attributes say. Operator fade windows ("operator start fadein" and the
  like) are ignored; the shipped content leaves them at 0.
- Renderers: `render_animated_sprites` draws camera-facing quads whose half
  width is the radius; `render_sprite_trail` draws a quad from the particle
  back along its velocity, `speed * trail length` long, clamped to the
  renderer's min and max, growing over `length fade in time`;
  `render_screen_velocity_rotate` turns the sprites to their motion on
  screen. Orientation types other than 0 are drawn as type 0.
- Sprite sheets: the VTF's resource 0x10 holds sequences of frames, each
  with a duration and four sets of texture coordinates (the first set is
  used, without blending frames). Guess: with `animation rate` r a
  sequence plays r times a second; `use animation rate as FPS` makes r
  frames a second; `animation_fit_lifetime` plays it once over the life.
- Materials: the VMT's own blend ($additive adds with vertex alpha, others
  blend by alpha). SpriteCard's `$startfadesize`/`$endfadesize` fade a
  particle as its diameter grows from one fraction of the screen height to
  the other; `$overbrightfactor` scales its colour. A missing material
  becomes an additive soft dot, which stands in well for TF2's glows.
