# Bulwark on the Nintendo 3DS

The 3DS build is the full engine (client, listen server, QuakeC VMs,
sound) with the renderer running on the PICA200 GPU through citro3d. It
runs Nuclide's `ai` map at full speed in the Azahar emulator, and draws it
the same way the desktop build does.

## Building

You need devkitPro with devkitARM, libctru and citro3d. The
`devkitpro/devkitarm` Docker image has everything:

```sh
docker run --rm -v $PWD:/src -v $PWD/build-3ds:/b -w /b devkitpro/devkitarm \
  bash -c 'cmake /src -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release && make -j4 bulwark_3dsx'
```

This produces `bulwark.3dsx` (about 1.8 MB) and `bulwark.smdh`. CMake turns
on `BULWARK_CONSOLE` for the 3DS. That leaves out:

- the dedicated server and the host tools;
- Panorama UI and JavaScript (`ui_null.c` stands in);
- SDL and FreeType;
- dynamic loading (`sys_dl.h` stubs it, so plugins are off);
- IPv6 (`net_compat.h`);
- the QuakeC compiler.

## SD card layout

```
sdmc:/3ds/bulwark.3dsx      the Homebrew Launcher finds it here
sdmc:/bulwark/              the base directory
sdmc:/bulwark/base/         game data (Nuclide's base, for example)
sdmc:/bulwark/args.txt      the command line, e.g. -game base +map ai
sdmc:/bulwark/bulwark.log   everything the engine prints, rewritten every run
```

The engine writes config, logs and saves into `sdmc:/bulwark`, so the SD card
must be writable. Sound needs the DSP firmware dump at
`sdmc:/3ds/dspfirm.cdc`. Without it the game runs silent.

## Controls

The top screen shows the game. The bottom screen shows the engine's text
output and works as a touch pad.

| 3DS | Engine key | Nuclide default |
|---|---|---|
| Circle pad | left stick | move |
| C-stick (New 3DS) | right stick | look |
| A B X Y | `GP_A` `GP_B` `GP_X` `GP_Y` | |
| R / L | right / left trigger | attack / secondary attack |
| ZR / ZL (New 3DS) | right / left shoulder | |
| D-pad | D-pad | |
| START / SELECT | `GP_MENU` / `GP_VIEW` | menu / scores |
| L + SELECT | | toggles the console |
| A with the console down | | opens the system keyboard and runs what you type |
| touch | mouse | a tap clicks in menus; a drag turns the view in game |

## Memory

The engine asks libctru for a 20 MB linear heap, which holds textures,
vertex buffers and the GPU command buffer. It also asks for a 1 MB main
stack. Everything else comes from the normal heap.

When `BW_LOWMEM` is set (3DS and DS, in `common.h`), the defaults shrink:

| | Desktop | 3DS |
|---|---|---|
| server edicts / QC heap | 4096 / 32 MB | 1024 / 4 MB |
| CSQC edicts / heap | 8192 / 32 MB | 1024 / 4 MB |
| menu edicts / heap | 4096 / 16 MB | 1024 / 2 MB |
| client entity slots | 65536 | 4096 |
| font atlas | 1024² | 512² |
| particles / scene polys | 32768 / 8192 | 8192 / 2048 |

`pr_maxedicts` and `pr_heapsize` still override the server's values.

Measured in Azahar on Nuclide's `ai` map, at a steady state after loading:
about 43 MB of heap and 11 MB of linear memory are in use, plus about 12 MB
of static data. `ctr_meminfo` prints the current figures.

- **New 3DS:** this fits. The emulator gives the app about 92 MB of heap.
- **Old 3DS:** this has not been tested on hardware. It probably does not
  fit, because the app gets about 64 MB in total.

## Renderer notes

`src/platform/ctr/gl_ctr.c` implements the subset of OpenGL 3.3 that the
renderer uses. The comment at the top of that file explains how it works.
Things that differ from the desktop build:

- **Not drawn:** dynamic lights, fog, lines and points. Framebuffer objects
  aren't available, and `glReadPixels` returns black, so screenshots are
  black too.
- **Texture size:** `gl_max_size` (default 256) caps world textures.
  Pictures and fonts for the 2D layer keep their size, up to 1024.
- **Texture formats:**
  - Textures are 16-bit: RGB565, RGBA5551 or RGBA4, whichever keeps the
    picture's alpha. `gl_ctr_texquality 1` stores 32-bit instead.
  - Lightmaps are always RGB8.
- **Mipmaps:** made on the CPU. `gl_ctr_mipmaps 0` turns them off.
- **Texture rows:** the GPU stores rows bottom up, so GL's row 0 lands in
  the last row of memory.
- **Screen orientation:** the screen is portrait in memory. The shim turns
  the projection, viewports and scissors by a quarter turn.

## The denormal float bug

The 3DS's VFP runs in flush-to-zero mode. QuakeC keeps integers (function
numbers, entity numbers from `externvalue` and similar) in float slots,
where they are denormal floats. Those read as zero there, so a check such as
`if (func)` failed and Nuclide broke in odd ways ("unimplemented shared API
call"). `qc_exec.c` now tests float truth and equality on the bits
(`ftrue`, `feq`), the way the desktop FPU behaves.

## Testing in Azahar

The `linuxserver/azahar` image runs headless under Xvfb. Its HOME is
`/config`, so the emulated SD card is
`/config/.local/share/azahar-emu/sdmc`. Bind your SD folder there, start
`azahar bulwark.3dsx`, and capture the X display with
`ffmpeg -f x11grab`. Read `bulwark.log` on the SD card for the engine's
output. Loading Nuclide's `ai` map takes about two minutes in the emulator.
