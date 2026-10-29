# Client game (csprogs) behaviour

What Nuclide's client game expects from the engine, beyond the builtin
declarations. Each item was found by running Nuclide's csprogs and reading
its QC source.

## Frame order

`CSQC_UpdateView(width, height, notmenu)` runs once per frame between the
renderer's begin and end. Width and height are virtual 2D pixels (the
framebuffer divided by `scr_scale`, which by default keeps at least 480
lines). `notmenu` is 1 while no menu progs is drawing over the game. Nuclide
renders 3D with `renderscene`, then draws its HUD; the console draws over
everything in `R_EndFrame`.

Before that call, every client game entity gets a physics frame
(`csqc_physics` in cl_csqc.c), using the server's toss, bounce, fly and
noclip movement through `W_RunEntityPhysics`:

- Its `think` runs when `nextthink` has come, and `customphysics`, when set,
  replaces the movement, as that field's doc comment in fteextensions.qc
  describes.
- Entities the game spawned itself (`entnum` 0) then move by their
  movetype, with gravity from the server's `sv_gravity` serverinfo key.
- Entities that mirror a server entity (`entnum` above 0) only think. Their
  position comes from snapshots and the engine's interpolation (below).
- The step is the client time since the last frame, at most 0.1 s.

Found from Nuclide's client QC: `BreakModel_SpawnID` (propdata.qc) spawns
gibs and debris as client-only MOVETYPE_BOUNCE entities that keep a think
scheduled and remove themselves with `ScheduleThink(Disappear, fadetime)`.
Without this frame they hung in the air where they spawned and never went
away (until the 33-entry pool reused them). Other client-only entities
that time out the same way, such as `EV_TraceDebug`'s, depend on it too.

## Drawing between snapshots

A server entity's fields only change when a snapshot carries it, about 72
times a second. Nuclide sends monsters' `frame1time` every tick and the
client game never advances it (only the server's `ncEntity::ParentUpdate`
adds `frametime`), so poses and positions stood still between updates. A
probe against FTE (2026-10-29) showed FTE draws them the same way. Bulwark
now goes further, as GoldSrc does:

- With `cl_lerpentities 1` (the default) an entity that mirrors a server
  entity is drawn `cl_interp` seconds (0.03) behind the newest snapshot,
  between the two snapshots around that moment: origin, angles, and
  `frame1time` while `frame` stays the same. The server's lag compensation
  already rewinds by the same `cl_interp`. Only the drawn copy changes; the
  client game's fields don't. A field the client game changed after the
  snapshot (prediction, effects) is drawn as it is, and so is the local
  player. A gap of more than 0.1 s between updates means the entity stood
  still, so the change is spread over the last 0.1 s; a move of more than
  256 units is a teleport and isn't spread.
- `r_seqblend` (0.1 s): when a skeletal model's `frame` changes, the pose it
  had then fades out over that time instead of snapping, unless the client
  game blends with `lerpfrac` itself or poses it with a skeleton object.

## Things that have to hold

- **`self` survives builtins.** `addentities` runs each entity's `predraw`
  with `self` set to that entity, then must restore the caller's `self`.
  Nuclide calls it from `ncView::UpdateView`, and the method calls after it
  (`SetupView`, `RenderView`) dispatch through fields of `self`. With `self`
  left on the last entity, those fields hold another class's methods (see
  the overlapping fields note in progs-format.md), and the 3D view was
  silently never drawn.
- **`numclientseats` is 0 during loading.** `View_Init`, run from
  `CSQC_RendererRestart`, creates the view model entities for the seats
  numbered at or above `numclientseats`. It only covers seat 0 if the
  global is still 0 then. The engine sets it to 1 once the player is in
  the game.
- **The console gets out of the way on entering the game,** as in Source.
- **`player_localentnum`** is the server entity number of the local player;
  Nuclide finds its player with `findfloat(world, entnum, ...)`.

## Console commands the client game sends

- `sky "<name>"` from `Sky_Update`, with the map's `skyname` server key.
- `music <path> [-]` from `Music_ParseTrack`/`Music_ParseLoop`. The path is a
  file (often `music/trackNN.flac`) or a bare CD track number; a trailing
  `-` plays once. This engine falls back to `.ogg` and `.wav` under the same
  name, since it doesn't decode FLAC.
- `stopmusic`, `snd_restart`.

## Sound

Entity sounds from the server use server entity numbers. The client game's
`sound()` uses its own entity numbers. The mixer keeps the two apart, and
follows an entity through its csprogs copy when a sound has
`SOUNDFLAG_FOLLOW`. Channel 0 mixes any number of samples, other channels of
an entity replace each other, and `SOUNDFLAG_NOREPLACE` leaves a playing
channel alone, as `fteextensions.qc` describes. Attenuation names a sound
level in dB that falls off with distance as in Source (see "Distance and
direction" in sound.md); 0 plays it everywhere. A WAV
`cue ` chunk marks the frame a sample loops back to.

## The camera and view models

- `view_angles` is set from the player's aim before `CSQC_UpdateView`. It
  is the game's working copy for that view: Nuclide adds bob and strafe
  roll to it and hands it to the camera. Changes to it are not fed back
  into the aim. The game turns the player with `setproperty(VF_CL_VIEWANGLES)`
  or `input_angles`. Feeding the bobbed angles back used to make the pitch
  run into the ±89° clamp and the roll build up to 76° within a second of
  walking.
- Models are drawn with their pitch inverted, as in Quake, while
  `r_meshpitch` is -1 (the default). Nuclide sets `r_meshpitch 1` when its
  server game starts, and then models use their pitch as a view does. The
  server publishes `r_meshpitch` in serverinfo and clients copy it. View models
  (RF_VIEWMODEL or RF_DEPTHHACK) are the exception: Nuclide gives them the
  camera's own angles, so drawing them with the camera's convention keeps
  the gun pointing where the camera does. FTE, run as a black box, inverts
  them too, so its gun swings up when the player looks down.
  `r_viewmodel_quakepitch 1` restores that behaviour.
- View models are drawn with a near plane of 1 unit, so the gun doesn't
  clip, and with `r_viewmodel_fov` as their field of view when it is set
  (0 uses the view's). RF_DEPTHHACK squeezes them into the front 30% of the
  depth range.
- When the command line starts a game, the menu progs load only once that
  game is running. Their init then sees `clientstate()` as 2 and doesn't
  queue the background map in front of the game.
