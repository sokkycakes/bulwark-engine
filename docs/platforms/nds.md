# Bulwark on the Nintendo DS

The DS build doesn't run the engine. It is a small, separate map viewer
that loads Quake-format maps (BSP version 29) and flies through them on
the DS's 3D hardware.

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

## Controls

| Button | Action |
|---|---|
| D-pad up / down | move |
| D-pad left / right | turn |
| L / R | strafe |
| X / B | look up / down |
| A / Y | rise / sink |
| touch and drag | look around |
| START | back to the map list |

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
  - A map's textures have to fit in 384 KB of VRAM (banks A, B and D).
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

## Bench

`bench.txt` lists map file names, one per line. At start-up, the viewer
loads each of them and turns once on the spot over 512 frames. It then
shows the average and worst frame rate, the peak GPU vertices, and how
many faces went over the GPU's limit. After the bench, it stays in the
last map. Use it to compare builds on real hardware.

## Not supported yet

- Half-Life maps (BSP 30, with textures in external WADs) and Quake 3 maps.
- Entities and models. Brush entities such as doors are drawn where the
  map places them, and they don't move.
- Collision: you fly through walls.
- Animated water, sky scrolling, sound.
