# Handoff: running Stiletto on Bulwark

Status as of 2026-10-27, branch `claude/project-thread-upkyrb`.

## The goal and the rules

Make Stiletto run correctly in Bulwark. Stiletto is the Nuclide mod at
`sokkycakes/nuclide`, branch `claude/project-thread-11jsb5`, game dir
`base/`. On FTE it drew its menus and HUD as HTML pages through an FTE
plugin called webcore. Two requirements from the owner:

- **Change only Bulwark.** Nothing in the nuclide repo may be modified,
  including new files. Engine runs must write into a separate game dir
  (see Testing) so screenshots and configs don't land in `base/`.
- **All WebCore UI becomes Panorama UI.** Bulwark has no HTML engine, so
  every page is ported to a Panorama layout shipped in Bulwark's `data/`.

Bulwark's clean-room rules apply (`CLEANROOM.md`): never read FTEQW
engine or plugin source. Stiletto's own files (QC, HTML, JS, CSS, configs,
notes) are fine. A few files in the nuclide branch embed FTE code and were
deliberately not read: `docs/verification/2026-09-12-figma-lobby-engine.patch`,
`Tools/stiletto/install_engine.py`, `_lobby_diag/**/*.py`, and the code in
`wiki/okf/log.md`.

## Where things are

| Area | Files | Reference |
|---|---|---|
| WebCore layer: page URL → Panorama layout, menu surface, `gecko_*`, `hud_drawnative`, the `fte_query` bridge, title backdrop map, menu music | `src/client/cl_webcore.c` | `docs/spec/webcore.md` (what Stiletto expects, written from its files) |
| Lobby: sessions, UDP protocol, ready/countdown/chat/start | `src/client/cl_lobby.c`, `lobby.h` | `docs/spec/lobby.md` |
| Panorama views: bridge, embedded views, view size and pointer, fonts, `{game}/` paths, `data:` images, deferred close | `src/ui/ui_panorama.cpp`, `ui.h` | `docs/spec/ui.md` |
| `font-family` patch to PanoramaEngine | `third_party/PanoramaEngine/src/panorama_style.cpp`, `panorama_font_atlas.cpp`, `include/.../panorama_style.hpp` | `third_party/README.md` |
| Panorama ports of the pages | `data/panorama/{layout,styles,scripts}/web/` | each layout's header comment |
| Font families the pages use | `data/panorama/webcore_fonts.cfg` (`ui_font` lines) | |
| Bulwark's own data dir on the search path | `src/common/fs.c` (`FS_SetEngineDir`, `-enginedata`) | `docs/spec/ui.md` |
| Builtins added for Stiletto | see the list in `docs/spec/builtins.md`, "Added for Stiletto" | |
| Renderer additions | `r_skin.c` (custom skins, body groups, `remapshader`, `r_readimage`), `r_svg.c` + `third_party/nanosvg`, `r_rtlights.c` (QC dynamic lights), `r_model.c` (BSP files as models) | |

URL mapping: `fte://data/web/<dir>/index.html?q` → `panorama/layout/web/<dir>.xml`,
and other files → `layout/web/<dir>/<name>.xml`. The query reaches the
script as `location.search`.

## Done

- **Every builtin Stiletto calls exists**, except `brush_*` (FTE raw-map
  editing; no Stiletto content uses it) and `skel_ragupdate` (ragdolls;
  only used with the `bodyque_forceRagdolls` cvar). `pr_missing
  [ssqc|csqc|menuqc]` lists unbound builtins for a loaded progs; use it
  after any Stiletto update.
- **WebCore behaviour:** `plug_load webcore` is answered by the engine,
  `forceqmenu 1` skips `menu.dat`, the title opens at start-up and after a
  disconnect over `webcore_menumap`, Esc and `togglemenu` follow spec 4.3,
  there is a menu music playlist, and every bridge request in spec 3 works.
- **Lobby:** create, join by address or room code on the LAN, ready,
  countdown, cancel, chat, map/ruleset/limits, start. The host runs a
  listen server and guests connect. Verified with two processes on one
  machine.
- **Ported pages, finished and verified:** `title-menu-v2` (the one
  Stiletto uses), `title-menu`, `pause-menu`, `lobby-menu`, `ql-menu` (duel
  selector plus the warmup/escape views), `hud`, `webcore-test`
  (index). Each was compared with a Chromium render of the original.
- **Engine fixes found while testing:**
  - Q1 item boxes (`maps/b_*.bsp`) are now drawn.
  - Fog uses Quake's density convention; LibreQuake maps were white.
  - An entity wearing the world model broke visibility, so maps vanished
    in game.
  - Images named in `.mat` files are looked up under `textures/`.
  - `serverkey("mapname")` works.
  - A page closing itself no longer crashes the engine.
  - Fonts register before the title opens.
- **Game-mode matrix:** deathmatch, teamdm, domination, lastmanstanding,
  invasion, singleplayer, duel and dm_lobby ran for 60 s or more each with
  bots on Q1, Q3 and FTEW maps, with no engine errors. Remaining log
  warnings are Stiletto content (unknown entity classes, missing
  SoundDefs, `sentences.txt`, `commandmenu.txt`).

## Not finished

1. **Half-finished revisions in the last commit (`699e8d3`).** Four port
   agents were stopped mid-edit while updating `hud`, `title-menu-v2` /
   `title-menu`, `ql-menu` and `lobby-menu` to use SVG art,
   `Bulwark.Width/Height`, `Bulwark.MouseX/MouseY` and a drawn cursor in
   `ql-menu`. Open each page and check the log for JS errors; finish or
   revert per file (`git diff 699e8d3~1 699e8d3 -- data/`). The smoke flow
   below still worked after the stop.
2. **`h3-main-menu` port barely started.** It is reachable with
   `set webcore_menu_url "fte://data/web/h3-main-menu/index.html"`. Also
   still to do: `h3-main-menu-copy` (decide whether it differs),
   `test/simple.html`, `basemod-cef-test/menu.html`, and the other
   `webcore-test/*.html` pages. The `basemod-wireframes*` folders are
   browser-only prototypes, not loaded by the game; no QC/cfg check was
   finished to confirm that.
3. **Online room codes work on the LAN only**, found by discovery; there is
   no internet rendezvous service. Spec 9.13 sketches the reference's
   broker/ICE design if this is wanted.
4. **Ragdolls and `brush_*`** are listed above.

## Known limitations, not fixed

- PanoramaEngine hit-testing ignores transforms. Ports compute positions
  instead of scaling panels.
- `Panel.style` from script only sets background, width, height, opacity
  and transform. There is no CSS `mask-image`, no skew, no circular
  clipping, and `<Image scaling="stretch-to-fit-preserve-aspect">` stretches.
- A focused Button is also activated by Enter, on top of the page's own key
  handling; the pages refocus the root after clicks.
- `onactivate` fires on disabled panels; pages guard against it.
- In scripted runs, `+wait` chains on the command line delay the game's
  autoexec. Use `in <secs> <cmd>` in a cfg to time steps.
- Archived cvars set with `+set` can be overwritten later in start-up; one
  agent saw `lobby_readytime` reset. It was not reproduced with a plain
  `+set` + `echo` test, so the cause is unconfirmed.
- The lobby's "Starting in N" uses QuickJS `Date.now()`; under `-frames`
  runs it can disagree with the engine's clock.
- A stray `unknown command "settings"` appears at start-up; the source was
  not found.

## Building and testing

Build as in the README (`cmake -S . -B build && cmake --build build`;
install `libsdl2-dev` for a window).

**Stiletto's progs:** the nuclide repo gitignores `*.dat`, the checked-in
ones are stale, and `progs/duel.dat` and `dm_lobby.dat` are missing.
Compile from source into a copy, never into the repo:

1. Build FTEQCC: clone fteqw and run `make qcc` in `engine/qclib`. Bulwark's
   rules allow building the compiler as a tool; don't read the engine code.
2. Copy `src/` and `base/src/` of the nuclide branch to a scratch tree with
   the same layout. Run `make QCC=<fteqcc>` in each of `base/src/{client,hud,server,menu,rules,maps}`
   (make `base/maps/` first).
3. Point an overlay game dir at the results.

**Test base dir** (keeps writes out of the nuclide repo):

```
B=/some/scratch/bw
mkdir -p $B/stbuilt/progs $B/stbuilt/maps $B/out
ln -s /path/to/nuclide/base $B/base
# link the compiled csprogs.dat hud.dat menu.dat progs.dat into $B/stbuilt,
# progs/*.dat into $B/stbuilt/progs, maps/*.dat into $B/stbuilt/maps
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 ./build/bulwark -basedir $B -game base -game stbuilt -game out -frames 1500 +exec flow.cfg
```

Screenshots go to `$B/out/screenshots/`. Useful commands: `ui_list`,
`ui_eval <view> <js>`, `ui_key <key>`, `ui_click <x> <y>`, `menu_webcore <url>`,
`r_speeds 1`, `pr_missing csqc`, `mapedit <map>`, `modelviewer <model>`.
Stiletto's server registers `addBot <profile> [skill]` (profiles via
`listBotProfiles`). Use `+set webcore_menumap ""` to skip the backdrop map.

**Smoke flow** (`flow.cfg` in the out dir): title → Create Lobby → LAN →
ready → game → pause:

```
set lobby_readytime 3
in 12 ui_key ENTER
in 13 ui_click 568 394
in 15 screenshot
in 16 lobby_ready 1
in 17 screenshot
in 30 screenshot
in 32 togglemenu
in 33 screenshot
```

**Two-client lobby:** run a second process with its own out dir and
`+lobby_join 127.0.0.1`. The lobby uses UDP 27501 and the game 27500, so
only one such test at a time.

## Suggested next steps

1. Finish or revert the half-done port revisions (item 1).
2. Port `h3-main-menu` and the small test pages; confirm nothing loads the
   `basemod-wireframes` pages.
3. Run a full duel between two clients through the lobby. The arena selector
   flow has only been tested with one client.
4. If wanted: ragdolls, an internet lobby rendezvous, and transform-aware
   hit-testing in PanoramaEngine.
