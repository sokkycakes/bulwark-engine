# Models

Sources: the public IQM v2 specification (Lee Salzman, MIT; the header and
struct layout), the doc comments of the `FTE_CSQC_SKELETONOBJECTS` and
`DP_QC_GETTAGINFO` builtins in Nuclide's `fteextensions.qc`, and how
Nuclide's QC calls them. Nothing here comes from another engine's source.

## Framegroups

`<model>.framegroups` names animations over a model's raw frames, one per
line: `first count fps loop // name`. When the file exists it replaces the
model's own list (MDL frame groups, IQM animations). `frame` on an entity
indexes this list.

## IQM

The header is the 16-byte magic `INTERQUAKEMODEL\0` and 27 little-endian
words: version (2), filesize, flags, num/ofs text, num/ofs meshes,
num_vertexarrays, num_vertexes, ofs_vertexarrays, num/ofs triangles,
ofs_adjacency, num/ofs joints, num/ofs poses, num/ofs anims, num_frames,
num_framechannels, ofs_frames, ofs_bounds, num/ofs comment, num/ofs
extensions. Nuclide's `.vvm` files use the same layout and magic.

- Vertex arrays used: position, texcoord, normal (floats), colour, blend
  indexes (4 ubytes) and blend weights (4 ubytes or floats).
- Triangles are wound the opposite way to GL's default front face.
- A mesh's material name without a `/` is looked up next to the model.
- Joints: name, parent (earlier joints only; anything else is treated as a
  root), translate, rotate (quaternion xyzw), scale. A vertex is
  transformed as rotate(v * scale) + translate, in the parent's space.
- Poses: per joint, a channel mask and ten channel offsets and scales
  (translate 3, rotate 4, scale 3). Each frame is a run of ushorts, one per
  masked channel, joint by joint; a channel is offset + value * scale when
  masked, otherwise the offset.
- Anims: name, first frame, frame count, framerate, flags (bit 0 loops).

A model with joints is skinned on the CPU each time it is drawn: the pose's
absolute bone matrices times the inverse bind pose (from the joints), up to
four weighted joints per vertex.

## Skeletons and tags

Bone matrices are 3x4: the bone's forward, left and up axes and its origin.
QC sees forward, right (the negated left axis) and up, as `makevectors`
gives them. Bones are numbered from 1 in QC; 0 is the entity itself.

An entity's pose: animation `frame` at `frame1time` seconds and `frame2` at
`frame2time`, blended by `lerpfrac` (0 is all `frame`). Within an animation
the two nearest raw frames are blended (quaternions the short way round);
a looping animation wraps, others hold their last frame. Animation numbers
outside the list give the bind pose.

- `skel_create(modelindex, useabs)` makes an object holding a bind pose,
  relative to each bone's parent, or to the entity when `useabs` is set.
  0 if the model has no skeleton.
- `skel_build(skel, ent, modelindex, retainfrac, firstbone, lastbone,
  addfrac)` sets each bone in the range (0, 0 for all) to bone * retainfrac
  + the entity's pose * addfrac. Left out, addfrac is 1: Nuclide's player
  passes retainfrac 0.001 and no addfrac to replace the torso bones, which
  only works that way.
- `skel_get_bonerel` and `skel_get_boneabs` return the origin and set
  `v_forward`, `v_right`, `v_up`. `skel_set_bone` sets a bone relative to
  its parent; `skel_premul_bone(s)` multiply on the left (in the parent's
  space), `skel_postmul_bone` on the right (in the bone's own space). When
  the orientation is left out the `v_` globals are used; with only a
  forward vector, right and up are made level with it.
- `skel_delete` takes effect once the frame has moved on, so an object
  deleted in a predraw is still drawn.
- `gettagindex(ent, name)` finds a bone of the entity's model.
  `gettaginfo(ent, tag)` gives its world position and orientation, from the
  entity's `skeletonindex` object when it has one, otherwise from its frame
  fields; a model's pitch is inverted as when it is drawn. A tag past the
  bones (Nuclide adds Half-Life attachment numbers to the bone count) gives
  the entity's own placement for now.
- An entity drawn with `skeletonindex` set uses that object's bones.

Checked by `tests/qc/vmtest.qc` against `tests/data/skeltest.iqm`, which
`tools/mkiqm_test.py` writes from the specification, and in the client by
spawning a viewer-facing variant with Nuclide's `sv propSpawn` and a
`.framegroups` file selecting the bent frame: the tip half stood up as
expected.

Not done: ragdolls (`skel_ragupdate`), `skel_set_bone_world`,
`skel_build_ptr`, `frame3`/`frame4` blending, and model events for IQM.

## Studio model frames and events

On GoldSrc studio models (format in [studio-mdl.md](studio-mdl.md)) a
"frame" in QC is a sequence number, as Nuclide's Half-Life code expects.
Both VMs answer the frame queries from the parsed model when the model is
not IQM: `frameforname` (sequence label), `frametoname`, `modelframecount`
(sequence count) and `frameduration`. A sequence lasts
`(frames - 1) / fps` seconds, or `1 / fps` for a one-frame sequence: a
sequence whose last frame is shown at its end time has `frames - 1`
intervals. `frameforaction(model, activity)` picks among the sequences
whose activity number matches, weighted by their activity weight, and
gives -1 if none does.

The model event builtins follow their fteextensions doc comments. An
event's time is its frame divided by the sequence's fps.

- `processmodelevents(model, frame, basetime, targettime, callback)` calls
  the callback for each event with a time in `(basetime, targettime]`, in
  time order, and writes `targettime` back into `basetime` (the argument is
  `__inout`, so the caller copies the parameter slot back after the call:
  the write has to happen after the callbacks, which reuse the slots).
  A window starting at 0 or earlier also takes events on frame 0, so an
  event on the first frame fires once when the animation starts. A target
  before the base means the animation was restarted: the window becomes
  `[0, target]`.
- Sequences with the looping flag (bit 0 of the sequence flags) repeat
  their events every duration, so a walk cycle's footstep events keep
  firing; after a long gap only the last four laps are replayed.
- `self` is put back after each callback.
- `getnextmodelevent` returns the first event of the same window (writing
  its time into `basetime`), and `getmodeleventidx` reads an event by
  index.

Checked by running Rad-Therapy's monsters on Bulwark: zombie and headcrab
sequences report the durations their schedules wait for, and the events in
the attack and death sequences reach the monsters' event handler at the
frames the model lists (`? event 2 in "attack1"` is logged by Nuclide for
the codes it does not handle, at 1.27 s into the sequence).

## Sprites

GoldSrc sprites (`IDSP` version 2) load as models of their own, so an entity
whose model is a `.spr` draws a camera-facing quad. Every frame is a
material named the way `spriteframe` names it (`model:N`, frame groups
counting each member), and the entity's `frame` picks one. The quad spans
the frame's origin offset and size times `scale`, takes `colormod` and
`alpha`, and is drawn in the translucent pass without writing depth.
Texture format 1, `EF_ADDITIVE` (32) or `RF_ADDITIVE` (8) add it onto the
scene; format 3 is alpha-tested. Orientation types follow the header:
0 and 1 stay upright, 2 faces the view, 3 uses the entity's angles and 4
faces the view turned by the roll angle. A sprite with `RF_DEPTHHACK` or
`RF_VIEWMODEL` uses the view model's projection and depth range, which is
how muzzle flashes on view models stay in front of the gun.

`r_viewmodel_mirror` also mirrors the tags `gettaginfo` returns for view
models, so attachments such as the muzzle stay on the drawn barrel.

The render flag `RF_MIRROR` (65536, Bulwark's own) mirrors one entity left
to right, model and tags alike. On a view model it flips the
`r_viewmodel_mirror` result, so a model built for the other hand (Counter-
Strike Online's are left-handed) is drawn the way the cvar asks.

The render flag `RF_OUTLINE` (131072, Bulwark's own) draws a teammate outline
like Counter-Strike Online's (`cl_outline_tdm`) around an alias or skeletal
model, after the scene's entities and through walls. Its colour is the
entity's `.outlinecolor` (0 to 1; unset is CSO's green, 76 208 113). As
CSO's hw.dll does it, the model goes into the stencil, then is drawn again as
a wireframe (`glPolygonMode` lines) where the stencil is clear, with depth
ALWAYS, so the outer half of the lines shows around the model, through
walls. Lines are 7 - d²/10000 pixels wide (at least 4.5), alpha is 0.2 +
d²/100000 (at most 0.8), and per vertex alpha is at most (z - (origin z +
mins z)) / (maxs z - mins z), fading it towards the bottom of the box. View
models cover it. Where GL has no polygon mode or lines that wide (WebGL),
the model is pushed out along its normals by half the line width instead.
`r_outline 0` turns outlines off and `r_outline_width` scales them (1 is
CSO's).
