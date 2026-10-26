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
channel alone, as `fteextensions.qc` describes. Attenuation 1 fades a sound
out over `snd_nominaldistance` (1000) units; 0 plays it everywhere. A WAV
`cue ` chunk marks the frame a sample loops back to.
