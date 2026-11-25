# Handoff: running Stiletto on Bulwark

Status as of 2026-10-27, branch `claude/project-thread-wbkro8` (the work
below up to `a49dae0` was on `claude/project-thread-upkyrb`).

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
other files → `layout/web/<dir>/<name>.xml`, and a page directly under
`data/web/` (`scratch.html`) → `layout/web/scratch.xml`. The query reaches
the script as `location.search`.

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
- **Ported pages, finished and verified:** every page under
  `data/web/` the game can load. `title-menu-v2` (the one Stiletto uses),
  `title-menu`, `pause-menu`, `lobby-menu`, `ql-menu` (duel selector plus
  the warmup/escape views) and `ql-menu/menu-demo`, `hud`, `h3-main-menu`
  (both pages, all five lobby modes, transitions, overlays, keys and mouse),
  `scratch` (the MGS4 mock-up), `webcore-test` (index and the six smoke
  pages), `test/simple` and `basemod-cef-test/menu`. Each was compared with
  a Chromium render of the original at 1920x1080; the differences left are
  fonts the game doesn't ship (stand-ins are named in each stylesheet) and
  the effects listed under Known limitations. `h3-main-menu-copy`, an older
  snapshot that fetched its data (which its own note says crashed FTE's
  loader), opens the `h3-main-menu` port; its plainer background gradient
  is not reproduced.
- **Checked:** no QC, cfg or header in the branch names a
  `basemod-wireframes*` page; they are browser-only prototypes and are not
  ported.
- **The half-finished revisions of `699e8d3`** (SVG art, `Bulwark.Width/
  Height`, `Bulwark.MouseX/MouseY`, the drawn `ql-menu` cursor) were opened,
  checked against the originals and kept.
- **Engine fixes found while testing:**
  - Q1 item boxes (`maps/b_*.bsp`) are now drawn.
  - Fog uses Quake's density convention; LibreQuake maps were white.
  - An entity wearing the world model broke visibility, so maps vanished
    in game.
  - Images named in `.mat` files are looked up under `textures/`.
  - `serverkey("mapname")` works.
  - A page closing itself no longer crashes the engine.
  - Fonts register before the title opens.
  - UI text was drawn too thin: atlas glyphs are premultiplied but were
    blended as straight alpha.
  - SVG `<mask>` is drawn (nanosvg patch plus `r_svg.c`); the HUD drew its
    mask shapes as solid art.
  - File searches let `*` cross folders, so hero defs loaded before
    `player.def` and a duel never spawned anyone. A host plus guest duel
    through the lobby now plays to a result: with the guest using `kill`
    each round, the arena went Round 1 → 2 → 3 → MATCH END at 3-0 and the
    host showed "player WINS THE MATCH".
  - A second copy on one machine no longer takes the lobby port for its
    game server.
  - `webcore_query <request>` prints the bridge's reply, for tests.
- **Game-mode matrix:** deathmatch, teamdm, domination, lastmanstanding,
  invasion, singleplayer, duel and dm_lobby ran for 60 s or more each with
  bots on Q1, Q3 and FTEW maps, with no engine errors. Remaining log
  warnings are Stiletto content (unknown entity classes, missing
  SoundDefs, `sentences.txt`, `commandmenu.txt`).

- **Online match flow** (docs/spec/setflow.md): Find Match on the title,
  the online hub with the fighter card, search and train-while-searching,
  the opponent card, blind hero select, first-to-2 sets with the loser's
  counterpick, the rematch vote (runbacks), forfeits, and records on Bulwark
  Live. `tests/live/setflow_ui.py` plays it with two windowed clients and
  key presses only. Stiletto's side is set play in `rules/arena.qc` (nuclide
  branch `claude/project-thread-mk4lnf`).

## Not finished

1. **Online room codes work on the LAN only**, found by discovery; there is
   no internet rendezvous service. Spec 9.13 sketches the reference's
   broker/ICE design if this is wanted.
2. **Ragdolls and `brush_*`** are listed above.
3. **Observed, not investigated:** in the two-client duel both players were
   named "player" although each cfg ran `seta name ...` first (possibly the
   archived-cvar reset noted below). The dialog seen at the match end
   ("Stiletto Swift / Proto / Tomboy Princess", OK) is Stiletto's message of
   the day (duel's `motd.LoadDefault`); set play and training skip it.

## Known limitations, not fixed

- PanoramaEngine hit-testing ignores transforms. Ports compute positions
  instead of scaling panels. `h3-main-menu` and `scratch` scale their stage
  down on views narrower than 16:9, where mouse hits on `h3-main-menu` miss.
- No CSS filters, masks or backdrop blur. `h3-main-menu`'s overlay has no
  blur behind it; `scratch` draws its icon glow into the SVGs and adds the
  brightening of its drop-shadow filter back with an additive layer.
- Layout quirks ports allow for (transitions from `none`, `squish`,
  background-less buttons, wrapped labels, children inside borders, label
  baselines) are listed in `docs/spec/ui.md`.
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

1. Look into the two observations in item 3 of Not finished.
2. If wanted: ragdolls, an internet lobby rendezvous, transform-aware
   hit-testing in PanoramaEngine, and CSS filters.
