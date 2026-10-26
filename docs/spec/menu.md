# Menu progs (`menu.dat`)

`src/client/cl_menu.c` runs a third QuakeC module beside the server and
client games: the front end, which exists with or without a map loaded.
Nuclide ships one as `base/menu.dat` (source in `src/menu-vgui`).

## Sources

- The entry-point doc comments in Nuclide's `src/common/fteextensions.qc`
  (lines 750-765 at the time of writing).
- Nuclide's menu QC (`src/menu-vgui/main.qc`, `src/platform/*.qc`), which
  shows how a real menu uses them.
- Probe runs of `base/menu.dat` in Bulwark, below.

No other engine's source was read.

## Loading

At startup, and on `menu_restart`, the engine loads `menu.dat` from the
search path into its own VM. Builtins bind by name, searching in order:
the menu-only table, the 2D drawing table, sound, the core and extra
tables, and the client game's table (for `clientstate`, key bindings and
the like). Missing builtins print a warning once and return 0.

The menu-only builtins are `setkeydest`, `getkeydest`, `setmousetarget`,
`getmousetarget` and `registercommand`.

After loading, the engine calls `m_init()` and then
`Menu_RendererRestarted("")` when there is a window.

If the command line does not start a game (no line starting `map `,
`connect ` or `playdemo `), the engine then calls `m_toggle(1)` so the
front end opens on its menu.

## Each frame

After the client game draws:

- While a connection is under way (connecting or loading, not yet in the
  game), `m_drawloading(screensize, 0)` is called.
- Otherwise `m_draw(screensize)` is called **every frame, with or without
  focus**. fteextensions.qc says so ("Will be called even if the menu does
  not have focus"), and Nuclide relies on it: `Platform_Draw` decides
  itself whether to draw, and takes the key focus back when a background
  map is running (`_background` cvar).

`time` and `frametime` globals are set before every call.

## Focus and input

`setkeydest(2)` (menu) or `setkeydest(3)` (menu, grabbed) gives the menu
the key focus; `setkeydest(0)` returns it to the game. While the menu has
focus:

- key, mouse delta and absolute mouse events go to `Menu_InputEvent`
  before the client game sees them;
- an event is consumed when the function returns non-zero, or always in
  the grabbed mode;
- the client game's `CSQC_UpdateView` is told the menu is up
  (`notmenu = 0`), and the console overlay is not drawn.

`togglemenu` (Esc when unbound) calls `m_toggle` with 1 when the menu is closed and 0 when it is open.

Commands the menu passed to `registercommand` are offered to
`m_consolecommand(line)` before the client game, the server game and the
engine's own commands.

## Observed with Nuclide's `base/menu.dat` (2026-10-26)

- It loads and runs `m_init`, registers its commands, precaches six fonts
  and queues `map_background background01` through `localcmd`.
- With the background map running, the main menu (Console, New game,
  Tutorial, ...) draws over the world.
- Unimplemented builtins it calls: the host cache family (server browser:
  `resethostcachemasks`, `sethostcachemaskstring`, `sethostcachesort`,
  `refreshhostcache`, `resorthostcache`, `gethostcachevalue`,
  `gethostcachestring`, `gethostcacheindexforkey`), `getgamedirinfo` and
  `gecko_get_texture_extent`. The menu works without them; the server
  browser and the custom game list are empty.
- Its configs use `alias`, `plug_load`, `cfg_load` and `cfg_save`, which
  Bulwark does not have yet.

### Testing note

`localcmd` appends to the command buffer, so a menu's startup commands run
after everything given on the command line, waits included. To screenshot
the menu over its background map, start the map yourself:

```
DISPLAY=:99 build/bulwark -basedir <nuclide> -game base -frames 800 \
  +set sv_background 1 +set g_gametype singleplayer +set coop 1 \
  +set sv_playerslots 2 +map background01 +wait 600 +screenshot
```

Input through the menu (clicks, Esc) has not been driven by a test yet.
