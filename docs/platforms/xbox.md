# Bulwark on the original Xbox

The Xbox build is the full engine: client, listen server, QuakeC VMs and
sound. It is built with nxdk, the open-source Xbox development kit, so no
Microsoft XDK is involved. The renderer runs on the NV2A GPU through nxdk's
pbkit. The game is Stiletto on Nuclide's `base`, with a menu and HUD drawn
in C (the native UI), because PanoramaEngine costs about 3.5 MB of XBE and a
lot of memory on a 64 MB console.

The port is still being brought up. It boots, reaches the title and loads
maps in xemu, but has not been played through on hardware. "Known limits"
at the end lists what is missing.

## Building

You need nxdk, built, with its `bin` folder on the path:

```sh
export NXDK_DIR=/path/to/nxdk PATH=$NXDK_DIR/bin:$PATH
cmake -S . -B build-xbox -DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-xbox --target bulwark_xbe -j8
```

This produces `build-xbox/xbe/default.xbe` (about 4 MB) and
`build-xbox/bulwark.map`, the linker map, for reading addresses in crash
reports. `src/platform/xbox/xbox.cmake` sets up the toolchain. It does the
following:

- turns on `BULWARK_CONSOLE`, which leaves out plugins, the dedicated
  server and the host tools;
- turns on `BULWARK_UI_NATIVE` (below), which also turns PanoramaEngine
  and JavaScript off;
- defines `BW_XBOX`, which also turns on `BW_LOWMEM`;
- puts the POSIX shims in `src/platform/xbox/compat` first on the include
  path;
- builds for the Pentium III's SSE (single precision) and leaves out the
  SSE2 paths of the image and MP3 decoders.

To build the Panorama UI instead, add
`-DBULWARK_UI_NATIVE=OFF -DBULWARK_UI_PANORAMA=ON`. The XBE grows by about
3.5 MB, and the HTML pages need much more memory at run time. That build is
kept for comparison only.

`-DBW_FORCE_LOWMEM` on a desktop build gives the same smaller QuakeC and
entity limits as the Xbox, for profiling memory on a PC.

## Disc layout

The XBE's own folder is mounted as `D:` (nxdk's automount), and that is the
base directory:

```
default.xbe     the engine
args.txt        the command line, e.g. -game base -game stbuilt
base/           Nuclide's base and Stiletto's data
stbuilt/        Stiletto's built progs, the native UI's atlas cache, config.cfg
data/           Bulwark's own data folder (its fonts)
bulwark.log     everything the engine prints, rewritten every run
```

`args.txt` takes the same words as a desktop command line. It also takes
three switches that only the Xbox knows:

| Switch | Effect |
|---|---|
| `-nonet` | don't start the network (by default it starts in the background and asks for an address by DHCP) |
| `-nodbgprint` | don't copy the log to the kernel debugger |
| `-fstest <dir>` | print what `opendir`, `readdir` and `stat` report for a folder, for checking the file system shims |

The engine writes `config.cfg`, saves and the log into the folder, so it
must be writable. On a DVD or ISO it isn't: the game still runs, but
nothing is saved, and the log goes only to the kernel debugger (`DbgPrint`,
which xemu shows on its serial port when it runs a debug kernel).

### Staging a disc

In development, the folder and an ISO image are made by a staging script
kept beside the repository (`mkdisc.sh <outdir> "<args>" [maps...]`). It
does this:

1. Copies `default.xbe` and writes `args.txt`.
2. Copies Bulwark's `data/`, and `stbuilt/` with its symlinks followed.
3. Copies Nuclide's `base/`, leaving out maps, map and model sources,
   demos, saves, editor files and logs.
4. Copies only the maps you name (default `lqdm1`), with their `.lit`,
   `.lux` and `.ent` files. The full map set doesn't fit in memory and
   takes a long time to copy.
5. Packs the folder into `<outdir>.iso` with nxdk's `extract-xiso -c`.

Before staging, bake the native UI's atlas cache into `stbuilt/ui/` on the
PC (see "Native UI").

### Running in xemu

A second script (`xrun.sh <iso> <seconds> <outprefix> [memory MB]`) boots
the ISO in xemu with no window interaction. It writes an xemu config with
these settings:

- no boot animation;
- 64 MB of memory by default, or 128 for a development console;
- the HDD image;
- for the flash, the open-source Roswell kernel (a ReactOS-based Xbox
  kernel), so no Microsoft BIOS is needed. With its debug build, the
  engine's `DbgPrint` output reaches the serial file.

It then starts xemu with `-snapshot`, so the HDD image is never changed. It
also opens a QEMU monitor socket, which is useful for reading registers
when the game hangs.

Look at `bulwark.log`, or the serial file, for the engine's output.

### In-match benchmark

The deathmatch rules spawn the local player at once and then ask for the
hero picker (`showClassSelectionMenu`, sent with `stuffcmd`). Text a server
stuffs goes to the end of the command buffer, behind any `wait` still
queued there, so a command line like `+wait 300 +selecthero collier +wait 30
+ui_close all` runs first and the picker opens after it, over a player who
stands still and dies. (`ui_close all` also closes the HUD view, which makes
the client game draw a slower HUD of its own.) A benchmark has to use
`in <seconds> <command>`, which doesn't block the buffer:

```
+set bot_minClients 8 +set sv_cheats 1 +map lqdm1
+in 40 "god 1" +in 45 "god 1" +in 50 "god 1" +in 55 "god 1"
+in 62 "ui_key enter" +in 72 "ui_key enter" +in 74 "god 1"
+in 75 "cmd setpos 1408 -128 28" +in 75 "setang_local 5 200"
```

`god 1` sets the flag rather than flipping it, so it can be repeated before
the player has surely spawned (about 40 s after boot in xemu). `ui_key
enter` presses A on the picker as a player would (the second press does
nothing once it is closed). The player then stands, invulnerable, in the
courtyard where the bots fight most, looking across it. Times are wall
time: under heavy host load the spawn can come later.

Two things make a match slower as it goes on, and neither is the engine's
to change: the game's `FindXRInput` spawns three NSXRInput entities each
time a player spawns or changes hero, about 20 a minute with 8 bots, and
never removes them (on desktop, 36 after 19 s of match time and 69 after
125 s). Its type test never matches, because it reads the high word of an
int64 `!=` result, which the VM leaves as the temp held it. The game relies
on that: its owner test is inverted, and with a full 64-bit write the
players took each other's XR inputs and the bots stopped scoring kills;
and sampling the guest with the QEMU monitor (`info registers`) slows it,
so take profiles in a run whose reports aren't the baseline.

## Installing on a console

This needs a console that runs unsigned code: one with a softmod (a
save-game exploit that installs a replacement dashboard) or a modchip. The
dashboard must have an FTP server; UnleashX, XBMC4Xbox and XBMC4Gamers all
do. Steps:

1. Connect the console to your network and note its address. The
   dashboard shows it.
2. Connect with an FTP client. The usual login is `xbox` / `xbox`. Use
   binary mode.
3. Make a folder for the game on a hard disk partition, for example
   `E:\Apps\Bulwark` (or `F:\Games\Bulwark` on a larger drive).
4. Upload the staged folder's contents into it: `default.xbe`, `args.txt`,
   `base/`, `stbuilt/` and `data/`. Copy the folder, not the ISO.
5. Start Bulwark from the dashboard's applications list.

Notes:

- **File names:** FATX, the Xbox's file system, allows at most 42
  characters in a name. Longer names fail to upload, and the engine then
  can't find those files. Stiletto's `base` has one such texture,
  `metal_samusd_001metal_samusd_001_col_temp.png`. Rename or drop it.
- **Writes:** the hard disk is writable, so `config.cfg`, `bulwark.log` and
  saves work there, in the game's own folder.
- **Quitting:** `quit`, and Quit on the title, go back to the dashboard.
  `Plat_Shutdown` calls nxdk's `XLaunchXBE(NULL)`, which sets the launch
  data to "launch the dashboard" and reboots. A softmod starts whichever
  dashboard it has installed. This has not been checked on hardware or in
  xemu yet.
- **Memory:** a retail console has 64 MB. Consoles upgraded to 128 MB give
  the game more room; the engine uses what the kernel reports.

## Controls

Controllers come through nxdk's SDL2 (its USB stack) as game controllers,
so the engine's controller bindings apply. The first time a controller
appears and none of its buttons are bound, it gets this layout
(`pad_defaults` in `cl_input.c`):

| Xbox controller | Engine key | Default |
|---|---|---|
| Left stick | left stick | move |
| Right stick | right stick | look |
| A | `GP_A` | jump |
| B | `GP_B` | duck |
| X | `GP_X` | reload |
| Y | `GP_Y` | use |
| Right trigger | `GP_RTRIGGER` | attack |
| Left trigger | `GP_LTRIGGER` | secondary attack |
| Black | `GP_RSHOULDER` | next weapon |
| White | `GP_LSHOULDER` | previous weapon |
| Left stick click | `GP_LTHUMB` | sprint |
| BACK | `GP_VIEW` | scores |
| START | `GP_MENU` | menu |
| D-pad | `GP_DPAD_*` | weapon slots 1 to 4 |
| BACK + START together | | console (caught in `vid_xbox.c`, before any binding) |

Stiletto's hero actions (`+hero_tool`, `+hero_melee`, `+hero_guard`,
`+groove`, `hero_action 0-2`) have no pad buttons by default. Bind them
under Settings, Buttons.

In menus the native UI uses these keys:

| Action | Keys |
|---|---|
| Move | D-pad, or the left stick (pressed past half way, released under a third). A held direction repeats after 400 ms, then every 100 ms. |
| Accept | A |
| Back | B |
| X | Ready (lobby); Refresh (server list) |
| Y | Settings (title) |
| START | closes the pause page; Start or Ready in the lobby |
| BACK | nothing (swallowed, so it doesn't show the scores) |

There is no text entry anywhere, because a console has no keyboard.

## Native UI

`BULWARK_UI_NATIVE` builds `src/ui/ui_native.c` as the `ui.h` backend.
Stiletto's WebCore pages are routed by `cl_webcore.c` exactly as on the PC,
and the native UI draws each of them in C through the 2D API:

| Page | Native screen |
|---|---|
| Title, and its Create and Join dialogs | `uin_title.c` |
| Lobby | `uin_lobby.c` |
| Pause | `uin_pause.c` |
| Hero picker | `uin_hero.c` |
| HUD | `uin_hud.c` |
| Loading, errors and button hints | `uin_misc.c` |

The design is in `src/ui/uin_local.h` and `uin_stiletto.h`. The game's
progs and data are not changed.

### Settings

The title's Settings opens the engine's own menu (`menu_options`,
`cl_emenu.c`). In native builds that menu is cut down to what a console
needs, and works from a controller alone:

- **Controller:** dead zone, response curve, turn and look speed, invert
  look and move speed (the `joy_*` cvars), and "Reset pad layout".
- **Buttons:** every action in the game's `scripts/kb_act.lst`, then any
  other command the configs bind (the "OTHER" group). Press A on a row,
  then the button to bind. START or BACK cancels, because bound to an
  action they would leave no way back to the menu or console. X clears a
  row's buttons.
- **Audio:** sound and music volume.
- **Video:** brightness (`r_lightmapscale`; nothing reads `vid_gamma`),
  field of view, crosshair, frame rate (`vid_xbox_swapinterval`) and the
  frame counter.
- **Game:** the player name, chosen from `scripts/bots.txt` since there
  is no keyboard, and "Reset everything to defaults".

B goes back a page, and the black and white buttons move ten rows at a
time. The settings are archived cvars and binds, so `config.cfg` keeps
them. On the Xbox the config is written as soon as the menus close, because
consoles are usually switched off rather than quit.

### The atlas cache

All of the UI's text and pictures come from one glyph and image atlas. At
H 432 that is two textures, 512×256 and 512×512, about 1.5 MB on the GPU.
A page grows if the art doesn't fit. It is baked from the
fonts and SVGs the manifest in `uin_stiletto.h` names. Stiletto's fonts are
commercial: they stay in Stiletto's data and are never committed to
Bulwark.

The bake depends on the layout height H, the screen height inside the
TV-safe inset: 432 on the Xbox (`ui_native_safe 0.05`), 480 without the
inset. Bake on the PC, where it is written to the writable game folder:

```sh
bulwark -basedir <stiletto> -game base -game stbuilt +ui_native_bake 432 +ui_native_bake 480 +quit
```

This writes `stbuilt/ui/native_<H>.txt`, `_p0.tga` and `_p1.tga`. Stage
them with the rest of `stbuilt/`. A PC build also bakes on first use, and
again when the manifest or the font files it resolved change.

An Xbox with no cache bakes in memory each time it starts, and logs
`ui: no atlas cache, baked at runtime`. It draws the SVGs at a lower
supersampling. To skip the 2.7 MB Iosevka file, it substitutes Trade Gothic
for that face. The cache is quicker and looks closer to the PC.

### Native UI cvars and commands

| Name | Default | Purpose |
|---|---|---|
| `ui_native_safe` | 0.05 on the Xbox, else 0 | TV-safe inset for the HUD and button hints, as a fraction of the screen |
| `ui_hud_style` | 0 | 0 draws the HUD as the PC port does, 1 adds the Figma extras |
| `ui_hud_debug` | 0 | outline the HUD's clusters |
| `ui_native_hudtest` | 0 | fixed HUD values, for screenshot comparisons |
| `ui_native_stats` | 0 | every 2 s, print the HUD's and the menus' average and worst time in µs, vertices, draw calls and atlas fill |
| `ui_native_bake [H]` | | bake and write the atlas cache for height H (default: the current one) |
| `ui_native_atlas [0\|1\|2]` | | show atlas page 0 or 1 at 1:1 |
| `ui_native_shot <name>` | | take a screenshot once the UI has drawn |
| `ui_font` | | font families, as in the Panorama UI |
| `ui_list` | | list the open views |
| `ui_key <key> [down\|up]` | | press a key through the normal input path, for scripted pad tests |
| `in_paddefaults` | | bind the controller's buttons to the default layout again |

Use `ui_native_stats 1` to measure the UI's per-frame cost on a console.
The targets are a HUD under 0.2 ms and menus under 1 ms per frame.

## Xbox settings

| Name | Default | Purpose |
|---|---|---|
| `vid_xbox_swapinterval` | 2 | vertical blanks per frame: 2 is a steady 30 fps, 1 is 60 |
| `gl_max_size` | 512 | largest side of a world texture; bigger ones are scaled down as they load |
| `gl_xbox_texquality` | 0 | 0 stores world textures at 16 bits (half the memory), 1 keeps 32 |
| `gl_xbox_mipmaps` | 1 | make mipmaps for textures that ask for them |
| `gl_xbox_aniso` | 1 | anisotropic filtering of mipmapped textures: 0 off, 1 2x, 2 4x, 3 8x |
| `gl_xbox_stats` | 0 | every 5 s, print the frame rate, frame and wait times, draws, triangles, push buffer use, GPU memory and free memory |
| `snd_xbox_rate` | 24000 | rate the mixer runs at, 24000 or 48000 |
| `sv_tickrate` | 30 | server ticks a second (72 on the desktop); players move by their own commands either way |
| `sv_maxticks` | 3 | most server ticks in one frame (no limit on the desktop); past it the world drops the time rather than catching up; players and bots still get the dropped time as movement, so only thinks, physics and projectiles slow |
| `xbox_meminfo` | | free memory, the kernel's pools, the C heap and GPU memory |
| `xbox_gpuinfo` | | GPU memory in use and the last frame's draws and triangles |

The engine's controller cvars apply as on the desktop. They are all on the
Controller settings page: `joy_deadzone` 0.2, `joy_curve` 2,
`joy_yawspeed` 220, `joy_pitchspeed` 160, `joy_invert` 0 and
`joy_movescale` 1.

## How it fits

| File | Contents |
|---|---|
| `sys_xbox.c` | start-up: `args.txt`, the log, memory figures, the fatal-error screen, quitting to the dashboard |
| `vid_xbox.c` | the screen (640×480) and controllers through SDL2 |
| `gl_xbox.c` | the renderer's OpenGL subset on the NV2A, written straight into pbkit's push buffer |
| `snd_xbox.c` | sound through the AC'97 codec |
| `posix_xbox.c`, `compat/` | the POSIX the engine expects: file names, threads, directories, clocks, sockets on lwIP |
| `fmt_xbox.c`, `math_xbox.c` | the parts of nxdk's C library that are stubs: float `printf`, `scanf` and `strtod`, `lrint`, `lround` and `log2` |
| `fs_xbox.cpp`, `cxxrt_xbox.c` | the C++ runtime pieces PanoramaEngine needs (Panorama builds only) |

A fatal error shows its message on screen, through pbkit's debug screen,
and waits for the console to be reset. The log in the game folder has the
rest.

### Renderer

`gl_xbox.c` recognises the renderer's five GLSL programs by their text and
runs each on the NV2A's fixed-function transform and lighting, with the
register combiners doing the fragment work. Fog is the hardware's
per-vertex exp2 fog, the same formula the shaders use. Differences from the
desktop:

- **Not drawn:** dynamic lights.
- **No framebuffer objects:** the renderer draws straight to the screen.
- **Textures:** resampled to powers of two, swizzled, and kept in
  contiguous memory. Mipmaps are made on the CPU. World textures are 16-bit
  unless `gl_xbox_texquality 1`. 2D pictures are always 32-bit.
- **Vertices:** streamed vertex data comes from a ring that alternates
  halves each frame. Skinned models stream only positions and normals; the
  rest of their vertices stays in GPU memory. Static index buffers become
  command runs in GPU memory the first time they are drawn, which the
  stream calls; streamed indices go inline, 16 bits each where the draw
  allows it.
- **Command stream:** a ring the CPU writes a frame ahead of the GPU in.
  It is handed to the GPU every 32 KB and before any wait for it, since
  each hand-over flushes the write-combine buffers. `gl_xbox_stats` also
  prints the hand-overs, the time spent waiting for the GPU, and how much
  a frame streams.

### Sound

The AC'97 codec plays 16-bit stereo at 48 kHz. Its completion callback
runs at DPC level, where floating point isn't allowed, so it only queues
the next ready buffer and wakes a mixing thread. The engine mixes at 24 kHz,
and the thread doubles that to 48 kHz by linear interpolation. That halves
the mixing work, and the game's samples are mostly 22 or 11 kHz anyway.
There are four buffers of 21 ms, so about 85 ms of latency.

### Network

nxdk's lwIP stack, started in a background thread with DHCP. System link
(LAN) games use it. `-nonet` leaves it off, and then every socket call
fails with `ENETDOWN`.

## Memory

`BW_LOWMEM` shrinks the engine's defaults, as on the 3DS
(`docs/platforms/3ds.md`, "Memory"): 1024 server, CSQC and menu edicts,
QuakeC heaps of 4, 4 and 2 MB, 4096 client entity slots, and a 512² font
atlas. `pr_maxedicts` and `pr_heapsize` still override the server's.

GPU memory comes out of the same 64 MB as everything else. Small objects
share 1 MB chunks, and `xbox_gpuinfo` shows the total. Use `xbox_meminfo`,
or `gl_xbox_stats 1`, to watch what is left.

The native UI keeps no heap of its own between frames. Its atlas is about
1.5 MB of GPU memory, and its vertex batch and tables are static data
(about 95 KB, by the design's estimate).

## Known limits

- **Memory:** in the bring-up runs in xemu (128 MB, Stiletto on `lqdm1`),
  loading ran out of memory even with `gl_max_size 128` and a smaller
  QuakeC heap. Getting a map to fit in 64 MB is the next piece of work.
  `xbox_meminfo` and `gl_xbox_stats` are there to measure it.
- **Not checked on hardware:** quitting to the dashboard, controller
  hot-plugging, and the frame rate. Everything so far is from xemu, which
  doesn't model the console's timing.
- **Dynamic lights** aren't drawn.
- **Online play:** no online lobbies, room codes or typed addresses, and no
  sending lobby chat (receiving works). System link is the only way to
  play with others.
- **Settings:** the pages a console can't use are left out of the menu.
  That covers renderer switches, resolutions, the mouse, voice chat,
  teamplay messages, mods, demos, downloads and the media player.
- **Entity growth:** see "In-match benchmark": the game leaks three
  entities a spawn. At the rate measured with 8 bots, a match of 15 to 20
  minutes would fill the 768 entity slots (not yet run that long).
- **DVD:** a read-only disc saves nothing. Run from the hard disk to keep
  settings.
- **FATX names:** at most 42 characters (see "Installing on a console").
