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
```

The PC runs a normal Bulwark server (`bulwark-ded` or a listen server).
Link players use UDP port 27901 (`sv_link_port`). The DS lists the servers
that answer, and A or a tap joins one.

While linked:

| Button | Action |
|---|---|
| D-pad up / down | move |
| D-pad left / right | turn |
| L + D-pad | strafe, and look up / down |
| A, B, X, Y, R | attack, jump, use, reload, attack2 |
| touch | goes to the game's private screen |
| START | leave |

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
| SELECT | steps through one pass, one pass with flicker, two passes, two passes with flicker |

In the map list, the D-pad and A pick a map, or you can tap a name. The
bottom screen shows the frame rate, faces, GPU vertices and polygons, and
RAM and texture VRAM in use.

## How it fits

All floating-point work happens once, at load time. A frame only uses
integer and fixed-point maths. The full list is at the top of
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
there. A line `multipass on` (or `off`) switches the drawing mode for the
lines after it, and `flicker on` (or `off`) does the same for flicker. At start-up, the viewer
loads each of them and turns once on the spot over 512 frames. It then
shows the average and worst frame rate, the peak GPU vertices, and how
many faces went over the GPU's limit. After the bench, it stays in the
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

## Measured: linked, in melonDS 1.1

melonDS joined a `bulwark-ded` server on Nuclide `base` with `link.txt`
(`server=10.0.2.2:27901`, `autojoin=1`) through melonDS's own WiFi. The
bottom screen showed the engine's default screen, and the top screen drew
`dev` and `lqdm3` from the server's player position. A second link player
walked down a corridor and showed up as a green marker in the right place.
The DS's own button presses couldn't be scripted in the emulator, so the
movement and buttons were tested with a script that sends the same packets.

## Not supported yet

- Half-Life maps (BSP 30, with textures in external WADs) and Quake 3 maps.
- Game logic: models stand where the map puts them and animate on the
  spot. Brush entities such as doors are drawn where the map places them,
  and they don't move. Health and ammo boxes (which are small `.bsp`
  models) aren't drawn.
- Collision: you fly through walls.
- Animated water, sky scrolling, sound.
