# Bulwark on the Nintendo DS

The DS build doesn't run the engine. It is a small, separate program with
two jobs:

- a **link client**: the DS joins a game running on a PC over WiFi as its
  own player, like a GBA on a link cable. The game draws a private screen
  for it on the bottom screen, and the top screen shows the map from the
  player's position (see "Joining a game" below);
- a **map viewer** that loads Quake-format maps (BSP version 29), with the
  monsters and items they place as Quake models (`.mdl`), and flies through
  them on the DS's 3D hardware.

Files come from the base folders, then from `pak0.pak` to `pak9.pak` in
them, so Quake's own `pak0.pak` (the shareware one works) can go in
`/bulwark/base/` as it is.

## Why it isn't the engine

| | DS / DS Lite | What the engine needs (3DS build, Nuclide `ai`) |
|---|---|---|
| RAM | 4 MB | about 66 MB |
| CPU | 67 MHz ARM9, no FPU | float everywhere |
| GPU | fixed function, one texture per polygon, 2048 polygons a frame | five shader programs, lightmaps |

Nuclide's three progs files alone take 3.9 MB. A flashcart such as the
DSPico adds storage, not RAM. So the DS gets the part of the engine that
fits the hardware: drawing a world.

## Building

```sh
docker run --rm -v $PWD:/src -v $PWD/build-nds:/b -w /b devkitpro/devkitarm \
  bash -c 'cmake /src -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/NDS.cmake -DCMAKE_BUILD_TYPE=Release && make'
```

This produces `bulwark_nds.nds`. The code is about 220 KB. To pack maps into
the ROM, add `-DBULWARK_NDS_NITROFS=/path/to/folder`. The folder can hold
`maps/*.bsp`, `gfx/palette.lmp` or `gfx/palette.tga`, and `bench.txt`. The
packed files appear as `nitro:/`.

## Running from a flashcart (DSPico and others)

Copy `bulwark_nds.nds` anywhere on the card, then lay out the files like
this:

```
/bulwark/base/maps/*.bsp       Quake (BSP 29) maps
/bulwark/base/gfx/palette.lmp  the Quake palette (or Nuclide's gfx/palette.tga)
/bulwark/base/bench.txt        optional, see below
```

The viewer reads the SD card through the cart's DLDI driver. It also looks
in `/bulwark/` and in the card's root. Without a palette, the textures come
out grey.

## Joining a game

Pick *[Join a game over WiFi]* at the top of the menu. The DS connects with
its own WiFi settings, or with `link.txt` from a base folder (for example
`/bulwark/link.txt` on the SD card):

```
ssid=MyNetwork
key=passphrase      WPA/WPA2 passphrase, or a WEP key in hex
security=wpa2       open, wep, wpa or wpa2 (leave it out to take the best offered)
server=192.168.1.20:27901   optional: asks this server as well as broadcasting
name=Player         optional
autojoin=1          optional: join the first server found at start-up
controls=touch      touch (default) or classic, see below
hand=right          left: move with A B X Y and fire with R
sens=100            aim speed in percent (100 turns 120 degrees across the screen)
diag=0              hide the frame rate, polygons and models along the top screen's top edge
fps=30              optional: cap at 30 fps (every frame takes two refreshes), for testing
predict=0           optional: show where the server says you are, without prediction (below)
cmdvb=2             optional: one movement command every 2 refreshes instead of every one, for testing
botwalk=1           optional: walk, strafe, jump and crouch by itself in a fixed 12 s pattern, for testing
interp=80           optional: ms other entities are drawn behind the server, 0 for as sent ("Other entities", below)
lerpframes=0        optional: models step from baked frame to frame instead of blending
volume=0.7          optional: sound volume, 0 to 1 (the PC's default)
sndcache=192        optional: KB of samples kept in RAM, 64 to 512 ("Sound", below)
sndreadms=2         optional: ms a frame the sound may spend reading the card, 1 to 16
sound=0             optional: no sound
snd_log=1           optional: log each sound and a line of stats a second through the no$gba port (2: the stats only)
```

The PC runs a normal Bulwark server (`bulwark-ded` or a listen server).
Link players use UDP port 27901 (`sv_link_port`). The DS lists the servers
that answer, and A or a tap joins one.

While linked, the bottom screen is a touch pad, as in Metroid Prime
Hunters:

- Its top 32 rows are the HUD's (`Hud_Sub`, below): the experience gauge
  flush along row 0, the full width and 11 rows high, with the level, rank
  and medals in it and the weapon in hand at its right; under it (rows
  14-28) the hero's ability squares and Energy (or armour). The score is not
  shown here (the objective panel on the top screen has it). The **GAME**
  tab right of the Energy (x 200-254, rows 14-28) swaps the pad for the
  game's own bottom screen (what the game draws with the `link_*` builtins,
  or the engine's name, health, map and player list); **PAD**, in the same
  place, swaps back. Touches on the game's screen go to the game.
- Drag in the middle (rows 32-143) to aim. A double tap jumps.
- The top screen shows health and Barrier (the game's armour) in big
  numbers at the bottom left. Its first line, right-justified at the top,
  shows frames a second, the slowest second's rate, polygons, and models
  loaded and failed, and the second line why the last map didn't load, if
  it didn't. `diag=0` hides the first line. This HUD is a layer in front of
  the 3D (BG1 in VRAM F and G, one 4-bit tile per 8x8 cell, so it works as
  a bitmap), drawn with a 1-pixel black outline in `fonts/hud.dsf` from a
  base folder or the ROM, else the console's 8x8 font.
  `tools/nds/dsfont.py font.ttf 8 hud.dsf` bakes a pixel font into it
  (sokky's pick is 04b09 from the 04 series, which is private: the baked
  file is not in git). The big numbers (health, Barrier, clip at 3x, the
  reserve at 2x) and the objective panel (1x) are in a second font,
  TallPixelFont at 9 px (SIL OFL, `data/fonts/`): it is free to ship, so
  it is built into the ROM (`src/platform/nds/hudnum_dsf.h`, made by
  `tools/nds/dsfont.py data/fonts/tallpixelfont.ttf 9 hudnum_dsf.h`), and a
  `fonts/hudnum.dsf` in a base folder or the ROM overrides it. No SD-card
  or ROM file is needed for it.
- Eight buttons: previous and next weapon, reload, use, jump, duck (stays on
  until pressed again), flashlight and secondary fire.

| Button | `controls=touch` (default) | `hand=left` | `controls=classic` |
|---|---|---|---|
| D-pad | move and strafe | (free) | up / down move, left / right turn |
| A B X Y | attack, jump, use, reload | X / B move, Y / A strafe | attack, jump, use, reload |
| L | attack | jump | held: D-pad strafes and looks up / down |
| R | jump | attack | secondary fire |
| touch | aim and the pad's buttons | the same | the same |
| START | leave | leave | leave |
| SELECT | debug views (no things, no moved models) | the same | the same |

`classic` is the scheme of Quake or Half-Life on a pad with one stick; the
touch pad still aims. The DS maps the buttons itself and sends what the
game reads (Nuclide's `input_buttons`), plus Quake's impulses 10 and 12 for
the next and previous weapon and 100 for the flashlight. Weapon switching by
impulse needs Nuclide `claude/project-thread-9qaek1` or later; the
flashlight needs a game that handles impulse 100, as the co-op rules do.
The DS itself draws no flashlight beam yet: its map lighting is baked.

The top screen draws the map only if the DS has a copy of it in `maps/`.
Other players show as coloured markers, green for other DS players. The
game can draw the bottom screen and add markers with the `link_*` QuakeC
builtins. Until it does, the engine shows the player's name, health, the
map and the player list. The protocol and builtins are in
`docs/spec/link.md`.

DS hardware has 802.11b WiFi. The DS Lite and the original DS only connect
to open or WEP networks in Nintendo's own software. The libraries here
include WPA2 code, but that hasn't been tried on hardware yet. An open
network or a phone hotspot is the safest first test.

## Half-Life maps

The DS reads Quake's BSP 29, so a Half-Life map (BSP 30, textures often in
WADs) is converted on the PC first:

```
python3 tools/nds/hlmap.py valve/maps/c1a0.bsp nitro/maps/c1a0.bsp -wad valve
```

`hlmap.py` takes the textures from the map or the WADs its worldspawn
names, halves the ones that cover the least of the world until they fit in
180 KB of texture memory, and gives the map one shared 255-colour palette
(`c1a0.pal`; index 255 stays clear for `{` textures). The light becomes
Quake's one byte a sample, with the colours beside it in `c1a0.lit`
(QLIT 1). The clip hulls go to `c1a0.hul` beside it, for the linked
player's movement prediction (below). Each texture
keeps its original size in the header, and a `DSz` tag after it gives the
size actually stored.

The DS loads one lump at a time, so a map needs about its own size in RAM
(c1a0: 695 KB). A map too big to fit with its coloured light (t0a0) loads
again with the grey, and then without joining faces. `hlmap.py` also drops
the planes only the clip hulls used (c5a1: 21126 planes down to 2176); with
both, every campaign map loads. Brush entities built round an origin brush (doors
that swing, trains) start where the map's `origin` key puts them.

Converted Half-Life maps are Valve's data: keep them on your own cards,
not in this repository or a public package.

### Half-Life models

Linked, the DS draws the server's NPCs and the other players with their
own Half-Life models, baked on the PC first:

```
cc -O2 -Isrc/common -o hlmdldump tools/nds/hlmdldump.c <libbwcommon.a> -lm -lz
python3 tools/nds/hlmdl.py valve/models/scientist.mdl nitro/models/scientist.mdl
```

`hlmdldump` poses every frame of every sequence (first sub-model of each
body part, first blend, root motion taken out as the engine does) and
`hlmdl.py` cuts the mesh to about 200 triangles by collapsing edges, keeps
one clip for each activity the game uses (idle, walk, run, attacks, deaths,
flinches) at up to 72 frames in all, and packs the skins into one 128×64
atlas of 255 colours. The result is a Quake model (IDPO 6) with a `DSHL`
trailer: the clips, which clip each sequence plays, and the palette. Player
models go in `models/player/<m>/<m>.mdl`, file names in lower case.

The server names each model index in `MODELS` (docs/spec/link.md). The DS
loads a named model the first time it sees it, one a frame, plays the
clip for the sequence the server says, and walks or runs a model the server
only has standing (other players) from how fast it moves. Things with no DS
copy of their model stay boxes.

The weapon in hand is drawn too, from its view model (`v_*.mdl`), baked
with `-view`:

```
python3 tools/nds/hlmdl.py v_9mmhandgun.dump nitro/models/v_9mmhandgun.mdl -view -tris 250 -frames 60
```

`-view` keeps a clip for every sequence (the game plays them by number) and
leaves out the arms and hands (Half-Life's glove, sleeve and forearm
textures) and additive effect meshes. `-mirror` swaps left and right, for
the models a weapon def marks `model_viewMirror` (CSO's). `STATS` tells the
DS the view model, its sequence, and when a sequence starts again, so each
shot replays it. It costs about 250 polygons.

Baked models are Valve's data too: keep them out of git and public
packages.

### Sound packs

The DS plays the game's sounds from sound packs (`.dsp`) and its music from
streams (`.dsm`), made on the PC:

```
python3 tools/nds/sndpack.py nitro/sound/10base.dsp games/base --report
python3 tools/nds/sndpack.py nitro/sound/40coop.dsp games/coop
python3 tools/nds/sndpack.py nitro/sound/10base.dsp games/base --music nitro/music
python3 tools/nds/sndpack.py --check nitro/sound/*.dsp nitro/music/*.dsm [--thorough] [--against games/base]
```

Each source is a game directory (its loose files beat its own `.pk3`
and `.pak` files, which go alphabetically, as the engine searches them), a
`.pk3`, `.pk3dir` or `.pak`; later sources win. Every `sound/**/*.wav`,
`.ogg` and `.mp3` goes in, named by the hash the server sends
(`docs/spec/link.md`, "Sound"); when one name has a `.wav` and an `.ogg`, the
`.wav` wins, as the PC tries it first. `.ogg` and `.mp3` need `ffmpeg` on
the PATH. Packs go in `sound/` of a DS base directory; the DS looks a sound
up in the pack whose name sorts last first (`10base.dsp`, `20valve.dsp`,
`30hldata.dsp`, `40coop.dsp`), like game directories layered. Music goes in
`music/`, one file a track, named after what the game plays with `/` as
`_` (`music/track02.ogg` is `music_track02.dsm`).

What the tool does to each sound:

- Mono (a stereo sample is mixed down: the PC plays stereo effects in
  stereo), at most 22050 Hz, and at most 11025 Hz for a sample that loops
  (a WAV `cue ` point, as `snd_mem.c` reads it); higher rates are
  resampled with a windowed sinc. The DS's mixer runs at about 32.7 kHz,
  so it would lose most of the rest anyway.
- IMA-ADPCM as the DS's sound hardware decodes it (4 bits a sample, no CPU
  cost), encoded against a copy of melonDS 1.1's decoder, so the SNR the
  tool prints is what melonDS plays. PCM8 instead when ADPCM comes out
  under 18 dB and PCM8 does better (`--adpcm-min-snr`), when the sample is
  under 1 KB, or when `--pcm8 GLOB` names it. 8-bit sources (Half-Life's,
  co-op's) are often such: PCM8 keeps them exactly.
- Lengths are whole hardware words: the end loses up to 7 samples, and a
  loop starts at the next multiple of 8 samples (ADPCM) or 4 (PCM8) at or
  after its cue point. `--report` lists both, per sound.
- A sound over 96 KB as ADPCM (`--stream-kb`) is a stream that the ARM9
  decodes as it plays (in base, only `ambience/wind2`, 172 KB).
- The hardware won't play a sample under 16 bytes; shorter ones are padded.

`--only-used` keeps only the sounds something names: the sound decls
(`decls/sound/*.sndshd`), `sound/sentences.txt`, strings in the progs,
BSP entity lumps and model events, the server's `LINKSNDUSED` dump
(`--used-list`, from `sv_link_log 1`), always `misc/talk`, `common/null` and
the weapon-select sounds, and `--keep GLOB`. It misses names the game
builds at run time (`sprintf("player/pain%d.wav", n)`), so a full pack is
the safe choice; SD cards have room. `--decls DIR` scans more game
directories and lists the samples the decls name that no source has (with
Half-Life's sounds absent, most of valve's).

`--check` reads packs and music files back: the layout, the directory's
order and pages, each sample's header against its entry, the name against
its hash, word alignment and loop points, every block state of a stream;
`--thorough` plays every ADPCM sample through the melonDS model, loops
included; `--against SRC...` converts the sources again and prints each
sound's SNR. `tools/nds/test_sndpack.py` tests the tool with generated
tones.

Measured on base and co-op (the tool's own SNR, against the source as
resampled; melonDS's decoder, not hardware):

| Pack | Sounds | Size | Formats | SNR |
|---|---|---|---|---|
| `10base.dsp` (`games/base`) | 233 | 2.67 MB | 212 ADPCM, 20 PCM8, 1 stream | median 28.1 dB, 10% under 19.7, worst 13.0 (`player/footsteps/tile`, quiet noise) |
| `40coop.dsp` (`zz_coopguns`, `zz_coopweapons`) | 139 | 2.11 MB | 69 ADPCM, 70 PCM8 (exact) | ADPCM median 21.7 dB, worst 18.1 |
| `--only-used`, base | 80 | 0.91 MB | | |
| `--only-used`, co-op | 101 | 1.46 MB | | |
| music, `games/base/music` (11 `.ogg`, 38 min) | 11 | 23.6 MB | mono IMA streams at 22050 Hz, 11 KB/s | median 30.6 dB, worst 18.5 (`track07`) |

The SNR leaves out base's 37 placeholder files (silent but for noise at 7 of
32767, about −73 dB). Building `10base.dsp` takes about 11 s on 4 cores, the music about 6 minutes.

Packs and music made from Half-Life's `valve/sound`, `hldata` or co-op's
`zz_*.pk3` are Valve's or the mod's data: keep them on your own cards, out
of git (the `.gitignore` has `*.dsp` and `*.dsm`) and out of public
packages. The tool won't write into a git work tree without `--allow-repo`,
and says PRIVATE when a pack comes from such a source.

## Feel: frame rate, view motion, effects

Turning and the viewer's flying go by the refreshes a frame took, not by
frames, so they keep their speed when a busy view drops to 30 fps. The
linked client's jump key is held 8 refreshes, so a 30 fps frame can't drop
it. On a map with clip hulls the DS predicts its own movement (next
section). Without them it shows the view the server sends 20 times a
second, eased toward where the last two said it is heading, up to 3
refreshes ahead.

The view's motion is a port of the Nuclide client code the PC runs for the
co-op game (valve/csprogs.dat), from the velocity, on-ground and crouch
flags of the predicted movement (or of `VIEW` without prediction): the camera bob of `valve/src/client/viewmodel.qc`
(`Viewmodel_CalcBob`) along the camera's up, the strafe roll of
`valve/src/client/camera.qc` (`Camera_StrafeRoll`, 0.015 degrees per unit a
second sideways), and the view model swing, drift and landing kick of
`src/client/viewbob.qc` (`cg_viewBobStyle 3`, the default). The DS turns
the gun by the bob angles about the eye rather than adding them to the
camera's angles, which differs only by a fraction of a degree. The view
model, our muzzle flash, the punch and the ammo come from the weapon
prediction ("Weapons" below); without it the view model plays the sequence
the server says (`STATS`). Bullet impacts (`FX`) draw as a spark and fading dust, our
own shots add a tracer (and a muzzle flash at the view model's barrel when
the weapon isn't predicted), and
other players' and monsters' shots a flash and a tracer from their gun.
These are untextured shapes; HL's sprites are not used.

## Prediction: moving as the PC client does

Linked to a link v2 server (`docs/spec/link.md`, "Prediction"), the DS
moves its own player the way the PC client does in the co-op game. The PC
runs Nuclide's prediction (`ncPlayer::PreFrame`, `src/shared/game/Player.qc`):
each command goes through the game's player movement (`Physics_Run`, then
`PMoveCustom_RunPlayerPhysics`) at once, and when the server's state for a
command comes back, the commands it hasn't run yet run again from that
state. The DS does the same in C:

- `pmove_nds.c` is that movement code (`src/shared/physics/pmove_custom.qc`,
  `player_pmove.qc` and the valve player's jump and sprint), one C function
  for each QuakeC one, in the same float operations and order, so it gives
  the server's results bit for bit. The movement values (`ncPMoveVars`:
  the global set and the player's own, which co-op's heroes change) come
  from the server (`PMOVEVARS`).
- `hull_nds.c` traces through the map's clip hulls the way the server's
  `SV_Move` does: the world, then the brush entities (`BRUSHINFO`, with the
  doors and lifts near the player where `PSTATE` says they are) and the
  boxes of the monsters and players near it, in the server's order. The
  hulls come from a `.hul` sidecar: `hlmap.py` writes one with every
  Half-Life map, and `hlmap.py --hul-only e0m1.bsp e0m1.hul` makes one for
  a Quake or Nuclide map (the DS can also take those from the map's own
  clip hulls). Point traces use the drawing tree the DS already holds.
  A `.hul` is refused ("bad .hul") unless every index is in range and each
  node's children come after it (as BSP compilers write clipnodes), so a
  damaged one can't send a trace round a loop.
- A command is made every refresh (16 or 17 ms, so two a frame at 30 fps).
  The PC's are 14 ms; 33 ms ones jump 2.5 units lower (39.6 against 42.0
  on `coop_arena`, `tools/nds/pmtest -resample`), 17 ms ones 0.4 lower. The
  keys make the PC's move values (400 forward, back and sideways,
  normalised as `ncPlayer::ClientInputFrame` does). At most 30 packets a
  second carry every command the server hasn't run, up to 8, so a lost
  packet loses nothing.
- The camera is the predicted eye, with the stair smoothing of
  `ncView::StairSmooth` (before the bob, where the PC does it after); the
  bob, roll and view-model swing read the predicted velocity, on-ground
  and crouch flags.
- When the server's state for a command matches what the DS predicted for
  it (within 1/32 unit and 0.5 units a second, and every other value the
  movement reads: a long jump picked up or a weapon's speed counts too),
  nothing runs again. Otherwise the commands after it run again, 16 a frame
  at most, and no more once 8 ms have gone and 4 have run: a longer backlog
  (a stall, or a heavy map) goes on over the next frames, with the view on
  the old prediction meanwhile, rather than one frame of up to 100 ms. When
  the game turns the player (a teleport, a respawn) the DS takes the facing
  and runs the commands the server hasn't again from its state, as the PC
  does. A state made with movement values or a brush list the DS doesn't
  have yet (a door came to rest somewhere new) is skipped, and prediction
  goes on.
- Below 30 fps a command covers two refreshes, so a frame never runs more
  than two (three at 20 fps would slow a heavy map further).
- The hulls load after the map only if 256 KB stay free for the studio
  models that come after it: on a map that only just fits, the monsters
  come first and the link goes without prediction.
- Without hulls (no `.hul` and no clip hulls in the map, or `hull: no RAM`
  on the second line of the top screen), on an old server, or while dead
  or spectating, the view is the server's, as before (`P` at the end of
  the first line shows prediction is on). The commands still go out, and
  the server runs each once.

What the DS can't predict, the PC can't either: teleports, a push with a
target, a lift or train carrying the player, monsters knocking it back or
moving into it. The next state corrects it, 30 times a second.
`trigger_push` and `func_conveyor` are predicted (see "Prediction over the
Half-Life campaign" below).

### Measured, in melonDS 1.1 (not hardware)

`bulwark-ded` with real collision and gravity: `coop_arena` (Half-Life
BSP 30, co-op rules) and the Nuclide base maps `e0m1` and `dev` (BSP 29,
`-game base`). The DS had the converted `coop_arena` with its `.hul`, and
the base maps with theirs. `botwalk=1` walked, strafed and ran into walls,
jumped and crouched; each run is 30 s with `link_god 1`, waves of monsters
on `coop_arena`. "150 ms" is `sv_link_lag 75` (75 ms each way).

- **The server's result.** `tools/nds/pmtest` replayed every command the
  server logged (`sv_link_log 1`) in these runs through the same C code
  built for the PC, with the DS's fixed-point drawing tree for point
  traces: 2272 of 2272 commands on `coop_arena` at 30 fps, 2279 at 60, 2301
  at 150 ms, 2376 on `e0m1` and 2482 on `dev` came out bit for bit. Built
  for the DS and run in melonDS on 866 logged commands (`coop_arena`,
  `e0m1`, `dev`), all bit for bit too.
- **On the DS, against each `PSTATE`** (the table): on `e0m1` and `dev`
  every state matched, most bit for bit and the rest by less than a
  hundredth of a unit: newlib's `sinf` and `cosf`, which differ from
  glibc's in the last bit for 13022 of the 65536 angles a command can
  carry (checked in melonDS against the PC). The DS now computes them as
  glibc does (below, "Speed"), and every state on both maps matches bit
  for bit. On
  `coop_arena` a few states a second differ by up to about a unit. They
  are the commands the server cut short (`clipped`): melonDS runs the DS
  0.25 % faster than real time (89.26 DS seconds in 89.04), and the
  server's movement budget follows its own clock. A DS's clock is within a
  few parts per million, so that should not happen on hardware. Monsters
  pushing against the player gave a few seconds of 0.4 to 10 unit errors
  in another run.
- **Cost** (before "Speed" below, which brought it to about 0.5 ms).
  One command takes 1.0 to 1.3 ms on `coop_arena` and 1.4 to
  1.9 ms on `e0m1` and `dev` (worst 4.4 ms); the hull traces are about
  half. At 30 fps that is 2.5 to 5 ms a frame. With 150 ms between the DS
  and the server a mismatch runs up to 8 commands again: 15 ms in that
  frame. Standing in the same spot of `coop_arena` at 60 fps, a frame
  took 13.1 ms on average against 12.0 ms with `2c5c447`, both at 59 fps.
- **Traffic.** `PSTATE` comes 30 times a second (180 to 400 bytes): the
  DS received 20 to 24 KB/s on `coop_arena` (11 KB/s before) and sent
  2.3 KB/s (0.7 before), 4.8 KB/s at 150 ms.

| Map, frame rate, delay | fps | Prediction per frame, average (worst) | States compared | Within 0.5 units | Error, units: average (worst) | Replays (commands run again) |
|---|---|---|---|---|---|---|
| `coop_arena`, 30 fps | 30 | 2.9 ms (4.0) | 900 | 888 | 0.019 (1.1) | 54 (0 commands) |
| `coop_arena`, 60 fps | 60 | 1.3 ms (3.2) | 899 | 895 | 0.007 (1.4) | 31 (0) |
| `coop_arena`, 30 fps, 150 ms | 30 | 3.1 ms (17.9) | 746 | 734 | 0.018 (1.6) | 34 (274, up to 10 at once) |
| `coop_arena`, 30 fps, 400 ms | 30 | 4.5 ms (28.6) | 837 | 829 | 0.017 (1.3) | 50 (1298, up to 24, 16 a frame) |
| `coop_arena`, 30 fps, standing | 30 | 2.5 ms (3.3) | 900 | 900 | 0 (0) | 28 (0) |
| `e0m1`, 30 fps | 30 | 4.3 ms (7.8) | 734 | 734 | 0 (0) | 0 |
| `dev`, 30 fps | 30 | 2.9 ms (4.1) | 900 | 900 | 0 (0) | 0 |

These are from the build after the review fixes (the brush entities are
placed once per list, not on every state: `dev`, with 53 of them, went
from 5.1 to 2.9 ms a frame). With a replay limited to the newest 16
commands instead of spread over frames, 400 ms put the view 30 units off
on average; spread, it stays as close as at 150 ms. Every logged command
of these runs came out bit for bit in `pmtest`.

RAM: the prediction code is 43 KB, its static data 18 KB, the hulls 10 KB
(`coop_arena`) and 21 KB (`e0m1`). On `coop_arena` the heap in use went
from 529 KB to 406 KB against `2c5c447`, from keeping a model's frames
exactly and reading big models in parts. The lowest free memory on c2a4e
is still 68 KB: that is the peak while the map itself loads, before any
model or hull. After the load, c2a4e had 1.50 MB free with `2c5c447` and
1.67 MB with this build; its hulls' size is not known yet (no original
map here).

Not measured yet: ladders and water (no map here has them), doors and
lifts moving under the player, a Half-Life campaign map with hulls, and a
DS.

### Speed: the same results in a third of the time

The movement code gives the server's results bit for bit, so making it
faster may not change a single one. `tools/nds/pmtest -dump -fuzz 8`
writes every output state of a logged run (and of the same commands with
their inputs nudged); each step below gave the same dump as before on all
seven reference logs, and a build with `-DHULL_SELFCHECK` checked each
shortcut against the long way. What changed:

- Float compares and `sqrtf` take exact integer shortcuts where the
  answer is certain (`fexact_nds.h`); a compare that isn't certain goes to
  the float routine as before.
- `sinf` and `cosf` are glibc's (the server's) instead of newlib's, so
  they agree on every one of the 65536 angles a command can carry
  (`pmtest -selftest`, and in melonDS).
- The ladder and water probes are left out when the hull near the player
  can't have a ladder or water in reach (they would find nothing).
- The trace code and libgcc's soft-float routines run from ITCM; the
  hull walk keys its nodes; a general plane is kept in fixed point to see
  which side a box is on, the float test deciding only when it is close.
- The last 32 traces are kept, and a trace the same as one of them takes
  its result (relying on `Hull_Focus` never changing a result, which
  `pmtest`'s `focus_vs_nofocus` checks).

`PM_Run` per command in the DS bench (melonDS 1.1, average / worst, about
16 µs of the bench's own timing included): `coop_arena` 1129 / 1560 µs
before, 383 / 721 µs after; `e0m1` 1638 / 3225 to 487 / 998; `dev`
1547 / 1986 to 638 / 1035. In the linked runs below it is 483 to 548 µs.
The code in ITCM grew from 2.8 KB to 18 KB (of 32 KB); the trace cache
takes 4 KB of static data.

## Other entities: drawn as the PC draws them

The PC client draws every networked entity a little in the past, between
the two snapshots around that moment (`CSQC_LerpEntity`,
`src/client/cl_csqc.c`): origin, angles and the time into the model's
sequence (`frame1time`), `cl_interp` (0.03 s) behind the newest snapshot;
a jump of over 256 units is a teleport, and after a long gap the change is
spread over the last 0.1 s. It poses Half-Life models at any time into a
sequence. Other players it doesn't draw in the past: Nuclide's
`ncPlayer::predraw` (`src/shared/game/Player.qc:740-756`) runs their
movement on from their networked velocity every frame,
`cg_extrapolateClients` 1 (the default; no shipped cfg changes it). The DS
does the same with what `ENTS` carries (`docs/spec/link.md`): the server's
time, each thing's `frame1time`, and the other players' velocity.

- The DS reckons the server's clock as the PC does (`update_clock`): it
  runs at real-time speed, nudged toward the newest `ENTS` time plus how
  long ago it came. Things and moved brush models (doors, lifts, trains)
  are drawn 80 ms behind it (`interp=` in `link.txt`, 0 for as sent), with
  the PC's rules for teleports and long gaps. The PC's 30 ms put it
  between two of its 72 snapshots a second; `ENTS` come 20 times a second,
  so the DS waits one more interval (50 ms).
- Models play their sequence at the time the server's `frame1time` says,
  run on between updates. Their baked frames (at most 10 a second,
  `hlmdl.py`) are blended, the way the PC's skeletal pose moves on
  between keyframes (`lerpframes=0` turns this off): the 12 nearest models
  within 768 units, the rest step from frame to frame.
- Other players are drawn at the newest update moved on by their velocity
  to the reckoned server time, for at most 0.1 s, facing the newest way
  and with their sequence run on as far. Nuclide runs the full movement
  code for this (`Physics_Run`, every frame until the next snapshot), whose
  `UpdatePlayerAnimation` turns them to the newest `v_angle` and runs their
  animation on (`Player.qc:509-519`). The DS moves them in a straight line:
  that differs when one hits a wall or lands, and in the air, where
  `Physics_Run` adds gravity (up to 4 units in 0.1 s), and on the ground,
  where it adds friction; and past 0.1 s without an update the DS waits
  where the PC goes on. Their legs walk and run as `UpdatePlayerAnimation`
  picks (`Player.qc:555-579`): idle under 1 unit a second, walking under
  150, running over 200, and between those what they were. Their markers
  (the coloured quads for players the DS has no model for) go with them.
- A thing that was out of `ENTS` (only the nearest 48 are sent) for 3
  updates or more, or comes back with another model on its edict, starts
  again with no earlier updates, as the PC makes a new entity of it
  (`cl_csqc.c:428`): it is drawn as sent until a second update comes.
- A door or lift coming home (the server leaves a brush model at its
  origin out of `ENTS`) is drawn on toward home until the drawn moment
  passes that update, then at home; leaving home, it starts from where the
  update before had it, as the PC's entity does. More than 32 brush models
  moving at once: the rest ease toward the newest, as from an older server.
- The doors and lifts `PSTATE` lists (those near the player) are drawn
  where the movement prediction collides with them: the server's place for
  them at the acknowledged command. The predicted player isn't carried by
  them (the PC's isn't either), so its feet stay on the lift as drawn; the
  PC draws the lift 30 ms in the past instead. Not tried on a moving lift:
  none of the maps here has one the rig rides.
- From an older server (no time in `ENTS`), updates are timed by when
  they come, and brush models ease toward the newest as before.

RAM: 64 entities and 32 brush models keep their last 3 updates, and a
blended frame is built in a 2 KB buffer: 9.2 KB of heap, taken at the
first `ENTS` after a map has loaded and given back before the next map
loads, so it doesn't come out of the lowest free memory, which is while a
map loads (the blend buffer is asked for once a map; without room models
step from frame to frame). A clip plays for as long as its sequence:
`hlmdl.py` writes each clip's lap in ms (`DSLP`, before the trailer,
where older loaders don't look); a model baked before keeps its whole
frames a second, which could be 11 % off (10 frames of a 3 s loop at
3 fps). The code is 4.4 KB more; static data 1.6 KB
less.

### Measured, in melonDS 1.1 (not hardware)

`coop_arena`, 30 fps, 40 monsters (`coop_waveBase 40`, `coop_wavePerWave
0`, `coop_spawnInterval 0.2`), the DS standing; one traced monster as it
walks, and another DS walking (`botwalk=1`) as the other player. Steps are
how far the drawn position moved from one frame to the next:

| Traced | Build | Steps: average (spread) | Frames it stood still |
|---|---|---|---|
| a walking monster | `a5b9769` | 1.8 units (2.0) | 57 % |
| a walking monster | this one | 2.3 units (0.9) | 9 % |
| the other DS, running | `a5b9769` | 7.3 units (8.1) | 43 % |
| the other DS, running | this one | 7.4 units (5.5) | 13 % |

Before, a monster moved every other frame (ENTS at 20 a second, frames
at 30); now it moves every frame. The other player's steps still vary:
the server moves a link player when its commands come (30 packets a
second), not at a steady rate, and the PC sees the same updates.

- **Cost.** Placing the entities (`lerp_things`, `move_models`, the clock)
  takes 0.35 ms a frame with 48 things (worst 0.9 ms). Blending the baked
  frames adds 0.76 to 0.80 ms of drawing with 40 monsters about (blending
  on and off in turn every second of one run, so the view is the same),
  0.26 ms on c2a4e. On c2a4e at the spot of the earlier measurements a
  frame took 31.5 ms against 31.1 ms with `a5b9769` (29 fps both; 1803
  and 1822 polygons).
- **RAM.** The lowest free memory on c2a4e (while the map loads) went
  from 68 KB to 64 KB: the code. On `coop_arena` it went from 2.863 MB
  to 2.859 MB.
- **Traffic.** `ENTS` grew from 849 to 862 bytes (18 a second).

Not measured: brush models moving (none of the maps here opened a door
for the rig), a DS.

## Weapons: firing as the PC client does

The PC client predicts its weapon with the movement: after each command,
`ncPlayer::Physics_InputPostMove` runs the attack, reload and idle timers,
the view model's time and the punch, and `ncPlayer::ProcessInput` calls
`ncWeapon`'s attack, reload and idle code (`src/shared/game/Weapon.qc`),
which picks the view model's sequence; the view model's studio events
(5001, 5011, 5021, 5031) make the muzzle flash as the frames pass
(`ncRenderableEntity::HandleAnimEvent`, `env_muzzleflash`, fading in 62.5 ms);
the camera adds the punch (`ncView::AddPunchAngle`). `weapon_nds.c` is that
code in C, with the decl's values from the server (`WEAPONINFO`,
`docs/spec/link.md`, "Weapon prediction"), run on each command after
`pmove_nds.c`, and run again with it from `PSTATE` when the server's state
differs. So:

- the fire sequence and the flash start in the frame the button goes down,
  the HUD's clip drops in that frame, and the view kicks by the fireInfo's
  `punchAngle` (or `punchSpring`, `punchKick`), easing back as the PC's
  does; a hard landing punches as `Physics_Fall` does (`falldamage` decl);
- reloads, bursts, semi-automatic fire, charging and released weapons,
  overheating, alternate modes, timed (shotgun) reloads and the melee test
  (traced through the DS's hulls and swept over the boxes `PSTATE` lists,
  with `Melee_Sweep`'s test that a wall isn't in the way) follow the decl;
  the aim and the knockback use the QC's `anglesToForward` bit for bit (as
  the movement does), and an activity with a list (`act_fire "1,2,3,4,5"`)
  picks among all of it, up to 8 (a longer one leaves that weapon to the
  server);
- the muzzle flash events are found as the PC's `View_DrawViewModel` asks
  `processmodelevents` for them (`src/client/viewmodel.qc:213-218`): from
  the last frame's time, whichever sequence that was, and from 0 only when
  the time went back;
- the HUD's clip (below, "The HUD") is the predicted one: it drops in the
  frame the button goes down.

The clip a reload fills is the server's (`_ReloadFinished` is a think), so
the HUD shows it one round trip after the reload animation ends; a weapon
switch, the ammo a pickup gives and projectiles are the server's too. With
`predict=0`, or before the WEAPONINFO for the weapon in hand has come, the
view model plays the server's sequence (`STATS`) and the flash comes with
the server's impact, as before.

The view model on `coop_arena` "restarting" about 15 times a second is the
game's: the JTF PDW's idle (`act_idle 6`) is 2 frames at 16 fps, 62.5 ms,
and `ncWeapon::Idle` starts it again whenever it ends (Weapon.qc:1833), on
the PC too; its pose doesn't change. Gravity has nothing to do with it.
`STATS` read that sequence (an integer field) as a float and always sent 0;
fixed.

### Measured: weapons, in melonDS 1.1 (not hardware)

`coop_arena` with `bulwark-ded` on the same machine (no added latency),
`fps=30`, the JTF PDW (burst of 3, `fireRate` 0.06, `burstTime` 0.3),
presses from `xdotool` (held 2 s, five taps, a reload, held 1.5 s), a
trace build printing each frame and command:

| | |
|---|---|
| frames where the press began showing the fire sequence and the flash | 7 of 7 (observed) |
| frames until `STATS` showed it (what the DS drew before) | 2 to 4 refreshes (observed, local server) |
| commands whose first prediction matched the server's (sequence, anim time and next attack within 1 ms, clip, state) | 2067 of 2078, 236 firing (observed); the 11: 2 a reload's clip filled by the server, 9 a command the server's budget ran shorter |
| `tools/nds/weptest` on that log, each command from the server's state | 2051 of 2051 identical (observed) |
| PSTATEs replayed / compared (a build printing only a line a second, same presses) | 13 / 994 (23 / 1020 without weapon prediction) |
| prediction per frame, average / worst (that build) | 2.47 / 3.57 ms (2.38 / 3.28 ms without; one run each, different monster waves) |
| frame rate | 30 fps while firing, both |
| code and data | +22 KB code, +11.5 KB bss; lowest free heap 3.14 MB (3.16 MB without) |

Not exercised on this map: a landing hard enough to punch (the arena's
jumps land at about 260 units a second, under `minLandDistance` 400),
melee weapons, charged and released weapons, alternate modes.

### Measured: all of it together, in melonDS 1.1 (not hardware)

The build with movement prediction made faster, other entities drawn
between updates and the weapon predicted, against `a5b9769` (movement
prediction only), each with its own `bulwark-ded`. `coop_arena` with 40
monsters (`coop_waveBase 40`, `coop_wavePerWave 0`, `coop_spawnInterval
0.2`), `botwalk=1` walking, jumping and crouching, and fire held 1.5 s of
every 2 s (a measurement build's `botfire=1`); `e0m1` and `dev` (`-game
base`) walking; `c2a4e` standing at `-576 1012 84` (no hulls, so no
prediction). 30 s each, `fps=30`, `link_god 1`. One run of each: the
monsters wander, so the arena runs saw different scenes.

| Run | Build | fps, median (worst second) | CPU a frame, median (worst second) | Prediction a frame, average (worst) | `PM_Run` a command, average (worst) | States within 0.5 units | Error, units: average (worst) | RAM in use / peak / lowest free | Received / sent |
|---|---|---|---|---|---|---|---|---|---|
| `coop_arena`, firing | `a5b9769` | 30 (30) | 7.7 ms (14.1) | 3.05 ms (4.4) | 1233 µs (2035) | 881 of 885 | 0.009 (3.0) | 399 / 400 KB / 2.86 MB | 33.3 / 2.3 KB/s |
| `coop_arena`, firing | this one | 30 (30) | 6.9 ms (12.3) | 1.55 ms (2.3) | 483 µs (832) | 889 of 895 | 0.017 (1.6) | 408 / 424 KB / 2.80 MB | 37.4 / 2.5 KB/s |
| the same, 150 ms | `a5b9769` | 30 (30) | 7.8 ms (10.9) | 3.40 ms (19.3) | 1211 µs (2129) | 691 of 705 | 0.029 (2.3) | 399 / 418 KB / 2.86 MB | 31.4 / 4.8 KB/s |
| the same, 150 ms | this one | 30 (30) | 7.6 ms (10.9) | 1.73 ms (9.4) | 490 µs (972) | 680 of 690 | 0.017 (1.1) | 408 / 424 KB / 2.82 MB | 41.8 / 5.0 KB/s |
| `e0m1` | `a5b9769` | 30 (30) | 6.9 ms (10.4) | 3.90 ms (7.8) | 1867 µs (4342) | 787 of 787 | 0 (0) | 586 / 587 KB / 1.77 MB | 16.4 / 2.4 KB/s |
| `e0m1` | this one | 30 (30) | 4.4 ms (7.0) | 1.35 ms (2.4) | 548 µs (1319) | 777 of 777 | 0 (0) | 600 / 600 KB / 1.73 MB | 19.7 / 2.6 KB/s |
| `dev` | `a5b9769` | 30 (30) | 4.6 ms (6.0) | 2.90 ms (4.1) | 1387 µs (2211) | 900 of 900 | 0 (0) | 302 / 303 KB / 2.45 MB | 16.1 / 2.3 KB/s |
| `dev` | this one | 30 (30) | 3.1 ms (4.1) | 1.21 ms (1.9) | 485 µs (1040) | 900 of 900 | 0 (0) | 311 / 312 KB / 2.41 MB | 19.4 / 2.5 KB/s |
| `c2a4e`, standing | `a5b9769` | 29 (1, a load) | 13.1 ms | 0.04 ms | – | – | – | 1867 / 1886 KB / 68 KB | 29.8 / 2.4 KB/s |
| `c2a4e`, standing | this one | 29 (1, a load) | 14.3 ms | 0.17 ms | – | – | – | 1877 / 1895 KB / 32 KB | 36.2 / 2.5 KB/s |

"Prediction" is the movement and the weapon together, the server's state
included. On `e0m1` and `dev` every state matched bit for bit (777 and
900 of them); on `coop_arena` the few that differ are the commands the
server cut short (see above). `tools/nds/weptest` on the server's logs of
these runs: 2248 commands while firing at 0 ms and 2316 at 150 ms, 2388 on
`e0m1`, 2482 on `dev`, each from the server's state, 0 differences.
`pmtest` on all 27 reference logs: bit for bit, as before.

- **Traffic.** `ENTS` is about 100 bytes bigger with 40 monsters about
  (885 against 779 bytes, 18 a second: `frame1time` and the players'
  velocity), `PSTATE` about 90 (542 against 450 bytes, 30 a second: the
  weapon block).
- **RAM.** Static data is 14.5 KB more (the weapon state 9.7 KB, the
  trace cache 4 KB) and code 40 KB more. The lowest free memory on c2a4e,
  while the map loads, went from 68 KB to 32 KB: it still fits, but with
  half the margin. The review fixes (below) moved the weapon tables and
  the trace cache to the heap after the map loads: 48 KB.
- **c2a4e** costs 1.2 ms a frame more (drawing 0.3 ms: blended frames;
  the rest placing entities between updates), still 29 fps. Its worst
  second is the models loading after the teleport, in both builds.

The weapon prediction first replayed 10 times a second on `e0m1` and
`dev`: their view models are Quake `.mdl`s with frame groups, which
`frameduration` times but `WEAPONINFO` didn't carry, so the DS timed the
idle at 10 s against the server's 0.1. `WEAPONINFO` now carries them
(`docs/spec/link.md`); the table is after that fix.

### After the review fixes, measured in melonDS 1.1 (not hardware)

The same runs with the fixes (WEAPONINFO, the weapon tables and the trace
cache on the heap, the melee wall test and `anglesToForward`, the replay
budget, things and brush models starting again as the PC's entities do),
against the build above, one run each, each with its own server:

| Run | Build | fps, median (worst second) | CPU a frame, median (worst second) | Prediction a frame, average (worst) | `PM_Run`, average (worst) | States within 0.5 units | Error, units: average (worst) | Replays (commands) | Lowest free |
|---|---|---|---|---|---|---|---|---|---|
| `coop_arena`, firing | before | 30 (30) | 6.9 ms (12.3) | 1.55 ms (2.3) | 483 µs (832) | 889 of 895 | 0.017 (1.6) | 86 (0) | 2.80 MB |
| `coop_arena`, firing | fixed | 30 (30) | 7.7 ms (12.2) | 1.69 ms (2.7) | 530 µs (1094) | 896 of 899 | 0.010 (1.0) | 80 (0) | 2.82 MB |
| the same, 150 ms | before | 30 (30) | 7.6 ms (10.9) | 1.73 ms (9.4) | 490 µs (972) | 680 of 690 | 0.017 (1.1) | 45 (384) | 2.82 MB |
| the same, 150 ms | fixed | 30 (30) | 7.4 ms (13.1) | 1.65 ms (7.8) | 476 µs (1056) | 700 of 713 | 0.026 (3.3) | 48 (384) | 2.82 MB |
| the same, 400 ms | before | 30 (30) | 7.9 ms (14.0) | 2.42 ms (14.1) | 479 µs (1191) | 760 of 771 | 0.022 (2.4) | 60 (1544) | 2.80 MB |
| the same, 400 ms | fixed | 30 (29) | 8.3 ms (16.2) | 2.67 ms (10.6) | 483 µs (1266) | 850 of 861 | 0.020 (2.7) | 71 (1863) | 2.82 MB |
| `c2a4e`, standing (1805 polygons) | before | 28 (1, a load) | 31.8 ms | – | – | – | – | – | 32 KB |
| `c2a4e`, standing (1805 polygons) | fixed | 28 (1, a load) | 31.9 ms | – | – | – | – | – | 48 KB |

The 8 ms replay budget held the worst frame's prediction at 400 ms to
10.6 ms (14.1 before); one frame of that run took three refreshes. The
server acknowledged `WEAPONINFO` once per run (`weapon 1/1`), and
`weptest` and `pmtest` on these runs' logs found 0 differences. The two
`c2a4e` runs were made one after the other on the same server; the scene
(the view after the teleport) varies between runs, so compare them only
with each other. Not run: a moving lift or door, a monster idling past
600 s, the gauntlets or machine pistols, a model baked with clip laps.

Screenshots of the firing run (kept outside git, as they show the game's
data) show the flash and the clip going down while fire is held, and the
monsters moving from frame to frame.

## The HUD: the co-op HUD as the PC draws it

The PC's co-op HUD is a Panorama layout (`coop/panorama/layout/coop/hud.xml`,
`scripts/coop/hud.js`, `styles/coop/hud.css`) that `coop/src/hud/jtf.qc`
feeds, under the Half-Life crosshair (`Jtf_DrawFeedback`) and over
Nuclide's nameplates and target ID (`src/client/nameplates.qc`).
`hud_nds.c` draws the same parts on the top screen's text layer (BG1,
4bpp, 16 colours), and the level and abilities on the bottom screen's top
rows, from `HUDINFO` (`docs/spec/link.md`) and what the device predicts
itself (health, the clip and reserve):

| Part | Where (the PC's layout, 900 units high, scaled to 192 lines) |
|---|---|
| the objective panel: wave, monsters left (red while fighting), score, points, in TallPixelFont | top left (below the diagnostics only when they reach over it) |
| the feed: obituaries (players only, as `HUD_Obituary`) and `coop_feed`'s team events, 5 lines for 6 s, framed when they name you | top right |
| the notice (`coop_notice`, split at `": "` or `"! "`), 3 s | centred at y 140, below the feed's lines here |
| health with the ghost 8s and the hero's Barrier (TallPixelFont at 3x), the Cadence chip and the three nearest teammates' cards | bottom left |
| the clip (red when low, TallPixelFont at 3x), the reserve (2x), the weapon's title, the hero's two slots for 2 s after a switch | bottom right |
| the XPM level, rank and medals in the experience gauge, the weapon in hand | the bottom screen, rows 0-10, full width |
| the ability squares (T 1 2 ENG EX), Energy with Engage's tick (or armour) | the bottom screen, rows 14-28, left of the GAME tab |
| the respawn box and the revive bar while down (only these and the objective panel are shown then, as hud.css's `.hud.down`) | centred at y 540 |
| the use prompt with its key, or a callout with a leader line on the item | y 510, or at the item |
| the target ID card (the ray against the box grown by 4, then a trace) | y 558 |
| nameplates over teammates and friendly NPCs in sight (875 units, the range where their alpha is over half) | over each |
| the weapon's crosshair sprite, scaled by 192/480 as the PC's 480-line HUD | centre, hidden while down |

Every frame each part writes a list of fills, lines and text runs; the
lists are compared op by op with the last frame's, and only the 8 × 8
tiles under what changed are drawn again: in a 1 KB row of tiles in RAM,
then copied to VRAM. The panels that change only with HUDINFO, the health
or the weapon (the objective, vitals and weapon panels) are rebuilt only
then. At most 112 tiles are drawn in a frame; a bigger change (going down,
getting up) finishes in the next frame or two. Nothing is allocated: the
lists (2 × 3.4 KB), the HUD's state (6.3 KB), the row (1 KB) and the tiles'
bookkeeping are static. The bottom screen's part is one more list
(2 × 640 bytes) drawn the same way through the row into the RGB15 bitmap
(each pixel through the HUD's palette), only where it changed, and in full
when the pad is drawn again (after the GAME page).

Deviations from the PC, all from the screen's size: the HUD font is 5
pixels high (04b09), so text is larger next to the screen than the PC's;
the XPM row, the abilities and Energy are on the bottom screen; the notice moves
below the feed's lines when the feed is up; fades are steps (nameplates
shown while their alpha is over half, the target ID for half its 0.5 s
linger, the notice without its 0.4 s fade); a nameplate's line of sight is
traced every third frame instead of each. The crosshair takes the sprite's
most common colour and the pixels its format draws.

The damage arrows and tint and the pickup notices come as `EVENT` records
and are drawn by `events_nds.c` ("Events" below). Not drawn yet: the ammo
pickup notice and the weapon selection list.

### Measured: the HUD, in melonDS 1.1 (not hardware)

`coop_arena`, `bulwark-ded` on the same machine, the DS and a PC client
(PCrig) in the game with Gina, monsters fighting, `fps=30`, `diag=1`, the
feed full and a notice up, 85 s alive and down; a build that times
`Hud_Frame` with `tickGetCount()` and prints each second:

| | before (03c3cef, drawing to VRAM) | now (9db9469) |
|---|---|---|
| `Hud_Frame`, a second's mean | 5 to 12 ms | median 1.7 ms, 90th percentile 2.1 ms, worst 2.6 ms |
| `Hud_Frame`, a second's worst frame | about 16 ms | median 3.5 ms, 90th percentile 6.7 ms, worst 11.2 ms (going down) |
| frame rate | 30 | 30 in 84 of 85 seconds |
| heap in use | | 514.7 to 517.6 KB (none of it the HUD's) |

Against 5730913 (the old HUD: the clip, health and the diagnostics line),
`arm-none-eabi-size` gives text 439868 bytes (+25304) and bss 192912
(+17888).

## Events: what the server shows, as the PC client shows it

Prints, chat, centerprints and the game's CSQC events reach the DS as
`EVENT` records (docs/spec/link.md, "Events"), in order and resent until
acknowledged. `events_nds.c` shows them with the client code the PC runs
for co-op, ported to C:

| What | The PC's code | On the DS |
|---|---|---|
| View shake (`env_shake`, sound shakes) | `src/client/shake.qc` `Shake_Start`, `Shake_Update` | the same offset and roll on the camera, the same decay |
| Fade (`env_fade`) | `src/client/fade.qc` `Fade_Parse`, `Fade_Update` | a translucent polygon over the 3D view, under the HUD (View.qc draws it before the HUD) |
| Damage | `src/client/damage.qc`, `coop/src/hud/jtf.qc` `Jtf_DrawFeedback`, `valve/src/hud/hud.h` `cg_damageFill 0.25` | an arrow on each side the hit lines up with (more than 0.25), as strong as it lines up, fading over a second; the tint over the view; both gone when the player dies (View.qc) |
| Centerprint | `src/client/entry.qc` `CSQC_Parse_CenterPrint`, `prints.qc` `Print_DrawCenterprint` | orange (`^xF80`), 3 s, then a second's fade, centred from the middle - 69 |
| Prints | `entry.qc` `CSQC_Parse_Print`, `prints.qc` `Print_Draw` | five lines at the top left; every 5 s the newest goes, as `Print_Draw` does |
| Chat, hints | `chat.qc` `Chat_Parse`, `Chat_Draw`; `event.qc` `EV_CHAT`, `EV_HUDHint` | five lines, wrapped at half the screen, bottom-aligned in the field from the middle - 80; lines go 5 s apart and the block as it was made stays until the last |
| game_text, the co-op hero and XPM lines, env_message | `text.qc` `GameText_ParseString`, `GameText_DrawMessage`, `GameMessage_Setup` | channels 0-4 and channel 0's queue, placed by x and y as `GameText_DrawString` places them, faded in, held and out, the typing effect letter by letter |
| Kill feed | `jtf.qc` `HUD_Obituary`, serverinfo `coop_feed`, `coop/panorama/scripts/coop/hud.js` | five lines at the top right, 6 s each, newest on top: attacker blue (#3396d4), weapon grey, victim |
| Pickups | `valve/src/hud/hud.qc` `HUD_PickedUpItem`, `HLHUD_ItemNotify_Draw` | battery, health kit and long jump only, with a count, 2.5 s and fading 0.5 a second |
| Punch (landing, recoil) | `player_pmove.qc` `Physics_Fall`, `Physics_InputPostMove`; `View.qc` `AddPunchAngle` | already predicted with the weapon ("Weapons" above): the server's punch fields reconcile it |

Text goes on its own layer, BG2, in a band from row 24 to 151, between
the frame-rate lines and health and ammo. BG2 is in front of the HUD
(BG1), as `entry.qc CSQC_Update2D` draws `Chat_Draw`, `Print_Draw` and
`Print_DrawCenterprint` after `HUD_Draw` (game_text, which it draws before
`HUD_Draw`, is in front of the HUD here too); its tiles are in VRAM D,
which the viewer keeps from textures for its two passes and a link session
doesn't use, and its map is after BG1's. Its font is the HUD's. The PC's 2D numbers are
taken as pixels of a 480-row HUD (an assumption: at 1080p Nuclide's integer
`vid_conautoscale` gives 480 to 540 rows), so a PC pixel is 0.4 of a DS
pixel. Each text group has its own palette bank, so its fading is a palette
write a frame. The band is drawn in main RAM in the layer's tile format and
only the rows of cells a changed group touches are copied (drawing straight
into VRAM pixel by pixel took 5 to 11 ms and dropped frames).

Where it differs from the PC:

- The PC's text fades to nothing. On the DS, when every text group up has
  the same alpha (to 1/16) the layers blend at that alpha (`REG_BLDALPHA`)
  and look as the PC's; otherwise each group's letters darken into their
  black outline and then go.
- The fade's modulate style and the damage tint multiply the scene on the
  PC; the DS can only blend toward a colour: the modulate fade dims, the
  tint blends toward a dark red at the same strength (0.25 at the hit).
- The fade's colour comes as 0-255 and the PC's `drawfill` gets it as it
  is; FTE clamps it, so any channel above 0 is full. The DS does the same
  (from the source; not compared on a PC).
- The damage arrows are trapezoids in an orange-red, not Valve's
  `640_pain` sprite; pickups are words, not the HUD sprite's icons.
- game_text is on BG3, behind the HUD, as `CSQC_Update2D` draws it before
  `HUD_Draw`; its second pass (effects 1 and 2, `text.qc
  GameText_DrawMessage`) is drawn in colour 2 over the first. env_message
  and worldspawn's `chaptertitle` are resolved in the game's titles.txt on
  the server (`text.qc GameText_ParseString`, `titles.qc`) and sent as
  game_text; `startdark` sends `fade.qc`'s fade from black.
- Colour codes are kept: `^0`-`^9` take the engine's `qcolors`
  (`src/render/r_draw.c`), `^xRGB` the nearest of those ten (a palette bank
  has no room for more), `^d` the text's own colour; links and styles are
  skipped. Valve's `\y \r \g \b \w \d` become `^3 ^1 ^2 ^4 ^7 ^8`.
- The pickup flash (`cl_cmds.c BonusFlash_f`, sent for an item def with
  `pickupFlash 1`) is the engine's gold at 50/255, gone at 100/255 a second.
- Shakes and fades, unreliable on the PC, come reliably: a lost one comes
  0.2 s late instead of not at all.
- Prints start under the frame-rate lines (row 24) rather than at the PC's
  16 pixels from the top.
- Since the HUD (above) is in, the device's first `'H'` sets its `phud 1`
  as `jtf.qc Jtf_SetPhud` does, so the co-op rules send it what they send a
  PC on the Panorama HUD: the hero and XPM lines no longer come as
  game_text (observed: 2 records in the 40 s arena run, before the first
  `'H'`). The feed is the HUD's; `EVENT` makes no feed records for it.
- Prints at the top left lie over the objective panel, and the chat field
  over the vitals' top (observed); the PC places them the same way, at its
  own scale.
- Not drawn yet: the HUD's damage-type icons (`valve/src/hud/hud.qc
  HLHUD_DamageNotifyDraw`), and pickup notices for ammo and weapons (only
  battery, health kit and long jump have words).

### Measured: events, in melonDS 1.1 (not hardware)

`fps=30`, coop_arena, standing at 160 160 40 facing 45 degrees with
`link_god 1`, 40 s each, the rig's measurement build (MEAS lines). "Triggered":
every 5 s a chat line, a two-line game_text, a fade and a shake.

| Run | fps (avg / lowest second) | CPU a frame, avg / worst | text: frame / redraw, worst | EVENT |
|---|---|---|---|---|
| before (5730913) | 30.0 / 30 | 14.3 / 16.1 ms | - | - |
| events, idle | 30.0 / 30 | 14.7 / 16.9 ms | 0.8 ms / none | 30 B/s |
| events, triggered, text into VRAM | 29.6 / 20 | 15.3 / 45.2 ms | - | 51 B/s |
| events, triggered, text in RAM | 30.0 / 30 | 12.8 / 18.7 ms | 0.3 / 4.2 ms | 50 B/s |

The polygon count differs between runs (833 to 1114: monsters move), so
the CPU numbers are within that spread. The ROM's code grew by 13.6 KB and
its static data by 24 KB (the band, 16 KB); the lowest free memory seen
went from 2.96 to 2.92 MB. All 163 records of the triggered run were
acknowledged, none dropped. `tools/nds/linktest_events.py` (a host client
on the same server) checks the delivery: 20 chat lines arrive once and in
order; with the first 3 EVENT packets ignored they come again and still
arrive once and in order; with nothing acknowledged during 80 lines the
next EVENT starts 32 records past it and the last 52 lines arrive. Screenshots (fade, game_text and its fade,
shake, damage arrow and tint, chat, centerprint, the feed) were taken with
the rig's `evtest.sh`; they show Valve's data and stay out of the repo.

### Measured: the HUD, events and the draw loop together, in melonDS 1.1 (not hardware)

The three pieces above (HUD, events, the draw loop on c2a4e) merged, with
their own `bulwark-ded`, against `5730913` with its own. Measurement builds
(`rig/make_meas_src.py`), `fps=30`, one run of each, observed:

- `coop_arena`, 40 monsters, `botwalk=1 botfire=1`, 40 s: `link_god 1` for
  the first 8 s, then a small explosion (damage), a red `env_fade`, an
  `env_shake`, a two-line game_text, a `say`, and an explosion that kills
  (the down box, "Respawn in N seconds", the respawn).
- The same with 150 ms of round trip (`sv_link_lag 75`), 30 s, god on.
- `c2a4e` at `-576 1012 84` facing 270 and `c1a0` at its start, standing,
  30 s (no hulls, so no prediction). c1a2 has no converted map in the rig,
  so it is not measured.
- `e0m1` (`-game base`), `botwalk=1`, 30 s.

| Run | Build | fps, median (worst second) | CPU a frame, median (worst second) | `Hud_Frame`, median of the seconds' means (worst frame) | Prediction a frame, average (worst) | States within 0.5 units | RAM in use / peak / lowest free | Received / sent |
|---|---|---|---|---|---|---|---|---|
| `coop_arena`, damage and death | 5730913 | 30 (30) | 7.6 ms (19.4) | – | 1.45 ms (2.9) | 1038 of 1050 | 421 / 438 KB / 2.82 MB | 40.5 / 2.46 KB/s |
| `coop_arena`, damage and death | merged | 30 (30) | 6.5 ms (14.7) | 0.77 ms (8.3) | 1.43 ms (3.0) | 1040 of 1049 | 415 / 433 KB / 2.77 MB | 39.9 / 2.64 KB/s |
| the same, 150 ms | 5730913 | 30 (30) | 7.3 ms (10.8) | – | 1.66 ms (8.1) | 698 of 712 | 421 / 439 KB / 2.82 MB | 38.4 / 4.98 KB/s |
| the same, 150 ms | merged | 30 (30) | 6.3 ms (8.8) | 0.60 ms (2.7) | 1.59 ms (7.9) | 683 of 695 | 415 / 429 KB / 2.75 MB | 38.5 / 5.16 KB/s |
| `c2a4e`, standing | 5730913 | 28 (27; 1 while the models load) | 31.9 ms | – | 0.18 ms | – | 1885 / 1906 KB / 48 KB | 34.2 / 2.43 KB/s |
| `c2a4e`, standing | merged | 30 (29; 1 while the models load) | 13.1 ms | 0.56 ms (3.5) | 0.18 ms | – | 1877 / 1900 KB / 192 KB | 34.7 / 2.70 KB/s |
| `c1a0`, standing | 5730913 | 29 (20) | 15.3 ms (16.4) | – | 0.17 ms | – | 946 / 964 KB / 1.57 MB | 28.6 / 2.46 KB/s |
| `c1a0`, standing | merged | 30 (30) | 10.4 ms (11.0) | 0.76 ms (4.1) | 0.17 ms | – | 942 / 962 KB / 1.49 MB | 28.1 / 2.64 KB/s |
| `e0m1`, walking | 5730913 | 30 (30) | 5.2 ms (7.2) | – | 1.37 ms (2.9) | 824 of 824 | 613 / 614 KB / 1.75 MB | 19.7 / 2.52 KB/s |
| `e0m1`, walking | merged | 30 (30) | 4.4 ms (6.2) | 0.24 ms (2.0) | 1.38 ms (2.9) | 877 of 877 | 612 / 614 KB / 1.67 MB | 20.6 / 2.67 KB/s |

"CPU a frame" includes `Hud_Frame` in the merged build; 5730913's old HUD
redrew its diagnostics line once a second (`hud_diag`, 7.2 ms on the arena,
15.5 ms on c2a4e, inside the worst seconds above). The worst `Hud_Frame`
frame on the arena (8.3 ms) is the down box coming up. Sent traffic is 0.15
to 0.2 KB/s more (INPUT's `'H'` and `'E'`). On the arena run `EVENT` was
185 B/s and `HUDINFO` 321 B/s; 196 records were made and all acknowledged,
most of them "Respawn in N seconds": `coop.qc
CodeCallback_PlayerRequestRespawn` centerprints it each frame fire is held
while down (the PC gets the same). The normal ROM is 457060 bytes of code
and 215604 of bss against 414564 and 175024 (+42.5 KB, +40.6 KB).

Screenshots of these runs (HUD, nameplates and feed, damage arrows and
tint, fade, shake, game_text, chat, the centerprint over the down box, and
the crosshair with the HUD worktree's synthetic sprite: no Valve sprite
sheets are in the rig, so the PC draws no crosshair there either) are kept
out of the repository: they show Valve's data.

### After the third review's fixes, measured in melonDS 1.1 (not hardware)

What changed: `EVENT` sends only new records (the window again when the
oldest waited 0.2 s) and folds "Respawn in N seconds" repeats into one
`CENTERAGAIN`; `HUDINFO` stays under its body limit and is taken whole or
not at all; the HUD's and the text's per-frame work is limited by time
(2.5 ms and 2 ms, checked between runs of tiles and between batches of two
rows of cells, at least one a frame); the glyph drawing runs in ITCM as ARM
code; text tiles and palettes go to VRAM right after the wait for the blank.

Text layers: BG2 (tiles in VRAM D, map base 13, in front of the HUD) for
prints, chat, centerprints, the feed and pickups; BG3 (tiles in VRAM D after
BG2's, map base 14, priority 1, behind the HUD) for game_text. Both
use bank D, so the viewer's two-pass mode (which takes D for textures) and
event text cannot run together; a link session has no two passes.

Same runs as above (`rig/make_meas_src.py` builds of d53e009 and its
`bulwark-ded`), one each, observed. The arena run adds a chat flood of 10
lines, two game_text channels with effect 1, and dies.

| Run | fps, median (worst second) | CPU a frame, median (worst second) | `Hud_Frame` worst frame | Text redraw worst frame | States within 0.5 units | RAM peak / lowest free | `EVENT` / `HUDINFO` |
|---|---|---|---|---|---|---|---|
| `coop_arena`, 40 firing, damage and death | 30 (30) | 7.3 ms (13.4) | 4.9 ms (8.3 before) | 4.7 ms (15.0 before the time limit) | 1029 of 1036 | 443 KB / 2.84 MB | 65 / 168 B/s (185 / 321 before) |
| the same, 150 ms | 30 (30) | 7.4 ms (10.5) | 3.4 ms | 4.7 ms | 581 of 583 | 438 KB / 2.84 MB | 90 / 165 B/s |
| `c2a4e`, standing | 30 (1 while the models load) | 13.6 ms | 3.6 ms | 1.4 ms | – | 1.90 MB / 160 KB | – |
| `c1a0`, standing | 30 (30) | 11.0 ms (11.7) | 3.9 ms | 1.5 ms | – | 983 KB / 1.53 MB | – |
| `e0m1`, walking (62c2ce3's parent build) | 30 (30) | 4.5 ms (6.5) | 1.9 ms | 2.6 ms | 851 of 851 | 629 KB / 1.72 MB | 5 B/s / – |

The text redraw's worst frames are on the death screen, with the chat
flood over the respawn countdown: one batch can take more than the 2 ms
(at least one is drawn a frame), and those frames take 3 to 7 ms in all, so
no second dropped. The lowest free RAM on c2a4e went from 192 to 160 KB
(the second text band and `HUDINFO`'s staging copy, about 22 KB of bss).
In the arena run all 117 records were acknowledged, none dropped, no
stops; c1a0's chapter title (`C0A1TITLE`) came from titles.txt.

## Sound: what the PC client hears

`sound_nds.c` plays what a PC client in the same game would hear, on the
DS's 16 hardware channels. The server sends the sounds (docs/spec/link.md,
"Sound"): `SOUND` packets for the engine's `svc_sound` and for what the PC's
client game makes by itself, `AMBIENT` for client-side loops, `EVENT`
records 12 to 15 for loops started or stopped reliably, sentences, music and
the precache list, and a table in `WEAPONINFO` for the view model's sound
events. The DS mixes them as the PC's mixer does (`src/sound/snd_main.c`):

| What | The PC's code | On the DS |
|---|---|---|
| Voices | `pick_channel` (`snd_main.c:155-182`), 128 channels less 32 ambient | 96 voices: the entity's numbered channel is replaced (`NOREPLACE`: the new one is dropped), else a free voice, else the one furthest into its sample that isn't the listener's |
| Gains | `spatialise`, `S_SoundLevel`, `S_DistanceGain` | the same in fixed point (`sndmath_nds.h`): distance in 1/128 units with the hardware square root, the pan from the camera's right vector, `volume` 0.7, the cull at 0.002 |
| Left and right | the PC's two gains | one volume and a pan: V = (L + R)/2 and P = 128R/(L + R), which gives each ear exactly L/2 and R/2 (6 dB of headroom) |
| `FOLLOW` | the entity's origin each frame | the drawn thing's origin, or the camera for our own player |
| Loops | `ambient_generic` and `ncSound` (`ambient_generic.qc`, `Sound.qc`) | the `AMBIENT` set; a switch of sample waits until the new one is in |
| Sentences | `sentences.qc` | each word after the previous one's length |
| View model | `HandleAnimEvent` 1004, 1008, 5004 | `WEAPONINFO`'s table (SoundDefs with their pitch range, distance shader and start offset) |
| Weapon switch | `HLWeaponSelect` | next: `common/wpn_moveselect` (the PC's `wpn_hudon` on a wrap round to the first weapon isn't known on the DS); previous: `wpn_hudon` when the list was closed, else `wpn_moveselect`; `wpn_hudoff` when the list closes, 3 s after the last press (the PC's `Trigger` or time-out). `wpn_select` has no caller in Nuclide |
| Chat | `chat.qc` | `misc/talk` |
| Model events with `AT` | as the client draws the frame | started when the drawn clock (`ENTS` time less `interp`) passes `at` |

Samples are named by key (FNV-1a 32 of the normalised name) and come from
sound packs, `<base>/sound/*.dsp` (made by `tools/nds/sndpack.py`, "Sound
packs" above), searched last name first like game folders.
A pack's directory is read a 512-byte page at a time. Samples are kept in one
cache, `sndcache=` KB (192 by default, less if the heap would keep under
384 KB for the models still to load), filled after each map loads and freed
before the next. A sample is read whole into the cache, 4 KB a read, within
`sndreadms=` a frame (2 ms by default): a read goes only while the time
spent and the slowest read lately still fit, and a frame always has one
read for what a voice waits for. A one-shot starts once its first 2 KB are
in (if it then plays past what is loaded, it stops: `underrun`), a loop once
all of it is. One-shots come first, in the order they came, then loops,
loudest first. After a map loads the DS looks up the map's precache list
(and the names it keeps itself, `PRECACHE`'s 0xf000 record) and reads the
smallest samples first, until 75% of the cache is full; in play only in a
frame that has read nothing else, one read. A frame over 100 ms (the
models load after the map at one or two frames a second) gets an eighth of
its length for reads, up to 60 ms, so most of the list is in before play.
Samples that voices play, and loops, can take up to half the cache. The
384 KB left for the models is a guess no run has tested: every map tried
had over 1.2 MB free after its models.

The 16 channels go to the loudest voices. A new one-shot takes a free
channel, or the quietest one if it is louder. A loop that loses its channel,
or gets quieter than the cull, gives it up and takes one again when it is a
quarter louder than the quietest held. Volume and pan are only rewritten on a
change of 1/16 or 2 steps of pan, at most 20 commands a frame.

Where it differs from the PC:

- One volume and a pan per channel, not two gains. Each ear gets exactly the
  PC's gain less 6 dB, so 16 loud voices don't clip.
- 16 voices are heard, not up to 96. A one-shot quieter than every held
  channel is dropped (`quiet`), and one that loses its channel ends. The PC
  keeps mixing both, under louder ones (coop_arena under fire: about 20 such
  in 40 s, observed).
- Sounds come at most every 1/36 s (`SOUND` waits for every second server
  frame at 72 Hz), so a sound starts up to ~28 ms later than on the PC;
  within a packet each starts as long after the first as it was made (the
  DELAY bits, 4 ms steps), so one that the next frame replaces is heard for
  a frame, as on the PC.
- A sound whose sample is in no pack replaces nothing (as on the PC, which
  can't load it); while its sample is still being looked up, the sound on
  its channel plays on until the new one starts.
- A loop that gets a channel back starts again from its loop point (PCM8
  and PCM16), not where it would be. An IMA-ADPCM loop starts again from
  the sample's start, intro and all: the hardware takes ADPCM's decoder
  state only from the header word at the start address, which exists only
  at the sample's start. A held loop's channel goes only to a sound a
  quarter louder, as a loop takes one back.
- A one-shot whose start isn't read within 150 ms is dropped (`late`).
- When looped samples would take more than half the cache, the quieter
  loops wait. On c2a4e with all five ambients aliased to base samples,
  two at -48 dB stayed silent (observed).
- A sentence word on an entity the DS doesn't draw plays at the listener.
- Positive start offsets of a SoundDef (skipping into the sample) are not
  done; negative ones (a delay) are.

Not done yet: music and other streams (EVENT 14 is logged only, and
samples over half the cache are not played), the fireInfo's release sound
(`WEAPONINFO`'s `release` entries).

`tools/nds/sndtest.c` checks the fixed-point gains against the PC's own
`spatialise` (cut out of `snd_main.c` by `tools/nds/sndtest_ref.py`) over
yaws, positions, attenuations (×64 bytes, and ×16 ones up to 15.9 for
what the PC's client game passes as a float: an `ncSound`'s 1000 / radius
is 7.8 at radius 128), volumes and flags: 1,860,934 cases, none more than
1 of 2047 in volume or 1 of 127 in pan; the DS's distance factor table
(`sndfactor_nds.h`, made by `sndtest -table`, so the DS needs no `powf`)
equals the function; the sample-name keys match the spec's test values.

### Measured: sound, in melonDS 1.1 (not hardware)

Test packs made from base's WAVs (PCM8, and ADPCM for `ambience/`) and a few
Half-Life names aliased to them (Half-Life's own sounds aren't here). The
server is the `snd-server` work before the merge (c4a0539), with `sv_link_log 1`. All values are
observed. "CPU" is `Snd_Frame` with its reads, from `SNDSTAT`:

| What | Result |
|---|---|
| Records started | coop_arena, firing for 15 s: all 196 `SOUND` records the server sent were started on the DS (an earlier run: 309 of 309); 10 view-model samples missing from the packs were reported `missing` |
| Start delay | 1 to 6 ms from the packet, at most 13 ms (a cold read) |
| Gains | e0m1's `ambience/comp1` (`AMBIENT`): vol 9, pan 86 on the DS; the PC formula at the player's position and yaw gives 9 and 86. c2a4e's siren and two lab drones: 130/64, 21/94 and 13/97 on the DS and from the formula |
| Audio (SDL disk capture) | a test tone at 27 volume and pan settings in three formats: the right-to-left ratio as computed, level linear in the volume (7.3 to 7.8 times the per-ear gain over vol 71 to 1433), onsets within 7 ms of the log (2 ms rms); comp1 looped steadily for 70 s, its spectrum within 1 dB of the WAV's up to 4 kHz; 11 clipped samples in 69 s of heavy fire |
| CPU a frame | nothing playing 13 to 15 us; one loop 88 us; firing (up to 14 voices) 110 to 190 us on average, worst 320 us; a frame that read went past its 2 ms budget (4.5 to 4.7 ms at the merge: the check came before each read); since the review a read goes only if it fits ("Measured after the review") |
| Reads | 0.3 to 1.2 ms each (melonDS, not a card) |
| RAM, c2a4e standing | used at peak 2.18 MB against 1.94 MB with `sound=0` (the 192 KB cache and about 46 KB of tables); 1.25 MB still free; the lowest free during the map load 124 KB in both, since sound frees everything first |
| Frame time, c2a4e | 13.6 to 13.8 ms against 13.5 ms with `sound=0`, 30 fps in both |
| Code | `sound_nds.o` 27.7 KB of code, 1.7 KB of bss |

### Measured after the merge, against 58a5bba (melonDS 1.1, not hardware)

The three parts together: the server (`bulwark-ded` RelWithDebInfo), the
measurement build of the DS (the rig's `make_meas_src`, which adds the
`MEAS` line each second), and packs made by `sndpack.py` from `games/base`,
`games/coop` and stand-in Half-Life files (`10base`, `40coop`, `30hldata`;
none of them in git). 58a5bba is the ROM and server before sound. Each run
is 30 to 40 s after the join and 10 s settling, the first second left out;
coop_arena has 40 monsters and fire held 1.5 s of every 2 s; c2a4e is in
zero gravity at -576 1012 84 (no hulls on the DS), where four glass
breakables make about 380 sounds a second for the PC as well (`SVSND`).
`snd_log=2` (the stats only) for the timings below. All observed:

| Run | fps median / lowest | CPU ms a frame, median / worst | Sound ms a frame, median / worst | RAM in use / peak / lowest free (KB) | rx KB/s | Sounds started / dropped (quiet, late, missing) |
|---|---|---|---|---|---|---|
| coop_arena, 58a5bba | 60 / 54 | 11.21 / 18.96 | - | 414 / 427 / 2796 | 44.8 | - |
| coop_arena, sound | 59 / 55 | 11.17 / 18.55 | 0.14 / 1.84 | 646 / 656 / 2556 | 43.1 | 639 / 22, 0, 1 |
| coop_arena 150 ms, 58a5bba | 59 / 57 | 11.68 / 17.27 | - | 414 / 431 / 2776 | 42.3 | - |
| coop_arena 150 ms, sound | 60 / 56 | 11.64 / 17.50 | 0.11 / 1.63 | 646 / 657 / 2548 | 45.0 | 421 / 0, 0, 1 |
| c2a4e, 58a5bba | 59 / 57 | 12.62 / 41.94 | - | 1878 / 1886 / 156 | 33.4 | - |
| c2a4e, sound | 57 / 53 | 13.28 / 18.64 | 0.39 / 1.04 | 2109 / 2128 / 124 | 46.2 | 3012 / 0, 0, 0 |
| e0m1, 58a5bba | 60 / 60 | 2.47 / 3.88 | - | 612 / 613 / 1680 | 19.2 | - |
| e0m1, sound | 60 / 60 | 2.60 / 4.67 | 0.08 / 0.26 | 844 / 845 / 1648 | 19.8 | the comp1 ambient, vol 8 pan 93 |

- RAM: the lowest free while c2a4e loads fell from 156 KB to 124 KB at the
  merge. That is the ROM's size, not sound's memory, and `sound=0` doesn't
  bring it back: sound takes its heap only after a map loads and gives it
  back before the next, and the measurement ROM's code and data grew 33 KB
  (text +32,384 bytes, bss +1,376: `sound_nds.o` ~28 KB of code, `powf`
  ~3 KB). Since then `sound_nds.c` is Thumb at `-Os` and `powf` is a table
  (11 KB less; "Measured after the review", below). In use goes up about
  232 KB while a map runs (the 192 KB cache and the tables).
- The cache: with `snd_log=1`, 98 to 99% of the starts in coop_arena found
  their sample already in (12 samples read in 28 s under fire), 94% in
  c2a4e's flood.
- Every record accounted for (`snd_log=1`, each `SND` line matched to the
  server's `LINKSND` by seq): coop_arena 674 records: 538 started, 113
  replaced on their channel before they started, 21 quiet,
  2 missing (`buttons/bell1`, in no source here); at 150 ms 805: 687, 99,
  14 silent, 4 quiet, 1 missing; c2a4e 8789: 2379 started, 6410 replaced.
  None unaccounted for. Starts came 2 ms (median) after the record, at
  most 23 ms. Most of the replaced ones are not what the PC does: `SOUND`
  carried two server frames, and the later frame's record replaced the
  earlier's before it started, where the PC plays the earlier for a frame
  (at most ~1000 of c2a4e's 6410 were in one server frame). Fixed since:
  the DELAY bits, and the server sends only what the PC hears of two in
  one frame ("Measured after the review", below).
- `snd_log=1` costs: each `SND` line goes out of the debug port, so in
  c2a4e's flood the frame took 19.3 ms against 13.3 ms, 33 fps against 57.
  Leave it off except to test.
- An old ROM (58a5bba) on the new server gets no sound records and runs as
  before (coop_arena: 60 fps, 10.77 ms).
- Audio (SDL disk capture, coop_arena firing, 56 s): no clipped samples,
  peak 22612 of 32767; the level, binned at 100 ms, follows the DS's `SND
  start` lines (r = 0.81 at the best offset).

### Measured after the review (melonDS 1.1, not hardware)

The review's fixes (branch `claude/project-thread-sqgyy6` after `af12e5e`):
the DELAY bits and same-frame drop on the server, the loader's budget and
long-frame reads, loops whole before they start, the precache list by
index, the ×16 attenuation, `sound_nds.c` as Thumb `-Os` with the factor
table. The same packs, maps, positions and rig as above; the 58a5bba and
merge rows are the runs above. "First 3 s of play" starts at the first
second at 20 fps or more after the model load; "first fire" is the second
fire starts. Sound packets is `SNDSTAT`'s `pktavg`/`pktmax`, the time in
`Snd_Packet`, `Snd_Event` and `Snd_ViewEvent`, which the merge's
measurement left out. `snd_log=2`. All observed:

| Run | fps median / lowest (or each second) | CPU ms a frame, median / worst | Sound frame ms, median / worst | Sound packets ms a frame, median / worst call | RAM in use / peak / lowest free (KB) | rx KB/s |
|---|---|---|---|---|---|---|
| coop_arena, 58a5bba | 60 / 54 | 11.21 / 18.96 | - | - | 414 / 427 / 2796 | 44.8 |
| coop_arena, merge | 59 / 55 | 11.17 / 18.55 | 0.14 / 1.84 | not measured | 646 / 656 / 2556 | 43.1 |
| coop_arena, now | 60 / 53 | 11.75 / 18.22 | 0.12 / 1.57 | 0.01 / 0.52 | 651 / 653 / 2556 | 42.8 |
| coop_arena first 3 s: 58a5bba, merge, now | 59 60 60; 59 60 60; 60 60 60 | worst 19.55; 21.15; 10.18 | merge worst 4.53, now 0.01 | | | |
| coop_arena first fire: 58a5bba, merge, now | 59; 54; 57 | worst 15.91; 40.71; 43.54 | merge 2.39, now 0.30 | now 0.11 | | |
| c2a4e, 58a5bba | 59 / 57 | 12.62 / 41.94 | - | - | 1878 / 1886 / 156 | 33.4 |
| c2a4e, merge | 57 / 53 | 13.28 / 18.64 | 0.39 / 1.04 | not measured | 2109 / 2128 / 124 | 46.2 |
| c2a4e, now | 56 / 45 | 13.82 / 48.10 | 0.60 / 2.28 | 0.18 / 1.15 | 2189 / 2194 / 132 | 45.7 |
| c2a4e first 3 s: 58a5bba, merge, now | 57 59 59; 39 50 59; 32 55 57 | worst 24.96; 36.25; 424.61 | merge worst 4.63, now 0.93 | now 0.61 | | |

- The first seconds: at the merge most of c2a4e's precache list was read in
  play (4.6 ms sound frames at 39 and 50 fps). Now 645 reads are done in the
  model load's long frames (up to 60 ms each, against 500 to 900 ms frames),
  and play reads 3 more in 30 s; the worst sound frame in the first 3 s is
  0.93 ms. The 32 fps second still holds the load's last frame (424 ms, the
  models, not sound: that second's worst sound frame is under 1 ms).
- c2a4e's glass flood (about 420 records a second) costs more than at the
  merge: the frame's median is 0.54 ms higher, and for 8 s the rate fell to
  45 to 51 fps while the network and packet time rose. The DS now starts
  about twice as many of the flood's sounds (5734 in 30 s against 3012):
  the earlier of two records a frame apart plays for a frame before the
  later replaces it, as on the PC. The 48 ms worst frame is one frame, not
  sound (that second's worst sound frame is 2.3 ms; 58a5bba has a 42 ms
  one). coop_arena's costs are as before.
- The server's same-frame drop: coop_arena 49 of 956 records (impacts and
  weapon sounds on one channel in one frame); c2a4e none (one record per
  entity and frame), so its rx is as at the merge.
- Every record accounted for (`snd_log=1`, `LINKSND` against the DS's `SND`
  lines, `LINKSNDGONE` for what the server dropped): coop_arena 956: 891
  started, 49 dropped by the server (same frame), 15 quiet, 1 missing;
  c2a4e 8777: 3641 started, 5136 replaced before they started (2379 and
  6410 at the merge). With `snd_log=1` the DS runs c2a4e at 31 fps, so two
  frames' records often fall due in one DS frame and the later still
  replaces the earlier there; at 60 fps (`snd_log=2`) the starts are the
  5734 above.
- RAM: the lowest free while c2a4e loads is 132 KB (124 at the merge, 156
  at 58a5bba): the measurement ROM's text is 488,148 bytes (499,268 at the
  merge, 466,884 at 58a5bba), bss 248,852 (247,924; 246,548).
- No `underrun`, `late` or `SNDBUG` in any run; reads are 0 to 1.3 ms in
  melonDS. A real SD card's read time is unknown: one 4 KB read that takes
  longer than `sndreadms` still runs, once a frame.

## Map viewer controls

| Button | Action |
|---|---|
| D-pad up / down | move |
| D-pad left / right | turn |
| L / R | strafe |
| X / B | look up / down |
| A / Y | rise / sink |
| touch and drag | look around |
| START | back to the map list |
| SELECT | steps through one pass, one pass with flicker, one pass capped at 30 fps (with and without flicker), two passes, two passes with flicker |

In the map list, the D-pad and A pick a map, or you can tap a name. The
bottom screen shows the frame rate, faces, GPU vertices and polygons, and
RAM and texture VRAM in use.

## How it fits

All floating-point work happens once, at load time. A frame only uses
integer and fixed-point maths, except the linked player's movement
prediction (soft float, so that it gives the server's results bit for
bit; see "Prediction" above). The full list is at the top of
`src/platform/nds/world_nds.c`.

- **Vertices:** 16-bit, in 1/8 map units. One GPU unit is 512 map units, so
  maps can reach ±4096 units.
- **Textures:**
  - They stay 8-bit with the Quake palette, the DS's 256-colour format.
  - A map's textures have to fit in 256 KB of VRAM (banks A and B): 192 KB
    for the world and 64 KB for models. Banks C and D are kept for two-pass
    drawing, and the bottom-screen console lives in bank H.
    When they don't, the viewer drops to a smaller mip level from the map
    itself.
  - Textures whose names start with `{` see through on index 255.
- **Lighting:** baked into vertex colours from the lightmaps, because the
  GPU blends one texture per polygon. The light is smoother than on the
  desktop, and light styles are added up and don't animate.
- **Culling:** the PVS, a frustum test on node boxes, and a CPU back-face
  test.
- **Faces:** each face is one triangle strip.
- **The GPU's frame limit:** the tree is walked nearest first. When a view
  holds more than the GPU can take, the farthest faces (often the sky) are
  the ones left out.
- **Hot code:** the draw loop runs from ITCM.

## Measured in melonDS 1.1

These figures come from the viewer's bench, a full turn on the spot at
each map's start. Hardware numbers are still to come.

| Map | File | Average / worst fps | Peak GPU vertices | Faces left out |
|---|---|---|---|---|
| `dev.bsp` (Nuclide) | 400 KB | 60 / 60 | 562 | 0 |
| `start.bsp` (LibreQuake) | 500 KB | 60 / 60 | 1105 | 0 |
| `e0m5.bsp` (LibreQuake) | 1.1 MB | 60 / 60 | 1189 | 0 |
| `lqdm3.bsp` (LibreQuake) | 820 KB | 60 / 60 | 3748 | 8361 over 512 frames |

With `lqdm3` loaded, RAM use is about 1.2 MB and texture VRAM is 220 KB.
`dev.bsp` uses about 600 KB of RAM. The view from its start position
matches the desktop engine's view.

## Models

Monsters, weapons, armour, powerups and keys from the map's entities are
drawn with their Quake models from `progs/`, animated at 10 frames a
second through their first 40 frames. The view model is `v_shot.mdl`.

- Frames stay as the file's bytes, plus a shade byte worked out at load
  from the geometry: fixed light from above, times the brightness of the
  floor under the model.
- Skins are shrunk to at most 128×128. 64 KB of texture VRAM is kept for
  them.
- Models are back-face culled on the GPU. They're drawn first, nearest
  first, and the engine then reads back how many polygons and vertices the
  GPU actually stored. The world gets what's left, so a crowd costs the
  farthest world faces, not the models.

## Bench

`bench.txt` lists map file names, one per line. A line can add a crowd
in front of the start position: `e1m1.bsp soldier 9` puts nine soldiers
there. These lines change a setting for the lines after them:

| Line | Setting |
|---|---|
| `multipass on` / `off` | two passes |
| `fps 30` / `fps 60` | cap one pass at 30 fps |
| `flicker on` / `off` | models take turns |
| `merge on` / `off` / `2` | joining faces at load; a number sets the light step |
| `portals on` / `off` / `auto` | following portals instead of drawing the PVS |

At start-up, the viewer loads each map and turns once on the spot over 512
frames. A result reads `e1m1.b 60/60fps p816 a230 v2652 m3 s0`: average
and worst frame rate, peak and average GPU polygons, peak vertices, most
models drawn, and faces that went over the GPU's limit (summed). A prefix
shows settings off their defaults: `u` unmerged, `p` portals forced on,
`n` portals off, `2` two passes, `f` flicker. After the bench, it stays in the
last map. Use it to compare builds on real hardware.

## Measured: Quake shareware with models, in melonDS 1.1

The shareware `pak0.pak` (v1.06), packed into the ROM. Each run is a full
turn on the spot at the start, over 512 frames. Polygons and vertices are
what the GPU actually stored at the peak, out of 2048 and 6144. *Models* is
the most drawn at once, including the view model. *Faces left out* is
summed over the 512 frames.

| Run | fps (avg / worst) | Peak polygons | Peak vertices | Models | Faces left out |
|---|---|---|---|---|---|
| `e1m1` | 60 / 60 | 1342 | 2817 | 3 | 0 |
| `e1m2` | 60 / 60 | 1823 | 4295 | 5 | 1741 |
| `e1m3` | 60 / 60 | 718 | 1751 | 2 | 0 |
| `e1m5` | 60 / 60 | 1667 | 4109 | 7 | 5836 |
| `e1m1` + 9 soldiers | 60 / 60 | 1961 | 4833 | 8 | 4813 |
| `e1m1` + 9 ogres | 60 / 60 | 1961 | 4852 | 8 | 4440 |
| `e1m1` + 9 shamblers | 60 / 60 | 1960 | 4803 | 9 | 2998 |
| `e1m1` + 16 knights | 60 / 60 | 1961 | 4781 | 11 | 1783 |

The id models are 213 to 328 triangles (knight 213, shambler 284, soldier
328), and about half of each survives back-face culling. LibreQuake's
models in Nuclide's `base` are two to three times heavier (soldier 886).
melonDS doesn't time the ARM9 exactly, so the frame rates need checking
on hardware. The polygon counts are the GPU's own.

## Quads

The GPU's 2048-polygon limit counts a quad as one polygon, the same as a
triangle. World faces are drawn as quad strips, so an n-sided face costs
(n - 1) / 2 polygons instead of n - 2. Over the shareware episode 1 maps
that is 55% of the triangle count (most Quake faces have four to six
sides). The image is the same: on `e1m2`, 2 pixels of 49152 differ by
more than a few shades from the triangle build.

Same bench, one pass, after the change:

| Run | fps | Peak polygons | Peak vertices | Models | Faces left out |
|---|---|---|---|---|---|
| `e1m1` | 60 / 60 | 878 | 2846 | 3 | 0 |
| `e1m2` | 60 / 60 | 1348 | 4350 | 5 | 0 |
| `e1m5` | 60 / 60 | 1362 | 4475 | 7 | 0 |
| `e1m1` + 16 knights | 60 / 60 | 1669 | 5206 | 11 | 0 |
| `e1m1` + 24 knights | 60 / 60 | 1552 | 4872 | 11 | 0 |
| `2:e1m1` + 24 knights | 30 / 30 | 2719 | 8380 | 22 | 0 |

The tables below were measured before this change.

## Joining faces

The map compiler cuts walls into many small faces, along BSP splits and
into pieces small enough for Quake's software surface cache. At load,
faces on the same plane with the same texture mapping are joined again
where they share a whole edge and the result stays convex
(`World_MergeFaces`). A vertex left in the middle of a straight edge is
dropped only when its light is within 2 steps (of 31) of what the edge
gives, so the lighting stays as it was. A face that was joined away points
at the face that took it in, so its leaves still draw it.

On the whole of `e1m2` this takes the GPU polygons from 8521 to about 6900,
and the peak in one view from 1343 to about 1270. Ignoring light, the
limit for joining convex pieces is about 74% of the polygons. An earlier
estimate of 55% counted triangles and allowed shapes the GPU can't draw
in one piece; it was wrong for quads. The image is the same as unjoined.

## Portals

Quake maps keep a PVS: from each leaf, the leaves that can be seen from
somewhere in it. `portals_nds.c` works out the portals, the openings
between neighbouring leaves, from the BSP tree at load. Each frame the
viewer follows them out from the eye's leaf, narrowing the view to each
opening as it goes, and draws only the leaves it reaches (`World_Portals`).

That pays only on maps without a PVS. On the shareware maps, which have
one, the PVS and the frustum test leave little for portals to remove, and
following them costs CPU time. So the default (`auto`) builds portals only
for a map with no PVS. Measured on `e1m1`, and on a copy of it with the
PVS removed:

| Run | fps (avg / worst) | Peak polygons | Average polygons | Faces left out |
|---|---|---|---|---|
| `e1m1`, PVS | 60 / 60 | 816 | 230 | 0 |
| `e1m1`, PVS and portals | 55 / 30 | 800 | 222 | 0 |
| `e1m5`, PVS | 60 / 60 | 1270 | 376 | 0 |
| `e1m5`, PVS and portals | 42 / 20 | 1268 | 368 | 0 |
| `e1m1` without its PVS | 39 / 20 | 1938 | 717 | 210833 |
| `e1m1` without its PVS, portals | 50 / 30 | 800 | 222 | 0 |

Without its PVS and with portals, `e1m1` draws the same image as with its
PVS, pixel for pixel, at the end of the bench. Building the portals took
about 6.8 s of loading in melonDS (about 3300 portals) and 350 KB of RAM.
Water leaves stay hidden, as Quake's PVS keeps them.

## Two passes: 4096 polygons at 30 fps

The GPU stores 2048 polygons for each screen refresh. In two-pass mode a
frame takes two refreshes (`multipass_nds.c`):

1. The far pass draws the faces past a split distance and the models out
   there. The display capture unit copies the 3D image into VRAM C. The
   screen meanwhile shows VRAM D, the last finished frame.
2. The near pass draws VRAM C as a texture on one quad at the back of the
   depth range, then the nearest faces, models and the view model over it.
   The capture unit copies the result into VRAM D for the next frame.

The world is walked once a frame, nearest face first. The split is the
point in that list where the near pass's budget runs out. Models are
counted into the budget at their distance as the list is walked, so a
crowd near the camera moves the split closer. Brush entities always go
in the near pass. Because the BSP order is front to back, nothing in the
far pass can cover anything in the near pass, so laying one image over
the other is correct without sharing a depth buffer. Both passes are
full resolution; in melonDS the joined image shows no seam. This hasn't
been checked on hardware yet.

This is the method pikmin-nds documents, written here from the hardware
documentation (GBATEK), not from its code.

Same bench as above, shareware `pak0.pak`, melonDS 1.1. A `2:` run is
two-pass. Polygons and vertices are both passes added together.

| Run | fps (avg / worst) | Peak polygons | Peak vertices | Models | Faces left out |
|---|---|---|---|---|---|
| `e1m2` | 60 / 60 | 1823 | 4295 | 5 | 1741 |
| `2:e1m2` | 30 / 30 | 1902 | 4455 | 5 | 0 |
| `e1m5` | 60 / 60 | 1667 | 4119 | 7 | 5696 |
| `2:e1m5` | 30 / 30 | 1943 | 4678 | 7 | 0 |
| `e1m1` + 16 knights | 60 / 60 | 1961 | 4799 | 11 | 1747 |
| `2:e1m1` + 16 knights | 30 / 30 | 2817 | 7211 | 17 | 0 |
| `e1m1` + 24 knights | 60 / 60 | 1960 | 4671 | 11 | 180 |
| `2:e1m1` + 24 knights | 30 / 30 | 3078 | 8086 | 22 | 1981 |

Two passes draw every face on the shareware maps that one pass has to cut,
and 16 knights fit whole. 24 knights (about 4600 triangles before culling)
overflow even 4096 polygons, so both passes fill and the far pass drops
faces, but twice as many knights get drawn. The cost is half the frame
rate and 128 KB of texture VRAM.

## Flicker: models take turns

With flicker on, the world is counted first, and models get only what it
leaves. The view model and the nearest models, up to 600 GPU polygons,
are always drawn. The models past those that don't all fit take turns:
each frame draws a different run of them, as NES games did with sprites.
A model drawn every other frame looks half transparent at 60 fps, and
flickers at 15 Hz in two-pass mode. Without flicker, models are drawn
first and the farthest world faces are cut instead.

Same bench. `f:` is flicker on, `2f:` two passes with flicker. *Taking
turns* is the most models sharing the leftover budget in one frame.

| Run | fps | Peak polygons | Models per frame | Taking turns | Faces left out |
|---|---|---|---|---|---|
| `e1m1` + 24 knights | 60 / 60 | 1960 | 11 | 0 | 180 |
| `f:e1m1` + 24 knights | 60 / 60 | 1837 | 11 | 22 | 0 |
| `f:e1m1` + 16 knights | 60 / 60 | 1812 | 11 | 14 | 0 |
| `f:e1m5` | 60 / 60 | 1644 | 7 | 2 | 1780 |
| `2:e1m1` + 24 knights | 30 / 30 | 3079 | 22 | 0 | 1959 |
| `2f:e1m1` + 16 knights | 30 / 30 | 2828 | 17 | 0 | 0 |
| `2f:e1m1` + 24 knights | 30 / 30 | 2972 | 21 | 8 | 0 |
| `2f:e1m1` + 32 knights | 30 / 30 | 2806 | 22 | 23 | 0 |

With flicker, no world faces are cut for models. `e1m5` still loses faces
because its world alone is over 2048 polygons in places. Flicker hasn't
been seen on hardware; melonDS screenshots show one frame at a time.

## Skeletons

The DS can pose a skeleton on the CPU and draw each bone's part with the
bone's matrix. `tools/nds/skelreduce` cuts a Source model's skeleton down
on the PC and writes a `.bsk` file: the kept bones, the model's hitboxes,
and every clip's keys (a quaternion and a translation a bone, 14 bytes).

- Every kept bone keeps its exact place. Its pose is taken from the full
  skeleton relative to its nearest kept ancestor, so whatever the bones in
  between did is folded into it.
- A dropped bone, with its hitboxes, rides rigidly on its nearest kept
  ancestor, as it was in the bind pose. `-report` says how far that puts
  each part of the body from where the full skeleton has it.
- `-clips` picks the clips and `-fps` resamples them. `tools/nds/skelplot.py`
  draws the full and cut-down skeletons over each other from `-poses`.

```sh
cc -O2 -o skelreduce tools/nds/skelreduce.c tools/nds/skelstub.c plugins/source/mdlskel.c -lm
./skelreduce <game folder> models/infected/hulk.mdl tank12.bsk \
  -keep Pelvis,Spine2,L_UpperArm,L_Forearm,L_Hand,R_UpperArm,R_Forearm,R_Hand,L_Thigh,L_Calf,R_Thigh,R_Calf \
  -clips Idle,Run_1,Walk,Melee,Throw -ignore debris_bone,jiggle_tongue01,jiggle_tongue2 -report tank12.txt
```

A `.bsk` is the game's data cut down, so it is kept out of git
(`.gitignore`) and out of packages.

On the DS (`skel_nds.c`), each bone's keys either side of the time are
blended (nlerp: one hardware square root and one hardware divide), turned
into a 4.12 matrix and multiplied onto the parent's. With up to 23 bones
the matrices are stored in the GPU's position matrix stack once, and each
part only restores its bone's slot, as a mesh with one bone a vertex would
be drawn. Bigger skeletons load the matrix again for each part. For now
each hitbox is drawn as a box.

In `bench.txt`, `skel <file> <copies> [clip]` runs a skeleton for 240
frames, every copy on its own clip unless one is named. A result reads
`tank12 12x8 60/60fps 166us p970`: bones and copies, frame rates, posing
time a copy, and GPU polygons. A list that ends in a `skel` line stays on
it: L and R step through the clips, UP and DOWN change the copies.

## Measured: the L4D Tank cut to 12 bones

The Tank (`hulk.mdl`, 56 bones: 30 of them fingers, plus a rock bone for
throwing and two tongue bones) with its 76 clips. The 12 bones kept are
pelvis, one spine bone, upper arm, forearm and hand for each arm, and thigh
and calf for each leg. Feet, head, neck, clavicles and fingers ride on
their parents. A Source unit is an inch.

| Bones | Worst-placed body part a frame: median / 95% / worst | All 76 clips at 30 fps |
|---|---|---|
| 56 | 0 | 3.6 MB |
| 20 (also feet, clavicles, neck, head, 3 spine) | 4.1 / 4.8 / 17.4 units | 1.3 MB |
| 13 (the 12 and the head) | 7.6 / 12.8 / 22.2 units | 0.87 MB |
| 12 | 8.5 / 13.6 / 22.2 units | 0.80 MB |
| 12, head instead of hands | 11.8 / 24.5 / 33.2 units | 0.80 MB |

The Tank's hands are as long as its forearms, so they matter more than its
head. With the hands kept, most of what remains is the spine's bend (one
bone instead of four), the head and the feet. Fingers on a kept hand move
at most 17 units, in clenched fists.

In melonDS 1.1, with 12 clips (1086 frames). On the PC, the DS's
fixed-point posing lands within 1.1 units (12 bones) to 1.9 units (56
bones) of the same keys posed in floating point.

| File | Size | Posing a copy | 8 copies | 16 copies |
|---|---|---|---|---|
| 56 bones | 834 KB | 781 µs | 59 / 60 fps | |
| 20 bones | 298 KB | 278 µs | | |
| 12 bones | 179 KB | 167 µs | 60 / 60 fps | 60 / 60 fps |
| 12 bones, 15 fps keys | 91 KB | 167 µs | | |

Posing costs about 14 µs a bone, so a 12-bone Tank takes 1% of a 60 fps
frame. melonDS doesn't time the ARM9 exactly, so these times need checking
on hardware. A mesh hasn't been tried: the boxes are 19 hitboxes, 114
quads a copy.

## Measured: linked, in melonDS 1.1

melonDS joined a `bulwark-ded` server on Nuclide `base` with `link.txt`
(`server=10.0.2.2:27901`, `autojoin=1`) through melonDS's own WiFi. The
bottom screen showed the engine's default screen, and the top screen drew
`dev` and `lqdm3` from the server's player position. A second link player
walked down a corridor and showed up as a green marker in the right place.
The DS's own button presses couldn't be scripted in the emulator, so the
movement and buttons were tested with a script that sends the same packets.

## Measured: the Half-Life campaign, in melonDS 1.1

All 97 maps of `valve/maps` convert. A bench of 8 spots a map (player
starts, landmarks and `info_node`s spread through the map) over the
campaign, at commit `9e8bac2`:

- Every map loads; loading takes 2 to 7 s in melonDS.
- Every map held 60 fps at every spot with no faces cut, except two:
  c1a2 (58 average, 30 worst, 173 faces cut at the worst spot, 5941
  vertices) and c2a5e (59 average, 30 worst, 30 faces cut). Both are at the
  6144-vertex limit there, not the polygon limit.
- The highest polygon count at any spot was 1706 (c2a5e); 1428 in c1a1b.
- Memory leaked per map load: 1 to 4 KB, from the bench's own count.

Linked: a `bulwark-ded` running Nuclide `valve` + `coop` hosted the DS and
a PC client at once. A script walked the link player along the 78-map
route from c0a0 to c5a1 (a breadth-first search over
`trigger_changelevel`), teleporting next to each changelevel with
`link_teleport` and touching it; 8 changelevels that only fire when used
or from a script were fired by rcon instead ("forced"). At each map the DS
stood at a path node and `link_stats` was read three times (commit
`795816d`):

- The DS loaded and drew all 78 maps, with the server's doors, lifts and
  NPC boxes, and reached c5a1.
- 76 maps held 60 fps. c2a2b1 dipped to 30 (924 polygons) and c1a2 ran at
  30 with a worst of 20 (1628 to 1839 polygons). c1a2 only fits without
  joining faces, so it draws more polygons than in the bench.
- Loading took 1.4 to 7 s on most maps; the big ones that need the
  fallbacks took 12 to 19 s (c1a2: 19 s), because each failed attempt
  reads the map again.

That proves the maps load, draw and follow the server; it does not prove
the campaign can be played through by hand, since the script skipped
every puzzle and fight.


### With models and several players

Commit `0f45a65`, three DS (each its own melonDS, `link_god 1`) plus a PC
client and a bot on one `bulwark-ded` (Nuclide `valve` + `coop`), all 206
campaign and player models baked:

- `coop_arena`, waves pushed to 40 monsters for 12 minutes: all three DS
  averaged 60 fps (lowest second 58), 1000 polygons on average, 1300 at
  most.
- c2a4e, each DS put 160 units from a group of grunts or headcrabs and
  facing it, 5 minutes: 30 fps at the busiest spot (1400 polygons), 55 on
  average at another. With models off (SELECT) the busiest spot was 30
  too; before the floor light was cached per thing it was 20 to 30.
- c1a2, the same, after a level change from c2a4e: two DS held 30, one 20
  (1830 polygons). With models off that one was still 20: it is the world
  there (c1a2 loads without joining faces), not the models. Few NPCs were
  in view at that spot.
- Before `0f45a65`, c1a2 failed to load after a level change: the server
  started sending again before the DS had loaded, and the DS's network
  stack kept the 378 packets (244 KB) that came in during the 15 s load.

melonDS's frame rate is not a hardware measurement: these need repeating
on a DS.

## Prediction over the Half-Life campaign, measured in melonDS 1.1 (not hardware)

The rig: `bulwark-ded` (Nuclide `valve` + `coop`) on the original Half-Life
BSPs (their clip hulls, real gravity), `link_god 1`, and the DS in melonDS
1.1 with `fps=30` and `botwalk=1`, a fresh boot per map. At each spot
(spawn, the `info_node` farthest from the start, and a ladder, water and a
lift or train where the map has them) 12 s of DS time are recorded after
4 s to settle. "Within" counts the commands whose predicted origin is
within 0.5 u of what the server sent for the same command. The level-change
chain boots once and changes level by rcon along the campaign route (103
maps), 10 s each. Every number here is from one run in melonDS, with up to
three melonDS and three servers sharing 4 cores; none is from hardware.

What changed, and why:

- **Level changes no longer drop the link.** The map loads inside the
  receive loop, and the link's last-heard frame was the one read before
  the load, so a load over the 5 s timeout ended the link at the next quiet
  loop: 7 of 102 changes in the chain. `lastrecv` and the frame now move on
  to after the load (`link_nds.c`, `handle_welcome`'s caller).
- **Less RAM at the load's peak.** The light is read through a 64 KB window
  instead of whole (`world_nds.c`, `light_at`), the face lumps are freed
  before faces are merged, the merged vertices are counted before they are
  allocated, and `mvert_t` is 16 bytes. On c4a1f → c4a3 the peak in use
  went from 2.92 to 1.53 MB; the load takes about 5 % longer (5460 → 5710
  ms). The drawn vertices are the same (c4a3: 14800 vertices, the same
  hash).
- **A body the server's player already stands in stays in the scene.** The
  focus left out every box the predicted player overlapped
  (`PM_FOCUS_DROPSTUCK`). The server keeps them: its trace starts solid
  there (`sv_world.c`, `trace_aabb`) and the player stays still. This
  caused the "server holds the player still" spots: c2a3e's spawn inside
  the `intro_box` breakables, c1a1b's lift, and c2a2a's. PSTATE now pins
  the boxes that overlap the server's player (`Hull_PinBoxes`).
- **Turned brushes are traced turned.** The server traces a brush entity
  with angles in its own frame (`sv_world.c`, `trace_brush_entity`) and
  culls it with the absmin/absmax it was last linked with. A
  `func_tracktrain` turned along its path keeps the box of its spawn (c2a2f).
  BRUSHINFO and PSTATE's movers now carry the angles and that box
  (`docs/spec/link.md`), and `hull_nds.c` traces the same float steps as the
  server. pmtest on a c2a2f log is exact for 2563 of 2563 commands; an
  old-format log of the same route was exact for 1332 of 2552. A brush
  whose angles the game turns from its think, with no `avelocity` (c1a1c's
  edict 810), counts as moving: before, it made a new BRUSHINFO version at
  every check.
- **Push triggers and conveyors.** Nuclide runs these touches inside the
  movement: `PMoveCustom_DoTouch` on what the move hits and stands on
  (`pmove_custom.qc:657`, `:679`), and `touchtriggers` at its end (`:727`).
  `pmove_nds.c` now ports `trigger_push::Touch` (`trigger_push.qc:353-404`)
  and `func_conveyor::Touch` (`func_conveyor.qc:376-388`). BRUSHINFO's
  touch block gives their speed, direction, flags and box. A push with a
  target (`Route_GetJumpVelocity`) stays the server's. In pmtest's chain
  mode (several commands predicted in a row, as the DS does between
  PSTATEs), falling into c1a1c's down push went from 104 to 180 of 180
  exact, and riding a c2a4c conveyor from 37 to 180 of 180. Nuclide's
  `func_water` is a door with no current (`func_door.qc:646`), so there is
  no water current to predict: c3a2b's water spot was the spawn's push and
  boxes.
- **t0a0a, t0a0b, t0a0b1, t0a0b2, t0a0c, t0a0d** convert with `hlmap.py`.
  Their textures are in the maps, so no WAD is needed. Their `.hul` files
  are byte for byte the ones the campaign package has.

Port check: `tools/nds/pmtest` (step mode, each logged command from the
server's own state, through `hull_nds.c`) gives exactly the server's result
for 370599 of the 370606 commands in the 103 new logs. Six of the other
seven are teleports (the rig's on c1a0c, the map's on c5a1). The seventh
is the command in which c1a4e's player died.

Before (`58a5bba`) and after, fresh boot per map:

| | before | after |
|---|---|---|
| maps measured | 97 (t0a0a-d had no DS map) | 103 |
| commands within 0.5 u | 97.20 % (102340 of 105290) | 99.27 % (109989 of 110803) |
| lowest free during a fresh load | 28 KB (c1a2b) | 260 KB (c2a4e) |
| lowest free in play | 1253 KB (c2a4e) | 1327 KB (c2a4e) |
| frame rate, median of every spot | 30 | 30 |
| prediction per frame, average / worst second | 1.77 / 11.96 ms | 1.89 / 14.69 ms |
| chain: level changes that lost the link | 7 of 102 | 0 of 102 |
| chain: lowest free during a load | 0 KB (c2a5e after c2a5d) | 196 KB (c2a4e after c2a4d) |
| chain: commands within 0.5 u | 89.7 % (23586 of 26290) | 99.77 % (30159 of 30230) |

The maps that were worst:

| map | within, before | within, after | error avg / max after |
|---|---|---|---|
| c2a3e | 56.2 % | 100.0 % | 0.000 / 0.00 u |
| c1a1b | 70.0 % | 99.6 % | 0.004 / 1.03 u |
| c5a1 | 68.3 % | 99.7 % (one spot of two) | 0.007 / 2.34 u |
| c0a0 (tram) | 75.4 % | 98.7 % | 0.016 / 3.49 u |
| c3a2b | 77.1 % | 99.2 % | 0.013 / 2.49 u |
| c2a2f | 77.9 % | 97.1 % | 0.158 / 10.0 u |
| c2a2a | 78.8 % | 99.4 % | 0.010 / 2.32 u |
| c2a3a | 83.8 % | 98.9 % | 0.015 / 1.79 u |
| c1a2b | 88.9 % | 99.1 % | 0.011 / 2.58 u |

A second run of four of these maps on a quiet host (same ROM and server,
one run each) came out close: c2a3e 100 %, c1a1b 99.7 %, c2a2f 96.9 %,
c2a4e 99.2 %, lowest free during c2a4e's load 260 KB again. One spot can
move by a few percent between runs (c2a2f's lift: 337 of 358 in two runs,
359 of 360 in a third).

What is left (observed, causes inferred unless it says otherwise):

- c1a2: 72 % of only 134 commands compared in 36 s. The server falls behind
  on this map (1036 entities, a few PSTATEs a second) on the shared host, as
  before. This is the rig or the server, not the DS port.
- c1a0e's far node (71 %, max 16 u): the bot walked onto a moving brush
  and rode it. The server's log has edict 1007 as the ground entity for
  those commands, and the player moving +x at 100 u/s between commands
  (1.39 u a 1/72 s frame) with no NPC box within 64 u, so this is the
  riding case below, not the boxes (Measured from the server's
  `sv_link_log`; at `58a5bba` the bot did not board it). c1a1c's spawn
  (95 %, max 31 u) has a moving box (edict 2) and three brushes the game
  rolls (810-812) within 150 u. The DS has each of them where the last
  PSTATE put it, as the PC client does. pmtest is exact on both logs.
- Riding a moving train or lift is not predicted: c2a2g's tracktrain was
  91 % within, max 12 u. The PC client does not predict it either
  (`pmove_custom.qc:66-69` leaves the ground entity's velocity out), so the
  DS shows what the PC shows: each PSTATE corrects it.
- A `TP_ONCE` push (`trigger_push.qc:392-397` makes the trigger
  `SOLID_NOT` once it pushed) used to be run again on every command that
  still overlapped it, until the server's next BRUSHINFO dropped it (up to
  a quarter of a second, 20000 u/s more each time on c2a5f). `hull_nds.c`
  now notes the command each one ran in (`pmtrace_if.fired`) and leaves it
  out of the commands after that; a replay from the server's state forgets
  the ones that ran after the acked command, so they run again where the
  server will. Tested with pmtest (Measured, host): on a c2a5f log where
  the bot is pushed by `debris_locker_push` and `blocked_door_push`, with
  the touch line that drops the spent trigger moved 8 commands later (as
  the DS gets it), step mode went from 1759 to 1767 of 1767 exact and
  chain mode from 1706 to 1767 within (max error 24.1 u to 0); the
  unedited logs, and step mode on the 103 campaign logs, are unchanged. In melonDS the
  first push after `sv trigger` turns the trigger on is still one wrong
  command (about 300 u, one snap) before and after: the player is already
  inside when it turns on, and the DS learns of it a BRUSHINFO later.
- c5a1's lift spot ran with prediction off: the server had moved the player.
- Single slow seconds right after a load or a teleport (c0a0d 8 fps, c1a3
  5, c2a4e 14). The median was still 30 at every spot.
- RAM: the hull format was left as it is. Hulls are 17-184 KB (c5a1) and
  load after the map, with a 256 KB reserve. With the load peak lower, the
  lowest free in play is above 1.3 MB on every map, and no map refused its
  hull. A smaller hull would only help a map that the load itself can no
  longer fit, and none is left.

## The draw loop: c2a4e at 30, measured in melonDS 1.1 (not hardware)

The spot: `c2a4e`, linked, `link_teleport -576 1012 84 270`, facing the open
area (about 1805 polygons before, 1928 after: more of the world now fits in
the 2048 budget). Times are melonDS's `tickGetCount` around each part of the
frame (`BULWARK_NDS_PROF`, `nds.h`: off in a normal build), median of a 20 s
run, turning 30 degrees right and back with touch drags unless it says
standing. Observed, one run each.

| Change (commit) | Tree walk (`World_Collect`) | Models | Chaining | Vertex submission | GPU polygons |
|---|---|---|---|---|---|
| before (`5730913`) | 17.0 ms | 7.8 ms | 1.15 ms | 2.75 ms | 1805 |
| the walk from ITCM in ARM code, clip flags, frustum in DTCM (`95e8727`) | 9.1 ms | 7.8 ms | – | – | 1805 |
| leaf face lists resolved at load, axial planes, three-word vertices, models in three loads a corner (`7b15964`) | 7.4 ms | 6.9 ms | – | – | 1805 |
| the face list kept while the view stands (`797f477`) | 1.4 ms standing, about 5 ms turning | – | – | – | 1805 |
| models as triangle strips made at load (`0a3d2ac`, `41242ae`) | – | 4.9 ms | – | – | 1805 |
| models before the walk when they don't flicker, chaining from ITCM (`6155b0e`) | 1.4 ms standing, 4.9 turning | 4.3 ms | 0.93 ms | 2.74 ms | 1916 |
| colour written only when it changes (`439f063`) | 1.4 / 5.0 ms | 4.3 ms | 1.0 ms | 2.61 ms | 1928 |

The waits on the GX engine (`Gpu_Counts`) measured 0-1 µs before and after:
the CPU was never waiting on the GPU here, so no busy-wait was removed.
DMA to the GX FIFO was measured and not used: in melonDS it costs about
4 cycles a word with the CPU stopped, against 8-12 for a CPU write, and
the vertex loop's loads are what is left of the 2.6 ms.

The DS's own frame rate (sent to the server with its input, `link_stats`),
a normal build, 20 s each:

| Build | Standing: seconds at 30 (worst second) | Turning: seconds at 30 (worst second) |
|---|---|---|
| before (`5730913`) | 0 of 20: 27-29 (worst 15-20) | – |
| after (`439f063`) | 20 of 20 (30) | 7 of 17, the rest 29 (20) |
| after, with `wip/hud`'s `hud_diag` fix merged (scratch, not committed) | 20 of 20 (30) | 20 of 20 (30) |

What is left while turning is `hud_diag`: 15.5 ms once a second, on top of
an 18 ms frame (observed). Its fix is on `wip/hud`, not here.

RAM: the lowest free during load, linked on `c2a4e`, is 49152 bytes before
and after (observed, `minfree_ever`). The strips' working lists are 16-bit
and freed before the corners are allocated, and `Mp_Frame`'s model lists
moved to the stack. In the viewer bench (`c2a4e`, `c1a0`, `coop_arena`, one
and two passes) the polygon, model and vertex counts are the same before and
after, the light maps hold 60/60 and two passes 30/30; the lowest free there
is 45056 before and after, and 40960 only with the profile hooks built in
(4 KB pages).

Not done:

- Two passes linked: the second pass needs VRAM banks C and D, and linked
  the bottom screen's 16-bit bitmap is in bank C.
- c1a2: no converted c1a2 was available, so its 20 fps spot is not
  measured again.
- melonDS's timings are not a hardware measurement: repeat on a DS.

## Not supported yet

- Half-Life maps straight off the disc (convert them, above) and Quake 3 maps.
- Game logic on its own: away from a server, models stand where the map
  puts them and doors don't move. Linked to a server, doors, lifts and
  trains move and other entities show as boxes (`ENTS` in
  `docs/spec/link.md`). Health and ammo boxes (which are small `.bsp`
  models) aren't drawn.
- Collision in the map viewer: you fly through walls. (Linked, the DS
  predicts the server's collision, above.)
- Animated water, sky scrolling; music ("Sound", above), and sound in the map viewer.
