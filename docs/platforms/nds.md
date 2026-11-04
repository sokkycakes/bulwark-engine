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
(QLIT 1). Clip hulls are dropped: the DS doesn't collide. Each texture
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
there. These lines change a setting for the lines after them:

| Line | Setting |
|---|---|
| `multipass on` / `off` | two passes |
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
`link_teleport` and touching it; changelevels that only fire when used or
from a script were fired by rcon ("forced"). The DS loaded each map the
server changed to, drew the doors, lifts and NPC boxes it sent, and
reported 60 fps (`link_stats`) on every map it was measured on, at up to
about 1050 polygons. That proves the maps load, draw and follow the
server; it does not prove the campaign can be played through by hand,
since the script skipped every puzzle and fight.

melonDS's frame rate is not a hardware measurement: these need repeating
on a DS.

## Not supported yet

- Half-Life maps straight off the disc (convert them, above) and Quake 3 maps.
- Game logic on its own: away from a server, models stand where the map
  puts them and doors don't move. Linked to a server, doors, lifts and
  trains move and other entities show as boxes (`ENTS` in
  `docs/spec/link.md`). Health and ammo boxes (which are small `.bsp`
  models) aren't drawn.
- Collision: you fly through walls.
- Animated water, sky scrolling, sound.
