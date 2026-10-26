# Bulwark engine

A permissively licensed (ISC) engine for running [Nuclide](https://code.idtech.space/vera/nuclide)
games. It is a clean-room reimplementation: it speaks the same QuakeC
interface Nuclide targets, but it is not derived from FTEQW or Quake's source.
See [CLEANROOM.md](CLEANROOM.md) for the rules.

Full Quake compatibility is not a goal. The aim is a small, modern base for
new games.

## Status

- **QuakeC VM**: loads FTEQCC v7 progs (16-bit records), the full extended
  opcode set, runtime strings with garbage collection, memalloc heap, and
  about 260 builtins. Self-tests in `tests/qc`.
- **Dedicated server** (`bulwark-ded`): loads Q3-family BSPs (IBSP, RBSP,
  FBSP) with brush collision, and Q1 BSPs for entities only. Runs Nuclide's
  server progs, including the multiprogs it adds (game rules).
- **Networking**: UDP with a reliable/unreliable channel, delta-compressed
  snapshots of `SendEntity` entities, client input with prediction, and
  lag compensation. A listen server talks to its own client over loopback.
- **Client** (`bulwark`): SDL2 window and input, Nuclide's csprogs and
  `hud.dat`, the console.
- **Renderer**: OpenGL 3.3 core. Q3-family world surfaces (including
  patches) with lightmaps, sky boxes, `.mat` and shader-script materials,
  Quake MDL and IQM models with the light grid, a batched 2D layer with
  TrueType fonts, screenshots. Nuclide's maps draw with its HUD.
- **Sound**: a software mixer over SDL audio with entity channels,
  spatialisation, WAV and Ogg Vorbis samples, and streamed music.
- **Scenes** (`bwscene`, `src/scene`): s&box's `.scene` and `.prefab` JSON
  format. Loads and saves s&box files, expands prefab instances, and spawns
  `Bulwark.Entity` components as QuakeC entities. `map foo` loads
  `maps/foo.scene` when it exists. See [docs/spec/scene-format.md](docs/spec/scene-format.md).
- Not yet: skeletal animation, particles, decals, PVS, pk3 files, Q1 BSP
  rendering and collision, the menu progs.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build        # needs FTEQCC=/path/to/fteqcc to rebuild the test progs
```

## Running Nuclide

```sh
./build/bulwark -basedir /path/to/nuclide -game base +map ai
./build/bulwark -basedir /path/to/nuclide -game base -width 1280 -height 720 +map ai
```

Client options: `-width`, `-height`, `-fullscreen`, `-headless` (no window
or sound), plus the server's options below. Useful commands: `screenshot`,
`r_speeds 1` (2 also lists the scene's entities), `pr_trace <name>` (prints
calls to QC functions whose names contain it), `music <file>`, `play
<sample>`.

## Running Nuclide's server

```sh
./build/bulwark-ded -basedir /path/to/nuclide -game base +map ai
./build/bulwark-ded -basedir /path/to/nuclide -game base -frames 720 +map ai   # 10 s headless, then exit
```

Options: `-basedir dir`, `-game dir` (repeatable, later ones override),
`-frames n`, `-developer`, and `+command args` for console commands.

## Scenes

```sh
./build/bwscene -basedir /path/to/nuclide -game base frombsp maps/ai.bsp ai.scene   # BSP entities into a scene
./build/bwscene -basedir /path/to/nuclide -game base check ai.scene                 # expand prefabs, count entities
./build/bwscene ents ai.scene                                                      # the entity text it spawns
./build/bwscene resave old.scene new.scene                                         # upgrade to the current format
```

## Docs

- [docs/spec/progs-format.md](docs/spec/progs-format.md)
- [docs/spec/opcodes.md](docs/spec/opcodes.md)
- [docs/spec/builtins.md](docs/spec/builtins.md)
- [docs/spec/bsp.md](docs/spec/bsp.md)
- [docs/spec/scene-format.md](docs/spec/scene-format.md)
- [docs/spec/materials.md](docs/spec/materials.md)
- [docs/spec/client-game.md](docs/spec/client-game.md)
