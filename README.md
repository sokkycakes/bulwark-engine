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
  FBSP) with brush collision, and Q1 (BSP29, BSP2) and GoldSrc (BSP30) maps
  with hull collision, including Half-Life's four hulls. Runs Nuclide's
  server progs, including the multiprogs it adds (game rules).
- **Networking**: UDP with a reliable/unreliable channel, delta-compressed
  snapshots of `SendEntity` entities, client input with prediction, and
  lag compensation. A listen server talks to its own client over loopback.
- **Client** (`bulwark`): SDL2 window and input, Nuclide's csprogs and
  `hud.dat`, the console.
- **Renderer**: OpenGL 3.3 core. Q3-family world surfaces (including
  patches) with lightmaps, sky boxes, `.mat` and shader-script materials,
  Quake MDL and IQM models with the light grid, skeletal IQM animation
  with the `skel_*` and tag builtins, a batched 2D layer with
  TrueType fonts, screenshots. Nuclide's maps draw with its HUD. Q1 and
  GoldSrc worlds (per-texture palettes, WAD3 textures, RGB lightmaps),
  PVS culling, r_part particle scripts, decals and player sprays.
- **Sound**: a software mixer over SDL audio with entity channels,
  spatialisation, WAV and Ogg Vorbis samples, and streamed music. Room
  effects: the game's reverb slots (`setup_reverb`), GoldSrc room types
  (`room_type`), underwater muffling, and an automatic room guess.
- **Input**: keyboard, mouse and SDL game controllers. Packages: `.pk3`
  and Quake `.pak`.
- **Bots**: a game-side QC bot framework in `qc/bots` (action stack,
  locomotion with climbing and gap jumps, vision, waypoint paths and chasing,
  and player bots that play as fake clients through player input), with
  headless scenario tests in `tests/bots`, including a TF2-style match.
- **Scenes** (`bwscene`, `src/scene`): s&box's `.scene` and `.prefab` JSON
  format. Loads and saves s&box files, expands prefab instances, and spawns
  `Bulwark.Entity` components as QuakeC entities. `map foo` loads
  `maps/foo.scene` when it exists. See [docs/spec/scene-format.md](docs/spec/scene-format.md).
- **FTEW worlds**: the compiled world format from the Godot-based Stiletto
  exporter, loaded wherever a BSP is (collision, drawing, entities), and
  nameable from a scene's `MapInstance`. See [docs/spec/ftew.md](docs/spec/ftew.md).
- **WebCore games** (Stiletto): the HTML menus and HUD of FTE's webcore
  plugin run as Panorama ports shipped in `data/panorama/`, with the
  `fte_query` bridge, `gecko_*` views, the title backdrop map and menu
  music, and a pre-game lobby (create, join by address or room code on the
  LAN, ready, chat, countdown, start). See
  [docs/spec/webcore.md](docs/spec/webcore.md) and [docs/spec/lobby.md](docs/spec/lobby.md).
- Not yet: ragdolls.

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
or sound), plus the server's options below. Every console command is listed in
[docs/spec/console.md](docs/spec/console.md). Useful ones: `screenshot`,
`r_speeds 1` (2 also lists the scene's entities), `pr_trace <name>` (prints
calls to QC functions whose names contain it), `music <file>`, `play
<sample>`.

## Running Stiletto

Stiletto is a Nuclide game made for FTE with its webcore HTML plugin. Run
it like any Nuclide game; its autoexec loads the plugin, which Bulwark
answers itself, and the title page opens over its background map:

```sh
./build/bulwark -basedir /path/to/nuclide -game base
```

Bulwark finds the Panorama ports of the game's pages in its own `data/`
directory (next to the executable, or the source tree's when run from a
build directory; `-enginedata <dir>` points elsewhere). Build Stiletto's
progs from its source first (`make game GAME=base` in the Nuclide tree, with
FTEQCC): its repository ignores `*.dat`, and the few checked in are old.

## Web export

A game can be exported as a folder any web host can serve, and played in
Chrome, Firefox and Android handheld browsers:

```sh
tools/web/build.sh build-web            # needs the Emscripten SDK
python3 tools/web/export.py --basedir /path/to/nuclide --game base --engine build-web \
    --out out/web-base -- +map ai
python3 tools/web/serve.py out/web-base
```

See [docs/platforms/web.md](docs/platforms/web.md), including how browser
players join a desktop server.

## Running Nuclide's server

```sh
./build/bulwark-ded -basedir /path/to/nuclide -game base +map ai
./build/bulwark-ded -basedir /path/to/nuclide -game base -frames 720 +map ai   # 10 s headless, then exit
```

Options: `-basedir dir`, `-game dir` (repeatable, later ones override),
`-frames n`, `-developer`, `-userdir dir` (write configs and saves there
instead of the game folder), `-port n`, and `+command args` for console
commands. The game client does the same with `bulwark -dedicated ...`.
The server runs the game's configs and then `server.cfg`, and takes
commands typed on the terminal (`status`, `serverinfo`, `map`, `kick`,
`addBot`, `quit`). See [docs/spec/dedicated.md](docs/spec/dedicated.md).

## Scenes

```sh
./build/bwscene -basedir /path/to/nuclide -game base frombsp maps/ai.bsp ai.scene   # BSP entities into a scene
./build/bwscene -basedir /path/to/nuclide -game base check ai.scene                 # expand prefabs, count entities
./build/bwscene ents ai.scene                                                      # the entity text it spawns
./build/bwscene resave old.scene new.scene                                         # upgrade to the current format
```

Scenes are edited in Godot with the addon in [tools/godot](tools/godot/README.md):
an entity browser and typed key editor built from Nuclide's entity
definitions, previews of BSP and FTEW worlds, Hammer-style views and grid,
and export straight to `.scene` and `.prefab` with a Run map button.

## Docs

- [docs/spec/progs-format.md](docs/spec/progs-format.md)
- [docs/spec/opcodes.md](docs/spec/opcodes.md)
- [docs/spec/builtins.md](docs/spec/builtins.md)
- [docs/spec/console.md](docs/spec/console.md)
- [docs/spec/bsp.md](docs/spec/bsp.md)
- [docs/spec/scene-format.md](docs/spec/scene-format.md)
- [docs/spec/ftew.md](docs/spec/ftew.md)
- [docs/spec/materials.md](docs/spec/materials.md)
- [docs/spec/client-game.md](docs/spec/client-game.md)
- [docs/spec/particles.md](docs/spec/particles.md)
- [docs/spec/sound.md](docs/spec/sound.md)
- [docs/spec/bots.md](docs/spec/bots.md) and [qc/bots/README.md](qc/bots/README.md)
- [docs/spec/ui.md](docs/spec/ui.md), [docs/spec/webcore.md](docs/spec/webcore.md), [docs/spec/lobby.md](docs/spec/lobby.md)
