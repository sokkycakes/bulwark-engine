# WebCore menu/HUD bridge and pre-game lobby: behavioural specification for Bulwark

## In Bulwark

Bulwark has no HTML engine. It runs these pages as Panorama views
([ui.md](ui.md)): `fte://data/web/<dir>/index.html` names the layout
`panorama/layout/web/<dir>.xml` (other files `layout/web/<dir>/<name>.xml`,
and a page directly under `data/web/` such as `scratch.html` is
`layout/web/scratch.xml`), and `data/panorama/` in Bulwark's source tree
holds Panorama ports of every Stiletto page: `title-menu-v2`, `title-menu`,
`h3-main-menu` (and its older `h3-main-menu-copy`), `pause-menu`,
`lobby-menu`, `ql-menu` (and `menu-demo`), `hud`, the `scratch` MGS4 mock-up,
and the `webcore-test`, `test` and `basemod-cef-test` smoke pages. The
`basemod-wireframes*` folders are browser-only prototypes that nothing in
the game loads, and are not ported. They use the game's own art,
fonts and scripted data in place (`{game}/...`) and send the same
`fte_query` requests as the HTML pages. `src/client/cl_webcore.c` is the
menu surface, the `gecko_*` builtins and the bridge below; the lobby is
`src/client/cl_lobby.c`, with Bulwark's own wire format in
[lobby.md](lobby.md). `plug_load webcore` (or `cef`) is answered by the
engine and turns the HTML menus on.

Where Bulwark's choices fill this document's open questions: host Start
works without everyone ready (9.7); `arena_action:join` is `cmd play`
(8.4); `g_gametype` is the ruleset in lower case (9.12);
`lobby_readytime` defaults to 5; the title map keeps running under the
lobby page and is dropped when the game starts; the menu music stops at
the first missing `menuN` and goes round again; the pause page is
`webcore_pause_url`; `cbuf:` commands with spaces are refused. Online room
codes are found on the local network only: there is no internet
rendezvous service.

The rest of this document is the specification as written before the work,
from Stiletto's files; it is kept as the reference the code follows.

Status: clean-room behavioural contract, written 2026-10-27.
Target: the Bulwark engine (ISC) running the game "Stiletto" (Nuclide `+game base`).

This document describes what the Stiletto content expects from the custom FTEQW
plugin "webcore" (an in-process HTML renderer and its JavaScript bridge) and from
FTE's pre-game lobby/session system. It describes only observable behaviour:
command names, cvars, request/reply strings, JSON shapes, state machines, timing
and network-visible behaviour, and which game files are loaded when. It contains
no engine code.

## 0. Provenance and conventions

### 0.1 How this was established

Everything here comes from game-side material: Nuclide QC (a client of the engine),
the web pages under `base/data/web/`, configs, plans and notes under `docs/`,
the project wiki, and the game-side test harnesses under `Tools/` (PowerShell and
Python scripts that drive the shipped executable and its HTML host DLL). No FTEQW
engine or plugin source was read. Section 12 lists the sources for each area.

Each statement carries one of these provenance tags:

| Tag | Meaning |
|---|---|
| **[SRC]** | Read directly from game content, a game test, or a project doc that describes shipped behaviour. |
| **[INF]** | Inferred from how the content uses the engine. It is very likely true, but no document states it. |
| **[PROPOSED]** | Not established from any source. This is a design proposal for Bulwark, made so that the contract is complete. Bulwark can choose differently, provided the page and QC behaviour still holds. |
| **[UNCERTAIN]** | The sources conflict or are silent. The open question is stated. |

"MUST", "SHOULD" and "MAY" are used in the RFC 2119 sense.

### 0.2 Glossary

- **Page**: an HTML document loaded from `fte://data/web/...`.
- **View**: one live HTML renderer instance with its own size and pixels.
- **Menu surface**: the single fullscreen, interactive view that the engine owns
  (title, lobby, pause and arena pages).
- **HUD view**: a second, non-interactive view that CSQC creates through `gecko_*`
  and draws with `drawpic`.
- **Bridge**: the synchronous JavaScript function `window.fte_query(string) -> string`
  that the engine injects into every view.
- **In game**: the client is connected to a real game server, meaning a server whose
  serverinfo does **not** have `background` = `1`.
- **Disconnected / title context**: no connection, or a connection only to the
  local title backdrop map (serverinfo `background` = `1`).
- **Lobby session**: the engine's mapless pre-game party (section 9), independent of
  any game server.

## 1. Runtime pieces and boot

### 1.1 What the reference shipped

- FTE plugin `webcore` (`fteplug_webcore_x64.dll`) loads a host library
  (`ftewebcore.dll`, a WebKit/WebCore build with a Cairo CPU renderer). [SRC]
- The host requires ICU data at `<exe dir>/resources/icudt67l.dat`. Without it,
  `@font-face` parsing in the HUD crashed. This is a packaging fact, not a Bulwark
  requirement, unless Bulwark also uses ICU. [SRC]
- Bulwark already has a Panorama-style UI (`base/panorama/scripts/titlemenu.js`
  calls `Bulwark.Cmd("lobby_create_lan")`). This spec covers only the WebCore-facing
  contract. A Bulwark implementation can host these HTML pages with any HTML engine,
  provided the behaviour below holds.

### 1.2 `base/autoexec.cfg` (executed at startup) [SRC]

In order:

| Line | Bulwark behaviour required |
|---|---|
| `seta con_savehistory 1` | Standard. |
| `set slint_menu 0`, `set slint_ui 0`, `set slint_hud 0` | Slint is not present. These cvars MUST be accepted (creating them is enough). `slint_hud 0` exists so that CSQC owns the HUD. |
| `set webcore_hud 1` | CSQC autocvar: enables the HTML HUD (section 6). |
| `set webcore_hud_url "fte://data/web/hud/index.html?v=9"` | CSQC autocvar: HUD page URL. |
| `set cl_webcore_arena_ui 1` | CSQC autocvar: routes duel/arena team and hero selectors to the HTML arena page (section 8). |
| `set webcore_menu_url "fte://data/web/title-menu-v2/index.html"` | Engine cvar: title page URL (section 4). |
| `plug_load webcore` | MUST be accepted. In Bulwark it can be a no-op that reports success, or it can initialise the HTML subsystem. |
| `set forceqmenu 1` | FTE meaning: do **not** load MenuQC (`menu.dat`). Bulwark MUST honour this: no MenuQC runs, so no `m_init`, and therefore `lobby_registermode`/`lobby_addteam` are never called (section 9.11). |
| `menu_restart` | Restart the menu subsystem. With `forceqmenu 1` this re-initialises the engine's own menus, and the WebCore title is (re)pushed under the rules in section 4.4. |
| `seta webcore_menumap "background02"` | Engine cvar: title 3D backdrop map (section 4.7). |
| model, `setinfo` and `bind` lines | Standard. |

`base/configs/Default.cfg` and `base/default_controls.cfg` bind `ESCAPE "togglemenu"`
and `` ` `` / `~` to `toggleconsole`. `Default.cfg` also has
`seta webcore_menumap "background02"`. [SRC]

### 1.3 Cvars that must exist before QC writes them

CSQC writes the following with `cvar_set`. Some are never declared anywhere
(they are not autocvars). In FTE, setting a cvar that does not exist is unreliable.
Bulwark MUST either create a cvar on first `cvar_set`, or pre-register all of these
as empty-string user cvars [INF]:

- `webcore_arena_snapshot`, `webcore_arena_hero_ids`
- `webcore_hud_timer`, `webcore_hud_health`, `webcore_hud_health_max`,
  `webcore_hud_ammo`, `webcore_hud_reload`, `webcore_hud_score_left`,
  `webcore_hud_score_right`, `webcore_hud_player1`, `webcore_hud_player2`,
  `webcore_hud_speed`, `webcore_hud_charges`, `webcore_hud_clip`,
  `webcore_hud_ammo_max`, `webcore_hud_drive`, `webcore_hud_drive_max`,
  `webcore_hud_ex`, `webcore_hud_ex_max`, `webcore_hud_guard`,
  `webcore_hud_guard_hp`, `webcore_hud_guard_hp_max`, `webcore_hud_groove`,
  `webcore_hud_mode`
- The HUD page also reads `webcore_hud_cooldown_0..2`. No QC writes these today.

## 2. Hosting the pages (applies to every view)

### 2.1 URL scheme [SRC]

- Pages are addressed as `fte://data/web/<dir>/<file>`. The authority `data` and the
  path together are a path in the game virtual filesystem:
  `fte://data/web/hud/index.html` maps to the game file `data/web/hud/index.html`
  (on disk, `base/data/web/hud/index.html`). The Nuclide test harness resolves URLs
  exactly this way: `<gamedir>/<netloc>/<path>`, with the path URL-decoded and
  kept inside the game directory.
- The query string (`?v=9`, `?mode=arena`, `?v=16`) MUST be ignored when resolving
  the file. It MUST remain visible to the page through `location.search`.
  `ql-menu` reads `?mode=arena`.
- Relative references (`style.css?v=4`, `app.js`, `assets/...`) resolve against the
  page URL.
- Content is trusted-local only. Remote `http(s)` navigation is not required and
  SHOULD be refused. [SRC: requirement R5]
- `fetch()` of a relative `fte://` resource MUST work (`webcore-test/app.js` fetches
  `smoke.json`). [SRC] The H3 page notes that fetch crashed the reference host and
  embeds its data instead. Bulwark simply needs fetch to work or fail cleanly.
- MIME type is chosen by file extension.

### 2.2 Rendering features the pages rely on [SRC unless marked]

- **Transparency.** Every page sets `html, body { background: transparent }`. All views
  composite with premultiplied alpha over whatever is behind them: the 3D backdrop
  on the title, the world in game. Mock-ups with white backgrounds mean transparent.
- **Canvas 2D** (`hud`, `ql-menu`), including `drawImage` from in-document `<img>`
  elements whose `src` is a `data:` URL, `fillText` with CSS font families,
  `globalAlpha`, and `clearRect`.
- **`data:` URLs** for images (PNG, JPEG, SVG) and fonts (TTF and OTF in
  `@font-face src: url("data:font/...;base64,...")`). `title-menu-v2/style.css`,
  `title-menu/style.css`, `hud/style.css` (3.7 MB, 7 faces) and `ql-menu/fonts.css`
  use `@font-face` with data URLs. Bulwark MUST support this.
- **Named fonts without `@font-face`.** The lobby page uses family names that the
  reference plugin registered privately from loose TTF files at view-host start.
  Bulwark MUST make these families available to every view:

  | Family name used by CSS | File (game VFS) |
  |---|---|
  | `Trade Gothic Next LT Pro BdCn` | `data/web/hud/assets/fonts/TradeGothicNextLTProBdCn.ttf` |
  | `standard 07_57` (note the space) | `data/web/ql-menu/assets/standard_07_57.ttf` |
  | `Inter` (Regular and Bold) | `data/web/lobby-menu/assets/fonts/Inter-Regular.ttf`, `Inter-Bold.ttf` |

  Other families (`Bahnschrift`, `Segoe UI`, `Arial Narrow`, `Consolas`, `Trade Gothic`)
  are only system fallbacks.
- `document.fonts.load(...)` returning a Promise: HUD and ql-menu wait on it before
  their first render and also proceed on rejection.
- CSS: `%` and absolute sizing; `transform: scale()`, `transform-origin`;
  `linear-gradient`; `-webkit-mask-image` with `repeating-linear-gradient`;
  `transition` (opacity 180 ms, top 100 ms); `calc()` with custom properties; flexbox;
  `matchMedia('(prefers-reduced-motion: reduce)')` (MAY report false).
  Pages avoid `vw`/`vh` because the reference host returned 0 for them. Bulwark
  SHOULD still implement them correctly.
- JS: ES2017+ (arrow functions, `const`/`let`, spread, template strings,
  `URLSearchParams`, `Promise.all`), `requestAnimationFrame` (driven every engine
  frame while the view is alive), `setTimeout`, `setInterval`.
- Layout metrics: `document.documentElement.clientWidth/clientHeight` MUST equal the
  view's pixel size. `window.devicePixelRatio` MAY be 1. The `resize` event MUST fire
  when the view is resized.
- **Text input.** `<input type="text">` and `<input type="number">` MUST be editable:
  printable characters, Backspace, Delete, arrow-key caret movement, selection,
  Ctrl+A/C/X/V (Ctrl+C/X/V use the OS clipboard), `maxlength`, `disabled`, Enter
  key events, `beforeinput`, `input` and `paste` events, and `preventDefault`
  suppressing insertion.
- **Text selection by mouse on non-editable text MUST NOT start.** The reference host
  disabled text interaction after mouse-down on plain text crashed it. Pages also set
  `user-select: none`.
- **DOM keyboard events** carry standard `key` values (`ArrowUp`, `ArrowDown`,
  `ArrowLeft`, `ArrowRight`, `Enter`, `Escape`, `Tab` with `shiftKey`, `' '`, letters,
  `F3`) and `ctrlKey`/`altKey`/`metaKey`. `keydown` MUST fire once per press, with
  key repeat allowed, followed by character input for printable keys.
- **Mouse events**: `mousemove`, `mouseenter`, `click`, `mousedown`/`mouseup`, `wheel`
  (`deltaY` > 0 means down), and `getBoundingClientRect` in view pixels. Hit-testing
  uses view pixel coordinates.
- `console.warn`, `console.error` and `console.info` SHOULD go to the engine console
  (developer-only).

## 3. The JavaScript bridge: `window.fte_query`

### 3.1 General semantics

- The engine injects a global function `fte_query(request: string) -> string` into
  **every** view: the menu surface and every `gecko_*` view, including the HUD. [SRC]
  Pages test `typeof window.fte_query === "function"` to tell engine from browser.
- The call is **synchronous**: it returns the reply string immediately. Pages wrap
  it in `try/catch` and treat `null`, `""` or an exception as "unavailable". [SRC]
- Requests are plain strings of the form `<kind>` or `<kind>:<argument>`.
  One exception: the HUD's fallback `get_cvar <name>` uses a space. [SRC]
- Size limits: a request whose UTF-8 size exceeds 4096 bytes MUST be rejected. Its
  reply is `""` and it has no side effect. [SRC] Replies MAY be any length. The
  arena snapshot and lobby snapshot can be a few KB.
- Each call has **exactly one** side effect. The reference host called the query
  callback twice, once to size the reply, and side effects ran only once. The
  Bulwark API should not have this problem at all. [SRC]
- Commands queued by the bridge (`cbuf:` etc.) are **appended to the console command
  buffer** and executed on the next command-buffer run (that frame or the next). They
  are not executed re-entrantly inside the JS call. Order across consecutive
  `fte_query` calls MUST be preserved. For example, the title page queues
  `lobby_create_lan` and then `menu_webcore_lobby`. [INF]
- Unknown request kinds MUST return `""`. Pages handle this. [PROPOSED]
- The HUD page calls the bridge several times every animation frame. The bridge
  MUST be cheap: no blocking I/O, except `hostcache_refresh` (section 3.10). [SRC]

### 3.2 Request index

| Request | Reply | Used by |
|---|---|---|
| `cbuf:<command>` | `ok` or something else | title-menu, title-menu-v2, pause-menu, ql-menu, lobby-menu |
| `localsound:<sample>` | ignored (`ok` suggested) | title-menu-v2, lobby-menu |
| `clipboard_copy:<text>` | `ok` / anything else | lobby-menu |
| `cvar:<name>` | cvar string value or `""` | hud |
| `get_cvar <name>` | same as `cvar:` | hud (fallback) |
| `getstats` | JSON array | hud (fallback), webcore-test |
| `gethud` | JSON object or `""` | hud |
| `getarena` | JSON object | ql-menu (arena mode) |
| `arena_action:<action>` | JSON `{"ok":bool,...}` | ql-menu (arena mode) |
| `hostcache_refresh` | ignored (`ok` suggested) | title-menu, title-menu-v2 |
| `gethostcache` | JSON array | title-menu, title-menu-v2 |
| `lobby_join:<target>` | `ok` / anything else | title-menu, title-menu-v2 |
| `getlobby` | JSON object | lobby-menu |
| `getlobbychat` | JSON array | lobby-menu |
| `lobby_action:<action>` | JSON `{"ok":bool,...}` (`ok` also accepted) | lobby-menu |

The sections below specify each request.

### 3.3 `cbuf:<command>`: allowlisted console command

- Behaviour: if `<command>` exactly matches an allowlisted string, append
  `<command>\n` to the command buffer and reply `ok`. Otherwise reply something else
  (for example `denied`) and do nothing. [SRC: pages compare `=== "ok"`; plans say
  the allowlist is exact and single-token]
- The allowlist MUST be an exact-string match. Arbitrary text from JS MUST NOT be
  executable. [SRC: requirement R9]
- Strings the shipped pages send:

  | String | Sent by | Meaning |
  |---|---|---|
  | `menu_options` | title-menu, title-menu-v2, pause-menu, ql-menu | Open the engine's settings UI (section 4.9). |
  | `quit` | title-menu, title-menu-v2 | Quit. |
  | `lobby_create_lan` | title-menu, title-menu-v2 | Section 9.3. |
  | `lobby_create_online` | title-menu, title-menu-v2 | Section 9.3. |
  | `menu_webcore_lobby` | title-menu, title-menu-v2 | Show the lobby page (section 4.2). |
  | `menu_lobby_create` | (`data-cmd` on Create; not reached by current JS) | Legacy: `lobby_create_lan` then show the lobby page. |
  | `menu_webcore_title` | lobby-menu (Leave) | Show the title page. |
  | `menu_webcore` | lobby-menu (fallback if the above is refused) | Show the default page for the context. |
  | `webcore_closemenu` | pause-menu, ql-menu | Close the menu surface. |
  | `menu_hostmenu`, `menu_changeteam`, `menu_callvote` | pause-menu, ql-menu | Engine and game sub-menus (section 4.9). |
  | `lobby_close` | pause-menu (Leave Game) | Leave any lobby session. |
  | `disconnect` | pause-menu, ql-menu | Standard disconnect. |
  | `ready` | ql-menu (non-arena warmup) | Forwarded to the server as a client command. |
  | `map_restart` | ql-menu (non-arena Restart confirm) | Standard. |
  | `team s`, `team r`, `team b`, `team a`, `team red`, `team blue`, `team free` | ql-menu (non-arena demo panels only) | Contain spaces. [UNCERTAIN] The reference allowlist was "single-token", so these were probably refused. Nuclide has no `team` command; it uses `joinTeam`. Bulwark MAY refuse them. These code paths are not reachable through the default arena flow. |

- Project notes also list `togglemenu`, `menu_maps` and `menu_newmulti` as accepted.
  Bulwark MAY accept them. [SRC, low confidence]
- Note that `pause-menu` "Leave Game" sends three entries in order:
  `lobby_close`, `disconnect`, `webcore_closemenu` (section 4.5 explains why the
  title must still appear afterwards). [SRC]

### 3.4 `localsound:<sample>`

- Plays a non-spatial UI sound immediately on the local client. `<sample>` is a
  sound path **without an extension**, relative to `sound/`
  (for example `misc/menu1` resolves to `sound/misc/menu1.ogg`, which exists). The
  engine tries the usual extensions (`.wav`, `.ogg`, ...). [SRC]
- Samples used: `misc/menu1` (navigate), `misc/menu2` (confirm), `misc/menu3`
  (back or dismiss). [SRC]
- Each request starts a new channel, so rapid rollovers overlap: a new sound never
  restarts, cuts off or drops one that is still playing (FTE's `localsound`
  behaviour). [SRC: user preference, 2026-11-25; this replaces the earlier rule that
  ignored a sample already playing]
- Sounds play whether or not a map is loaded, and are unaffected by `-nosound` only
  as far as normal sound is.
- Reply: anything. Pages ignore it. `ok` suggested.

### 3.5 `clipboard_copy:<text>`

- Puts `<text>` (the remainder after the first colon, verbatim) on the OS clipboard as
  text. Replies `ok` on success and anything else on failure. [SRC]
- Used for the lobby room code, so the text is short and alphanumeric.

### 3.6 `cvar:<name>` and `get_cvar <name>`

- Returns the string value of the cvar, or `""` if it does not exist. [SRC: HUD
  `cvarNum`] The HUD parses the value with `parseFloat` and falls back to a default
  on NaN.
- The page tries `cvar:<name>` first and uses `get_cvar <name>` only when the first
  reply is `""` or null. Both MUST behave identically. [SRC]
- Names read today: `webcore_hud_drive`, `webcore_hud_drive_max`, `webcore_hud_clip`,
  `webcore_hud_ammo_max`, `webcore_hud_reload`, `webcore_hud_cooldown_0..2`.
- [PROPOSED] Limit reads to cvars whose names start with `webcore_` or `lobby_`, plus
  non-private cvars. Never return cvars flagged private or server-only, such as
  `rcon_password`.

### 3.7 `getstats`

- Reply: a JSON array of the local player's numeric client stats, indexed by
  stat number. [SRC: HUD fallback reads `stats[0]` as health and `stats[3]` as ammo,
  which matches the Quake stat numbering STAT_HEALTH=0, STAT_FRAGS=1, STAT_WEAPON=2,
  STAT_AMMO=3; `webcore-test` shows the raw string]
- Example: `[100,0,1,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]`
- [PROPOSED] The first 32 stats as integers. Floating-point stats are written as JSON
  numbers. When not connected, reply `[]`.

### 3.8 `gethud`

- Reply: a JSON object built from CSQC-published cvars (section 6.3). If HUD data
  is unavailable (no plugin support or not in game), reply `""`, and the page then
  falls back to `getstats`. [SRC: `hud/app.js` `queryHud`; notes: "CSQC publishes
  timer/health/ammo cvars; speed via predicted velocity"]
- The page checks `typeof x === 'number'` for most fields, so numbers MUST be JSON
  numbers, not strings. `timer` is a string. Missing keys mean "no change".
- Shape (keys the page consumes):

  | Key | Type | Source (suggested) | Page use |
  |---|---|---|---|
  | `health` | number | `webcore_hud_health` | HP value; a drop triggers a shake. ≤0 means "dead" art, ==1 means "low". |
  | `healthMax` | number | `webcore_hud_health_max` | HP max (min 1). |
  | `ammo` | number | `webcore_hud_ammo` | Used as ammoMax only when `ammoMax` is absent. |
  | `ammoCurrent` | number | `webcore_hud_clip` [PROPOSED] | Current clip. |
  | `ammoMax` | number | `webcore_hud_ammo_max` | Clip size. |
  | `charges` | number | `webcore_hud_charges` | Ability pips (clamped by the page). |
  | `timer` | string `"m:ss"` | `webcore_hud_timer` | Match clock. The page parses it with `split(':')`. |
  | `speed` | number | `webcore_hud_speed` (or predicted horizontal speed) | Speedometer, in Quake units per second. |
  | `scoreLeft`, `scoreRight` | number | `webcore_hud_score_left/right` | Score header. |
  | `player1`, `player2` | string | `webcore_hud_player1/2` | Names in the score header. |
  | `reload` | number | `webcore_hud_reload` | Reload seconds remaining. |
  | `exLevel` | number | `webcore_hud_ex` [PROPOSED] | EX resource count. |

  Example:
  `{"health":87,"healthMax":100,"ammo":0,"ammoCurrent":6,"ammoMax":8,"charges":2,"timer":"3:07","speed":412,"scoreLeft":2,"scoreRight":1,"player1":"Bob","player2":"Alice","reload":0,"exLevel":1}`
- Separately from `gethud`, the page also reads Drive, clip, reserve, reload and
  cooldowns through `cvar:` every frame (section 3.6).
- The HUD page also defines `window.WebCoreHud_Receive(data)`, which accepts the same
  object. The engine MAY push data this way instead of being polled. [SRC; optional]

### 3.9 `getarena` and `arena_action:<action>`

Specified in section 8.

### 3.10 `hostcache_refresh` and `gethostcache` (LAN lobby discovery)

- `hostcache_refresh`: starts a LAN discovery sweep for lobbies and game servers.
  It MUST also probe `127.0.0.1:<lobby_port>` directly so that same-machine lobbies
  appear. The reference "waits briefly" inside this call: the title page reads
  `gethostcache` **immediately** afterwards in the same JS tick. [SRC]
  [PROPOSED] Block for up to about 250 ms collecting replies, then return. Results
  persist and accumulate, so pressing Refresh again shows late responders.
- Reply to `hostcache_refresh`: ignored. `ok` suggested.
- `gethostcache`: a JSON array of discovered entries. [SRC: title pages]

  | Field | Type | Meaning |
  |---|---|---|
  | `addr` | string | Address to pass to `lobby_join:` (`ip:port`). Required; rows without it are skipped. |
  | `name` | string | Host or lobby name (`hostname`). |
  | `map` | string | Selected map (lobby) or current map (server). |
  | `players` | number | Current player count. |
  | `max` | number | Maximum players. |
  | `lobby` | boolean | `true` for a mapless lobby advertisement, `false` for an ordinary game server. |

  Example:
  `[{"addr":"192.168.1.20:27501","name":"Bob","map":"envtest","players":1,"max":16,"lobby":true}]`
- Display rule implemented by the page: if any row has `lobby: true`, rows with
  `lobby: false` are hidden. With no rows, the page shows
  "No LAN lobbies found — try Address join (127.0.0.1:27501)". [SRC]
- Bulwark SHOULD list only lobbies in phase WAITING (joins are rejected otherwise;
  section 9.6). [PROPOSED]

### 3.11 `lobby_join:<target>`

- `<target>` is either an address (`host`, `host:port`, `[v6]:port`) or an online room
  code (for example `ABCD12`). The title page strips whitespace and sends the code
  through this same request. [SRC]
- Behaviour: validate the target. If it is acceptable, queue `lobby_join <target>`
  (section 9.3) and reply `ok`. Otherwise reply something else and do nothing. An
  empty target MUST be rejected. [SRC]
- [PROPOSED] Validation: 1–64 characters from `[A-Za-z0-9._:\-\[\]]`.
- After `ok`, the page immediately queues `cbuf:menu_webcore_lobby`. The lobby page
  must therefore render a JOINING state until the session is established or fails
  (section 9.5). [SRC]
- On refusal (`!== "ok"`) the page stays on the join dialog. [SRC]

### 3.12 `getlobby`, `getlobbychat`, `lobby_action:<action>`

Specified in sections 9.8–9.10.

## 4. The fullscreen menu surface

### 4.1 Model

- There is at most one menu surface at a time. It is a single view that fills the
  whole framebuffer: a stretch blit to `(0,0)–(width,height)`, **not** letterboxed.
  Its pixel size equals the framebuffer size and it is resized whenever the window or
  video mode changes. [SRC]
- Pages letterbox their own content. Title v2 uses a 1024×768 reference, lobby 960×720,
  ql-menu 640×480 and pause 1280×720.
- It composites with premultiplied alpha over the 3D scene or HUD, so the world shows
  through transparent pixels (in game and on the title backdrop). When nothing is
  rendered behind it, the backdrop is black. [SRC + INF]
- While the menu surface is open:
  - It receives keyboard, character and mouse input, except the console toggle and
    the console itself (section 4.6). Gameplay input bindings MUST NOT fire.
  - The mouse is an absolute pointer: unlocked and not used for mouselook. Positions
    are delivered in view pixels. Wheel events are delivered. [SRC]
  - The OS or engine cursor SHOULD be shown, because title, lobby and pause draw no
    cursor of their own. [PROPOSED] Hide it for the arena page. `ql-menu` draws its
    own pointer (`assets/3_cursor3.png`) and sets `cursor: none`; honouring the
    page's CSS cursor achieves the same.
  - JS timers and `requestAnimationFrame` keep running every engine frame.
- The menu MUST NOT be treated as a cinematic: Esc never "skips" it, and it never ends
  on its own. [SRC]
- Opening the surface while it is already open with the **same URL** MUST NOT reload
  the page. Repeated selector requests from the server happen (section 8.4).
  Opening a different URL replaces the document in the same surface. [PROPOSED]

### 4.2 Commands

| Command | Behaviour |
|---|---|
| `menu_webcore` | No argument: open the default page for the current context (in game: pause page; otherwise: title page). [INF] |
| `menu_webcore <url>` | Open an explicit `fte://data/web/...` URL. Example: `menu_webcore fte://data/web/ql-menu/index.html`. [SRC] |
| `menu_webcore title` | Open the title page (`webcore_menu_url`). [INF] |
| `menu_webcore lobby` | Open `fte://data/web/lobby-menu/index.html`. [SRC] |
| `menu_webcore pause` | Open the pause page. [PROPOSED alias] |
| `menu_webcore arena <selector>` | `<selector>` is `team`, `class` or `hero`. In game only: open `fte://data/web/ql-menu/index.html?mode=arena` as an in-game, transparent, closable surface. Do not start title music and do not load the backdrop map. The engine remembers `<selector>`. If the page cannot be opened (renderer unavailable or failed to create), the engine MUST queue `<fallback> vgui`, where `<fallback>` is `showTeamSelectionMenu` for `team`, `showClassSelectionMenu` for `class` and `showHeroSelectionMenu` for `hero`. The trailing `vgui` argument makes CSQC show its native VGUI selector instead of retrying WebCore. [SRC] |
| `menu_webcore_title` | Same as `menu_webcore title`. [SRC] |
| `menu_webcore_lobby` | Same as `menu_webcore lobby`. [SRC] |
| `menu_lobby_create` | Run `lobby_create_lan`, then open the lobby page. Legacy: it must **not** set `sv_background` or load a map. [SRC] |
| `webcore_closemenu` | Close the menu surface if open; otherwise do nothing. In game, input returns to gameplay at once and the mouse is re-captured. If disconnected, the title-pending rule (section 4.4) reopens the title on a later frame. MUST be idempotent. [SRC] |
| `togglemenu` (bound to Esc) | See section 4.3. |
| `webcore <url>` | Optional debug: open a windowed view. Not required. [SRC: optional] |

`<selector>` MAY be appended to the arena URL as `&selector=<selector>`. The current
page ignores it. [PROPOSED]

### 4.3 Escape key and `togglemenu`

| Situation | Esc does |
|---|---|
| Console visible (in game) | Standard console behaviour (close the console). |
| Disconnected, console fully down, no menu | Close the console and show the title (`menu_webcore title`). This is required: "Escape ... return from the disconnected console to the title menu". [SRC] |
| Menu open, title or lobby context (not in game) | Deliver Esc to the page **only**. The engine MUST NOT close the menu. Title pages use Esc to close their Join/Create dialog; the lobby page uses it to open or close its Leave dialog. On the bare title, Esc does nothing. [SRC] |
| Menu open, in game (pause, arena, custom URL) | Deliver the Esc `keydown` to the page, then close the menu (same as `webcore_closemenu`). Pause and ql-menu also request a close themselves; the close is idempotent. ql-menu's non-arena warmup view deliberately does not handle Esc and relies on the engine to close. [SRC + INF] |
| No menu, in game | `togglemenu` opens the pause page. [SRC] |

`togglemenu` typed or bound: in game it opens the pause page if the menu is closed and
closes it if open. When not in game it opens the title if nothing is shown, and does
nothing if a title or lobby page is already up. [INF]

### 4.4 When the menu opens automatically

- **Startup:** when the client is idle (not connecting, signing on or loading, and no
  server being started from the command line such as `+map`), the title page opens
  once. Auto-title MUST be suppressed while a server is running or loading, while the
  client is signing on, or while a connection is in progress. Otherwise title
  startup would load the backdrop map over the requested world and switch rules. [SRC]
- **Disconnect:** after the client becomes disconnected, the engine sets
  *title pending*. On a **later frame**, once the command buffer has drained, if no
  menu surface is open and the client is still idle, it opens the title page (and
  loads the backdrop, section 4.7). Deferring to a later frame is what makes pause
  "Leave Game" (`lobby_close; disconnect; webcore_closemenu`) end on the title rather
  than on nothing. [SRC behaviour; ordering mechanism INF]
- **Lobby terminal states:** if a lobby session closes or fails while the lobby page
  is shown (host left, timeout, rejected join), the engine shows the title page.
  [SRC: "terminal failures return to title"] When the session reaches INGAME and the
  client becomes active in the game, the menu surface closes and gameplay input
  resumes. [INF]
- **Feature switch:** cvar `webcore_menu_enabled` (default `1`). When `0`, every
  menu push and the automatic title/backdrop startup are suppressed. It exists for
  test and launcher use. [SRC]
- In-game pause and arena pushes are always allowed while in game, regardless of the
  title guards. [SRC]

### 4.5 Pages per context

| Context | Page | Cvar |
|---|---|---|
| Title | `webcore_menu_url`. Engine default `fte://data/web/title-menu/index.html`; autoexec sets `fte://data/web/title-menu-v2/index.html`. | `webcore_menu_url` [SRC] |
| Lobby | `fte://data/web/lobby-menu/index.html` | (none) [SRC] |
| Pause (in game) | `fte://data/web/pause-menu/index.html` | [PROPOSED cvar `webcore_pause_url`] [INF: plans name the pause page; arena plan requires "opening pause after closing arena still opens the normal pause page"] |
| Arena selector | `fte://data/web/ql-menu/index.html?mode=arena` | (none) [SRC] |

### 4.6 Console

- The console toggle key (`` ` `` / `~` → `toggleconsole`) MUST be intercepted
  **before** the page sees it, and MUST work while any menu surface is open. [SRC]
- While the console is down it draws over the menu and receives all keys. When it
  closes, input returns to the menu surface.
- Key events with Ctrl, Alt or Meta that a page does not use reach the page but MUST
  NOT block engine shortcuts. The lobby page explicitly lets those through. [SRC]

### 4.7 Title backdrop map: `webcore_menumap`

- Cvar `webcore_menumap`, archived, default `background02`. Empty means no backdrop.
  [SRC]
- When the title page is pushed and the client is idle (section 4.4 guards), the engine
  starts the named map locally as a backdrop:
  - A local, single-player style server with serverinfo key `background` = `1`. CSQC
    reads `serverkeyfloat("background") == 1` and then draws no HUD and never pauses.
    [SRC: `entry.qc`]
  - The title page renders over the live 3D scene (pages are transparent).
- The backdrop is not a game. Esc, auto-title and music treat it as "disconnected".
  Lobby discovery MUST NOT advertise it. [INF]
  [PROPOSED] Do not open it to the network at all.
- The arena and pause routes MUST NOT load the backdrop. [SRC]
- The backdrop's `set g_gametype singleplayer`/`coop 1` would otherwise carry into the
  next `map` typed at the console. The game hands them back: Nuclide's server resets
  `g_gametype ""` and `coop 0` when it consumes `sv_background`. [SRC: `entry.qc`]
- The arena route closes on a level change (`cl.spawncount` changes, or the client
  leaves the game); an arena mode on the new level opens it again. Left open, it sat
  over the next mode and took its input. [SRC]
- The backdrop runs `set g_gametype singleplayer`, `set coop 1`, `set deathmatch 0`,
  `set sv_background 1`, then `map <webcore_menumap>`. It leaves the player-slot count
  alone (Nuclide's own `map_background` also sets `sv_playerslots 2`): two slots would
  carry over to a game typed at the console after the title, leaving room for one bot.
  [SRC]
- The backdrop is queued behind whatever is in the command buffer and checked again
  when it runs: if a server is active or a connection is under way by then, or the
  title page closed, it is skipped. A start-up cfg still in a `wait` before its own
  `map` would otherwise have its game replaced by the backdrop once the cfg ran out.
  [SRC]
- The test harness disables it with `set webcore_menumap ""`. [SRC]
- [UNCERTAIN] Whether the backdrop keeps running while the lobby page is shown.
  The lobby is "mapless" meaning no gameplay server or listen server. A purely local
  visual backdrop is compatible with that, provided it never answers lobby or game
  traffic. At game start the host's `map <selected>` replaces it, and a joiner's
  `connect` drops it.

### 4.8 Menu music

- Engine-side, not played by the page. [SRC]
- Playlist: `music/menu_boot`, then `music/menu1`, `music/menu2`, `music/menu3`, …,
  then back to `music/menu_boot`, looping. Each track plays once; on track end the
  engine advances. [SRC]
- Shipped files: `base/music/menu_boot.ogg`, `menu1.ogg` … `menu5.ogg`, `menu11.ogg`.
  [UNCERTAIN] Whether the sequence stops at the first missing index (after `menu5`)
  or skips gaps. [PROPOSED] Stop at the first missing `menuN`, then wrap to
  `menu_boot`.
- Music plays while the client is in the title or lobby context. It MUST keep
  advancing even when a classic engine menu (for example `menu_options`) is on top of,
  or replaces, the WebCore title. [SRC]
- It stops when a real game starts. The arena and pause routes MUST NOT start it.
  [SRC]
- [PROPOSED] Respect a cvar `webcore_menu_music` (default `1`). The name appears in
  project notes; its semantics were not confirmed.

### 4.9 Engine and game sub-menus requested from pages

- `menu_options`: open the engine's settings UI on top of the menu surface. The
  surface stays loaded underneath. When settings close, the WebCore page is visible
  and interactive again. Title v2 hides its chrome for 180 ms and then restores it
  itself. [SRC]
- `menu_callvote`, `menu_hostmenu`, `menu_changeteam`: in game. Open the corresponding
  engine or game UI if Bulwark has one; otherwise do nothing.
  [PROPOSED] `menu_changeteam` → `showTeamSelectionMenu`.
- `quit`, `disconnect`, `map_restart`: standard.
- Unknown console commands queued from the menu while connected are forwarded to the
  server as client commands. This is standard Quake behaviour and how ql-menu's
  `ready` reaches the server. [INF]

## 5. Input-focus rules summary

| Surface | Keyboard | Mouse | Draws over |
|---|---|---|---|
| Menu surface (any page) | Yes, except the console key | Yes, absolute | Everything except the console |
| HUD view | Never | Never | World, under the menu surface |
| Console | When down | n/a | Everything |

## 6. The HTML HUD and the `gecko_*` builtins

### 6.1 Builtins Bulwark must provide (CSQC; declared in `fteextensions.qc`, DP_GECKO_SUPPORT)

| Builtin | Required behaviour |
|---|---|
| `float gecko_create(string name, optional string initialURI)` | Create a "browser tab" image resource named `name` that `drawpic(..., name, ...)` can draw. `initialURI` `webcore:<url>` means an HTML view of `<url>` (`fte://...`). Returns non-zero on success and 0 on failure (renderer unavailable, bad URI). Per the declaration, the name should not already exist. [PROPOSED] If a view with that name already exists, re-navigate it and return success; this happens after a CSQC reload, see 6.4. |
| `void gecko_navigate(string name, string uri)` | Either navigate to `uri`, or, if `uri` begins with `cmd:`, send a decoder command: `cmd:transparent` makes the view clear to fully transparent (alpha 0) before each paint, producing premultiplied pixels; `cmd:opaque` reverts that; `cmd:focus`/`cmd:unfocus` set logical focus. [SRC] |
| `void gecko_resize(string name, float w, float h)` | Resize the view to `w`×`h` pixels (rounded). A no-op if unchanged. The page receives `resize`. [SRC] Called every frame by the HUD. |
| `void gecko_destroy(string name)` | Destroy the view and free the image. [SRC] |
| `float gecko_keyevent(string name, float key, float eventtype, optional float charcode)` | Send a key event to the view. Not used by Stiletto. |
| `void gecko_mousemove(string name, float x, float y)` | Mouse position in 0..1. Not used by Stiletto. |
| `vector gecko_get_texture_extent(string name)` | Current pixel size. Not used. |
| `string gecko_getproperty(string name, string prop)` | `url`, `title`, `status`. Not used. |

`drawpic(pos, name, size, rgb, alpha, flags)` with a gecko name draws the view's
latest frame stretched into `pos`/`size`, using premultiplied alpha blending
(source × 1 + destination × (1 − source alpha)), and tinted by `rgb × alpha`.
The first frames, before any paint, draw nothing (fully transparent). [SRC]

Views created this way get `window.fte_query` (section 3), run timers and
`requestAnimationFrame` every engine frame, and receive no input unless
`gecko_keyevent`/`gecko_mousemove` are used. [SRC: HUD must not steal input]

### 6.2 HUD lifecycle (CSQC, `base/src/hud/hud.qc`) [SRC]

1. Every in-game HUD frame, if `webcore_hud > 0`:
   - If the HUD view does not exist yet, call
     `gecko_create("webcore_hud", "webcore:" + webcore_hud_url)`. On success,
     immediately call `gecko_navigate("webcore_hud", "cmd:transparent")` and mark it
     alive. On failure, retry next frame.
   - Publish the HUD cvars (6.3).
   - `gecko_resize("webcore_hud", HUDSize.x, HUDSize.y)`.
   - `drawpic(HUDMins, "webcore_hud", HUDSize, [1,1,1], 1, 0)`.
   - Skip the classic sprite HUD.
2. If `webcore_hud <= 0`, `gecko_destroy("webcore_hud")` (once) and draw the classic
   HUD.
3. Nothing is drawn when serverinfo `background` = `1` (title backdrop).

`HUDMins`/`HUDSize` is the letterboxed HUD-safe rectangle (Nuclide's 4:3 HUD aspect).
The HUD page is authored for a 1440×1080 (4:3) canvas and scales to the view.

### 6.3 Cvars CSQC publishes for the HUD [SRC]

From `hud.qc`, every HUD frame:

| Cvar | Value |
|---|---|
| `webcore_hud_timer` | `"m:ss"` from CSQC `time` |
| `webcore_hud_health` | player health |
| `webcore_hud_health_max` | `getstatf(STAT_USER+3)` |
| `webcore_hud_ammo` | active weapon ammo1 |
| `webcore_hud_reload` | `getstatf(STAT_USER+1)` |
| `webcore_hud_score_left`, `webcore_hud_score_right` | duel: serverinfo `arena_score1/2`; otherwise the top two frag counts |
| `webcore_hud_player1`, `webcore_hud_player2` | duel: serverinfo `arena_name1/2`; otherwise the top two names |

From `HeroActions_PublishHud` (CSQC predicted player, per frame):
`webcore_hud_speed` (integer horizontal speed), `webcore_hud_charges`, `webcore_hud_clip`,
`webcore_hud_ammo_max` (both 0 when there is no weapon), `webcore_hud_health`,
`webcore_hud_health_max`, `webcore_hud_drive`, `webcore_hud_drive_max`, `webcore_hud_ex`,
`webcore_hud_ex_max`, `webcore_hud_guard`, `webcore_hud_guard_hp`,
`webcore_hud_guard_hp_max`, `webcore_hud_groove`, `webcore_hud_mode`.

These are high-frequency writes. `cvar_set` from CSQC MUST be cheap and MUST NOT
trigger archive writes. Do not mark these cvars archived.

### 6.4 Shutdown

CSQC's `g_webcoreHudAlive` is reset when CSQC reloads (map change or reconnect), so
CSQC will call `gecko_create("webcore_hud", ...)` again with the same name. Bulwark
MUST either destroy gecko views owned by a CSQC instance when that instance shuts
down, or accept a re-create of an existing name (6.1). [PROPOSED: do both]

## 7. Pause page (in game)

`fte://data/web/pause-menu/index.html` [SRC]

- Items (top to bottom), activated by click, Enter or Space:
  1. Return to Game → `cbuf:webcore_closemenu`
  2. Host Menu → `cbuf:menu_hostmenu`
  3. Change Team → `cbuf:menu_changeteam`
  4. Call a Vote → `cbuf:menu_callvote`
  5. Settings → `cbuf:menu_options`
  6. Leave Game → `cbuf:lobby_close`, `cbuf:disconnect`, `cbuf:webcore_closemenu`
     (three calls, in this order)
- Keys: ArrowUp/W and ArrowDown/S move; Enter/Space activate; Escape →
  `cbuf:webcore_closemenu`. Mouse hover selects. The wheel moves the selection.
- Required outcome: Return closes the pause UI and resumes play. Leave ends on the
  title page (section 4.4). [SRC: user preference]

## 8. Arena / duel selector flow

### 8.1 CSQC adapter (`src/client/webcore_arena.qc`, `entry.qc`, `cmd.qc`) [SRC]

- Enabled when the CSQC autocvar `cl_webcore_arena_ui` is true (QC default false;
  autoexec sets 1), there is exactly one local seat, and serverinfo `arena_state` is
  non-empty.
- Every CSQC frame with one seat:
  - If `arena_state` is empty and the selector is not open, or there is more than one
    seat: *Clear*, which means close the selector if CSQC opened it (by queuing
    `webcore_closemenu`), then set `webcore_arena_snapshot` to `{"active":false}` and
    `webcore_arena_hero_ids` to `""`.
  - Otherwise *Publish*:
    - If `arena_state` ≠ `WARMUP`, close the selector (queue `webcore_closemenu` if
      CSQC believes it is open).
    - If the selector is open and the local `*arena_state` has just transitioned to `2`
      (spectator), close it.
    - Build the snapshot JSON (8.2). If it changed, set `webcore_arena_snapshot` to
      it and `webcore_arena_hero_ids` to the space-separated hero ID list.
- On CSQC shutdown or disconnect: *Clear*.
- Commands `showTeamSelectionMenu`, `showClassSelectionMenu` and
  `showHeroSelectionMenu`. These are CSQC console commands; the server stuffs them,
  for example in duel on spawn or join without a hero, after a hero pick while joining,
  and on `play`/`choosehero`:
  - If the first argument is not `vgui` and the adapter is available: mark the selector
    open, publish, and queue `menu_webcore arena team`, `menu_webcore arena class` or
    `menu_webcore arena hero` respectively.
  - Otherwise show the VGUI selector.

### 8.2 `webcore_arena_snapshot` JSON (exact shape produced by CSQC) [SRC]

Inactive: `{"active":false}`

Active:

```
{
  "active": true,
  "phase":  string,   // serverinfo arena_state: "WARMUP" | "COUNTDOWN" | "LIVE" |
                      //   "SUDDEN DEATH" | "ROUND END" | "MATCH END" | "?"
  "round":  string,   // serverinfo arena_round, e.g. "Round 1 / 3"
  "map":    string,   // serverinfo mapname
  "readyCount": number, // serverinfo arena_ready (queue length)
  "players": [string, string], // serverinfo arena_p1, arena_p2: "Name (points)" or ""
  "local": {
    "state": number,      // local userinfo *arena_state: 0 NOTREADY, 1 READY (queued),
                          //   2 SPECTATOR (opted out), 3 COMPETITOR
    "hero":  string,      // local userinfo *hero ("" = none)
    "needsHero": boolean, // joining && hero == ""
    "joining":   boolean  // local userinfo *duel_joining non-empty
  },
  "heroes": [ {"id": string, "name": string}, ... ] // from serverinfo arena_heroes
                          // (space-separated IDs); name == id
}
```

The QC JSON-escapes strings by escaping `\` and `"` and stripping CR/LF. Example:

```
{"active":true,"phase":"WARMUP","round":"Round 0 / 3","map":"duel_alley","readyCount":1,
 "players":["Bob (0)",""],"local":{"state":0,"hero":"","needsHero":true,"joining":true},
 "heroes":[{"id":"hero_collier","name":"hero_collier"},{"id":"hero_juliet","name":"hero_juliet"}]}
```

`webcore_arena_hero_ids` holds the same IDs as a space-separated list,
for example `hero_collier hero_beaumont hero_juliet`.

### 8.3 `getarena`

- Reply: the current value of `webcore_arena_snapshot` verbatim, if it is a JSON
  object. If the cvar is empty or not an object, reply `{"active":false}`. [SRC]
- The page polls this every 120 ms. With `active:false` it shows
  "Waiting for arena state…". [SRC]
- The snapshot may name the arena mode: `mode` (`duel`, `freezetag`,
  `archstiletto`, ...) and `modeName`, its display name ("FREEZE TAG").
  The panel's title is `modeName` (else `mode`, else "DUEL") in capitals
  followed by " ARENA" ("FREEZE TAG ARENA"), as the game's own page draws
  it. The line over the competitors reads "CURRENT DUEL" when `mode` is
  `duel` or absent, else "CURRENT MATCH".

### 8.4 `arena_action:<action>`

Reply: JSON `{"ok":true}` on acceptance, or `{"ok":false,"error":"<reason>"}`. The
page parses JSON and checks `ok === true`. The `error` field is
[PROPOSED]. [SRC]

Actions and the console command each produces. These are game-server client commands
handled by Nuclide RuleC (`duel.qc` / `arena.qc`); the server stays authoritative:

| Action | Precondition (bridge) | Effect |
|---|---|---|
| `join` | in game, snapshot active | Queue `cmd play`. [PROPOSED] Duel's `play` sets `*duel_joining`, clears the hero, sets not-ready, gives a waiting view and stuffs `showClassSelectionMenu`. The fixture treats `join` and `play` identically. [UNCERTAIN] The reference may instead have sent auto-assign `cmd joinTeam <best team>`, but the observable result in duel is the same. |
| `play` | same | Queue `cmd play` (JOIN QUEUE from spectator). [INF] |
| `choosehero` | same | Queue `cmd choosehero`. [INF] |
| `hero:<id>` | `<id>` matches `[A-Za-z0-9_]+` **and** is one of the words in `webcore_arena_hero_ids` | Queue `cmd selecthero <id>`. Otherwise `{"ok":false}` and nothing is executed. [SRC: validation rule; INF: command] |
| `ready` | same | Queue `cmd ready`. [INF] The menu stays open and shows the queued state. |
| `unready` | same | Queue `cmd notready`. [INF] |
| `spectate` | same | Queue `cmd spectate`, then close the menu surface. [SRC: "plugin closes on spectate"] The page also sends `arena_action:close` afterwards; this must be harmless. |
| `close` | always | Close the menu surface (same as `webcore_closemenu`). It MUST have no game side effect, in particular no `cmd arena_enter`. [SRC] |
| anything else | n/a | `{"ok":false}`, nothing executed. [SRC] |

Server-side meaning, for reference (game code, not engine):

- `selecthero` records the pick while the player is joining. On success it clears
  `*duel_joining` and re-stuffs `showClassSelectionMenu`, so the open selector must
  **not** reload (4.1).
- `spectate` sets the pure-spectator state, `ready`/`notready` toggle the queue, and
  `choosehero` re-stuffs the selector.

### 8.5 How the selector opens and closes (end to end)

1. The duel server stuffs `showClassSelectionMenu` (for example on spawn while joining
   without a hero).
2. CSQC publishes the snapshot and queues `menu_webcore arena class`.
3. The engine opens `ql-menu/index.html?mode=arena` as an in-game transparent surface:
   no music, no backdrop map. If that fails, it queues `showClassSelectionMenu vgui`.
4. The page polls `getarena` every 120 ms and renders by `local`:
   - `needsHero`: hero grid (up to 8) + SPECTATE.
   - `state == 2`: JOIN QUEUE.
   - `state ∈ {1,3}`: CHOOSE HERO + LEAVE QUEUE (= `spectate`).
   - Hero chosen and `state == 0`: READY UP + CHOOSE HERO + LEAVE QUEUE.
   - Otherwise: JOIN MATCH + SPECTATE.
5. Close paths:
   - Esc on the page → `arena_action:close`.
   - SPECTATE or LEAVE QUEUE → `spectate`, which closes.
   - CSQC queues `webcore_closemenu` when `arena_state` leaves WARMUP or the local
     state enters spectator.
   - Disconnect or CSQC shutdown → CSQC Clear.
   - A later stuffed selector command reopens the page.
6. The page shows `phase` and `round` text. It never runs its own countdown in engine
   mode. [SRC]

## 9. Lobby system (engine pre-game session)

### 9.1 Product contract [SRC]

- Host-authoritative, **mapless** pre-game party. No game server and no listen server
  exist until the start commits. A local title backdrop, if kept, is not a server for
  this purpose (4.7).
- Three topologies share one session model: two processes on one PC (localhost), LAN
  (discovery or direct address), and online (room code via broker and ICE).
- Phases: `IDLE → CREATING | JOINING → WAITING → COUNTDOWN → STARTING → INGAME`, with
  terminal failures passing through `CLOSING` (long enough to publish a reason), then
  `IDLE` and the title page.
- Leave returns to the title. Host leave closes the lobby for everyone.

### 9.2 Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `lobby_port` | `27501` | UDP port the host's lobby listens on. Also the default port for `lobby_join <host>` without a port. [SRC] |
| `lobby_readytime` | [UNCERTAIN; PROPOSED `5`] | All-ready countdown length in seconds. Configs use 3 and 10. [SRC] |
| `lobby_maxplayers` | `16` | Seats. Snapshot `max`. [SRC] |
| `lobby_defaultmap` | `envtest` | Map selected at create (`base/maps/envtest.bsp`). [SRC] Bulwark leaves it empty and picks the ruleset's own map from `scripts/lobby.txt` ([lobby.md](lobby.md)). |
| `lobby_defaultruleset` | `DUEL` | Ruleset at create, and the fallback mode ID when no MenuQC registered modes. [SRC] |
| `lobby_snapshot` | `""` | The current atomic snapshot JSON (9.8), refreshed whenever the revision or local connection state changes. Read-only for users. [SRC] |
| `hostname` / `name` | standard | `name` is the local player name in the roster. [SRC] |
| `ruleset_allow_in` | FTE standard | Must be 1 to allow the delayed `in <secs> <cmd>` command used by the test configs. [SRC] |
| `net_ice_broker`, `net_ice_debug`, `net_ice_allowmdns`, `net_ice_exchangeprivateips` | FTE standard | Online carrier configuration (9.13). [SRC] |
| Compatibility, optional: `lobby_room_code`, `lobby_host_name`, `lobby_state_str`, `lobby_player_count`, `lobby_max_players`, `lobby_player1_name` … `lobby_player4_name` | | Legacy mirror for the VGUI/Slint lobby. The roster names carried a `[READY]` prefix when ready. `lobby_player_count` is 0 when no session exists. [SRC] |

### 9.3 Console commands

| Command | Behaviour |
|---|---|
| `lobby_create <lan\|online\|offline>` | Create a session as host (seat 0, not ready, phase WAITING, map `lobby_defaultmap`, ruleset `lobby_defaultruleset`, limits empty or 0). `lan`: bind `lobby_port`, answer discovery. `online`: register a room code with the broker; the phase is CREATING until registered, then WAITING with `roomCode` set. `offline`: a single-process session for testing (no network). Fails if a session already exists ([PROPOSED]: close the old one first). No map is loaded. [SRC] |
| `lobby_create_lan`, `lobby_create_online`, `lobby_create_offline` | Aliases of the above. [SRC] |
| `lobby_join <target>` | Join as guest. A target that looks like an address (contains `.` or `:`, or is `localhost`) is treated as an address, with the port defaulting to `lobby_port`. Otherwise it is a room code resolved through the broker. [PROPOSED rule] The phase is JOINING (status text) until JOIN_ACCEPT, then WAITING with `localSeat` set. Retries with bounded attempts; on failure the phase goes to CLOSING with an error, then IDLE, and the menu returns to the title. [SRC] |
| `lobby_ready [1\|0]` | Set the local seat's ready state; `1` if omitted (set, not toggle). [SRC] |
| `lobby_cancel` | Host: cancel the countdown (9.7). [SRC] |
| `lobby_start` | Host: commit the start (9.7). [SRC] |
| `lobby_close` | Leave or close the session. A host broadcasts CLOSE; a guest sends LEAVE. Afterwards the phase is IDLE. Harmless with no session (pause "Leave Game" always sends it). [SRC] |
| `lobby_set ruleset <ID>` | Host: set the ruleset (9.10). [SRC] [PROPOSED] Also `lobby_set map <id>`. |
| `lobby_limits <minutes> <frags>` | Host: set the time and frag limits. [SRC] |
| `lobby_chat_hex <hex>` | Send a chat message (UTF-8 bytes, hex). [SRC] |
| `lobby_snapshot` | Debug: print the snapshot JSON and write it to `lobby_snapshot.json`. [SRC; output form INF] |
| `lobby_chat_snapshot` | Debug: print the chat JSON (and [PROPOSED] write `lobby_chat.json`). [SRC; output form INF] |

Debug artifact: the reference engine writes `lobby_snapshot.json` (the snapshot JSON)
into the **process working directory** whenever the snapshot changes. The test
harnesses poll this file, and the online test never issues `lobby_snapshot`.
[SRC] [UNCERTAIN] Whether this happened only with `developer 1`; both tests set it.
Bulwark SHOULD write it at least when `developer` ≠ 0. Readers tolerate partial files.

Command-line use: `+lobby_create online`, `+lobby_join <code>`, `+lobby_create_lan`,
`+lobby_join 127.0.0.1`. [SRC]

### 9.4 Frame pump

The session advances once per client frame from the main client loop, independent
of which UI is visible or whether a menu is open: carrier polling, retransmits,
heartbeats and timeouts, countdown, snapshot publishing, and start/listen polling.
UI drawing never advances it. The countdown must complete with the console open. [SRC]

### 9.5 Authority, seats and roster

- The host owns seat assignment, roster, map, ruleset, limits, phase, countdown
  deadline, ready clearing, close reason and the start commit. [SRC]
- Guests send intents only: join, ready/unready, leave, chat, and acknowledgements.
  A peer may mutate only its own seat. Its source endpoint is bound to its seat after
  acceptance; anything from an unbound endpoint is ignored. [SRC]
- Every authoritative change increments `revision`. A full snapshot is sent on
  accept and periodically as repair (it includes countdown state). [SRC]
- Re-sent join requests from the same peer address reuse its seat, so no ghost seats
  are created. [SRC]
- Joins are rejected (with an explicit reason) when the lobby is full, during
  COUNTDOWN/STARTING/INGAME, or on a protocol version mismatch. [SRC]
- The joiner's display name is captured before session init; names must survive
  synchronisation unchanged. [SRC]
- Teams: the WebCore UI shows a flat roster (no team columns).
  `lobby_registermode`/`lobby_addteam` define teams for the Slint UI only (9.11).

### 9.6 Phases and capabilities

| Phase | Joins | Host may | Anyone may |
|---|---|---|---|
| CREATING / JOINING | n/a | n/a | leave |
| WAITING | allowed | set map, ruleset, limits; start; ready | ready/unready, chat, leave |
| COUNTDOWN | rejected | cancel; (map/ruleset change cancels) | unready (cancels), chat, leave |
| STARTING | rejected | (nothing mutable) | chat, leave |
| INGAME | rejected | n/a | n/a |
| CLOSING | n/a | n/a | n/a (UI shows `status` / `error`) |

Snapshot capability flags (the UI enables controls only from these) [SRC: names;
PROPOSED: exact formulas]:

- `canReady` = active && localSeat ≥ 0 && phase ∈ {WAITING, COUNTDOWN}
- `canStart` = isHost && phase == WAITING
- `canCancel` = isHost && phase == COUNTDOWN
- `canChangeMap` = isHost && phase ∈ {WAITING, COUNTDOWN}. Also gates ruleset and
  limits in the UI.
- `canLeave` = true while active ("Leave always enabled")
- `canChat` = active && localSeat ≥ 0 && phase ∈ {WAITING, COUNTDOWN, STARTING}

### 9.7 Ready, countdown, cancel, start

- The host is a normal player and must ready too; there is no implicit host-ready.
  [SRC]
- When **all occupied seats are ready** (including a solo host), the host arms
  COUNTDOWN with `countdownEnd = now + lobby_readytime`, after a short settle period
  following any join or leave. [SRC] [PROPOSED] Settle 0.5 s.
- During COUNTDOWN, any of the following returns the phase to WAITING and clears ready
  for **every** seat: host cancel (`lobby_cancel` / action `cancel`), any unready, a
  join or leave, or a map/ruleset/limit change. [SRC]
- Countdown reaching zero commits the start: STARTING (9.12).
- Explicit host Start (`lobby_start` / action `start`):
  [UNCERTAIN] The plans say Start "may force the normal countdown only when all
  occupied seats are ready; it never bypasses readiness". However, the reference
  three-client test readies everyone, cancels (which clears all ready), then issues
  `lobby_start` 2 s later, and requires every client to reach INGAME. The project log
  calls this "a subsequent successful explicit Start". **Observed behaviour therefore
  wins:** host Start MUST succeed in WAITING even when not everyone is ready.
  [PROPOSED] It goes straight to STARTING (or runs a `lobby_readytime` countdown
  first; both pass the test). It replies `{"ok":false}` with status
  "Not all players are ready." only if Bulwark chooses the stricter rule. Do not
  choose it, or the reference test fails.
- Guests display the countdown from `countdownEnd` and `serverTime` in **their own
  local clock**. The host sends the remaining duration, not an absolute time, and the
  guest converts it to its own clock. Clocks of different processes are never
  compared. [SRC]

### 9.8 `getlobby` and the snapshot JSON

`getlobby` returns `lobby_snapshot` verbatim, after checking that it is a JSON
object. With no session it MUST still return a valid inactive object. The page
treats `""` as "waiting for connection" and shows an error after 2 s. [SRC]

Shape [SRC: plan field list, UI consumers and the native test fixture]:

```
{
  "active":      boolean,
  "sessionId":   string,   // random per session
  "revision":    number,   // increments on every authoritative change
  "uiVersion":   number,   // 1; non-zero enables chat/ruleset/limits UI and getlobbychat
  "network":     string,   // "lan" | "online" | "offline"   [PROPOSED values]
  "isHost":      boolean,
  "localSeat":   number,   // -1 when not seated
  "phase":       string,   // "IDLE"|"CREATING"|"JOINING"|"WAITING"|"COUNTDOWN"|
                           //  "STARTING"|"INGAME"|"CLOSING"
  "status":      string,   // human text, e.g. "Waiting for players", "Joining 127.0.0.1…"
  "error":       string,   // "" or terminal/rejection reason
  "roomCode":    string,   // online room code, "" for LAN
  "map":         string,   // selected map id, e.g. "envtest"
  "max":         number,   // seat count (lobby_maxplayers)
  "countdownEnd":number,   // local-clock seconds; 0 when not counting
  "serverTime":  number,   // local-clock seconds at snapshot time
  "canReady": boolean, "canStart": boolean, "canCancel": boolean,
  "canChangeMap": boolean, "canLeave": boolean, "canChat": boolean,
  "settings": {
    "ruleset":    string,  // e.g. "DUEL" | "DEATHMATCH" | "TEAMDM" | "DOMINATION"
    "servertype": string,  // "LISTEN SERVER" (only option)
    "timelimit":  string,  // decimal minutes as STRING, e.g. "12" ("0" = none)
    "fraglimit":  string   // decimal frags as STRING, e.g. "25"
  },
  "players": [
    { "seat": number, "name": string, "host": boolean,
      "ready": boolean, "connected": boolean }
  ],
  // Bulwark additions (lobby.md, "Maps and rulesets"); pages fall back to
  // their own lists when these are missing:
  "maps":     [ { "id": string, "name": string } ],
  "rulesets": [ { "id": string, "name": string, "map": string, "available": boolean } ]
}
```

Notes:

- The reference test compares `settings.timelimit` with `'12'` and `fraglimit` with
  `'25'` **as strings**. Keep them strings. [SRC]
- `players` is ordered by seat. No 4-seat cap; 1–16 players must serialise. [SRC]
- The page's refresh key ignores `serverTime` changes outside COUNTDOWN, so
  `serverTime` may change on every call. [SRC]
- Inactive example:
  `{"active":false,"sessionId":"","revision":0,"uiVersion":1,"isHost":false,"localSeat":-1,"phase":"IDLE","status":"","error":"","roomCode":"","map":"","max":16,"countdownEnd":0,"serverTime":123.4,"canReady":false,"canStart":false,"canCancel":false,"canChangeMap":false,"canLeave":false,"canChat":false,"settings":{},"players":[]}`
- Active example (from the native test fixture):
  `{"active":true,"sessionId":"ui-test","revision":1,"uiVersion":1,"isHost":true,"localSeat":0,"phase":"WAITING","status":"Waiting for players","map":"envtest","max":16,"roomCode":"A41B9C","network":"online","canReady":true,"canStart":true,"canCancel":false,"canChangeMap":true,"canLeave":true,"canChat":true,"serverTime":1000,"settings":{"ruleset":"DUEL","servertype":"LISTEN SERVER","timelimit":"10","fraglimit":"20"},"players":[{"seat":0,"name":"PlayerName","host":true,"ready":false,"connected":true}]}`

### 9.9 `getlobbychat`

- Reply: a JSON array of the latest **10** messages, oldest first. [SRC]
  `[{"id":number,"seat":number,"name":string,"text":string}, ...]`
- `id` is a stable, increasing local message ID; the page keys rows by it and
  auto-scrolls when the last ID changes. `text` is the decoded UTF-8 text. The page
  renders it as a text node, never as HTML. [SRC]
- `[]` with no session. The page requests this only when the snapshot has a truthy
  `uiVersion`. [SRC]
- Each received chat line is also printed to the engine console as `name: text`. The
  test greps console logs for the message text on every client, including the
  sender. [SRC: test; exact format INF]

### 9.10 `lobby_action:<action>`

Reply: `{"ok":true}` when the action was validated and queued (the next snapshot
carries the result), otherwise `{"ok":false,"error":"..."}`. The page also accepts the
bare string `ok`. [SRC]

| Action | Who | Validation | Effect / console equivalent |
|---|---|---|---|
| `ready` | any seated | `canReady` | `lobby_ready 1` |
| `unready` | any seated | `canReady` | `lobby_ready 0` (cancels a countdown, 9.7) |
| `start` | host | `canStart` | `lobby_start` |
| `cancel` | host | `canCancel` | `lobby_cancel` |
| `close` | any | always | `lobby_close`. The page then queues `cbuf:menu_webcore_title` itself. |
| `setmap:<id>` | host | `canChangeMap`; `<id>` matches `[A-Za-z0-9_]{1,63}` and `maps/<id>.bsp` exists [PROPOSED existence check] | Set the selected map. |
| `setruleset:<id>` | host | `canChangeMap`; `<id>` ∈ {`DUEL`,`DEATHMATCH`,`TEAMDM`,`DOMINATION`} ∪ registered mode IDs | `lobby_set ruleset <id>` |
| `setlimits:<minutes>:<frags>` | host | `canChangeMap`; minutes integer 0–180; frags integer 0–999 | `lobby_limits <m> <f>` |
| `chat:<hex>` | seated | even-length lowercase or uppercase hex; decoded length 1–240 bytes; valid UTF-8; [PROPOSED] reject control characters below 0x20 | `lobby_chat_hex <hex>` |
| other | n/a | n/a | `{"ok":false}`, no effect |

UI map catalogue (fixed in the page): `envtest` (Env Test), `lean` (Lean Test),
`arch_testbed` (Arch Testbed), `arch_crux_test` (Arch Crux Test). The UI ruleset
catalogue: `DUEL`, `DEATHMATCH`, `TEAMDM`, `DOMINATION`. [SRC]

Chat: hex encoding keeps user text out of console syntax. The guest sends the
message to the host; the host checks the sender is the bound seat, assigns the
sender name from its roster (never trusting a name from the guest), and relays to
everyone. Each client keeps the last 10. [SRC]

### 9.11 MenuQC builtins `lobby_registermode` / `lobby_addteam` [SRC: docs/lobby-mode-registration.md]

- `lobby_registermode(string id, string name, string start_cmds)`: registers a mode.
  `start_cmds` is a semicolon-separated list of console lines executed on the **host**
  when the game starts, before `map`.
- `lobby_addteam(string mode_id, string team_id, string team_name, float max_slots, float is_spectator)`:
  adds a team to a mode.
- The registry is cleared at lobby subsystem init and refilled each time `menu.dat`
  loads and runs `m_init`.
- If no modes are registered when a lobby opens, the engine synthesises a fallback mode
  from `lobby_defaultruleset` (default `DUEL`) with team `player` (1 slot, as
  documented) and team `spectator` (15 slots, spectator).
- **With `forceqmenu 1` (the shipped autoexec) MenuQC never runs, so only the
  fallback exists.** The WebCore ruleset list is still accepted (9.10). [INF]
- The declarations are not in this tree's `fteextensions.qc`; they are MenuQC-only.
  Bulwark needs them only if it runs MenuQC.

### 9.12 Game hand-off (STARTING → INGAME) [SRC unless marked]

Host:

1. Publish phase STARTING. The lobby carrier stays open.
2. Apply match settings locally before the map loads:
   - `gamedir base` if the current game directory is empty (so `maps/*.bsp`
     resolves).
   - `deathmatch 1`.
   - `maxplayers <n>`. [PROPOSED] n = `lobby_maxplayers`.
   - `g_gametype <ruleset>`. [UNCERTAIN on case] Nuclide loads
     `progs/<g_gametype>.dat`, and the shipped files are lowercase (`teamdm.dat`,
     `deathmatch.dat`, `domination.dat`). **Bulwark MUST lowercase the ID** (`TEAMDM`
     → `teamdm`). There is no `progs/duel.dat` in this tree, so the `DUEL` ruleset will
     fail to load rules until it is built. Report that clearly.
   - `timelimit <minutes>`, `fraglimit <frags>` (empty → leave unchanged or 0).
   - The registered mode's `start_cmds`, if any.
   - `map <selected map>`.
3. Poll until the game server is actually accepting connections (fully active) on its
   game port. The reference used UDP 27500 (FTE default) while the lobby used 27501.
   There is no fixed sleep. Do not mistake the old title backdrop for readiness.
4. Send each peer exactly one idempotent GAME_STARTING containing the session ID,
   start nonce, map and a **per-peer connect target**:
   - Same-machine peer: `127.0.0.1:<gameport>` (IPv4 loopback; IPv6 endpoint
     selection was a bug).
   - LAN peer: the host address as seen from that peer's authenticated lobby
     endpoint, plus the game port.
   - Online peer: the reachable game URI from the broker/ICE publication (an
     `rtc://...`-style URI). It is never a private IP guess, and never the lobby peer
     address or port.
5. Host phase → INGAME. The pre-game session closes after peers acknowledge or time
   out.
6. Failure (bad map, bind failure, publication failure, timeout): CLOSING with a
   reason to all peers, then everyone returns to the title.

Guest:

1. On GAME_STARTING: acknowledge, close the lobby carrier, `connect <target>` with
   bounded retry (connect-before-listen must succeed within the retry window).
   Duplicate GAME_STARTING messages MUST NOT cause duplicate connects.
2. Phase → INGAME. CSQC initialises only now. The reference test asserts that
   "Initializing Client Game" never appears before Start.
3. On failure: CLOSING with a reason, then the title.

UI: on INGAME, once the client is active, the menu surface closes (4.4). The game's
own RuleC flow (for example duel `showClassSelectionMenu`) then drives the arena page.

### 9.13 Carriers and wire protocol [PROPOSED, not from source]

The sources fix the semantics (9.5–9.12) and say: "one versioned schema, sequence
plus acknowledgement for reliable messages, revisions, idempotent start nonce, peer
IDs, endpoint binding, strict size limits, heartbeats". The reference wire format was
private text datagrams. Its history mentions short type prefixes such as `P:`
(player) and `G:` (game starting). Bulwark does not need to interoperate with FTE
clients, so it can define its own format. A suitable design:

- **Transport:** UDP. The host binds `lobby_port`; guests bind ephemeral ports (two
  processes on one PC must work).
- **Datagram:** magic `BWLB`, `u8 version` (1), `u8 type`, `u64 sessionId`,
  `u32 peerToken` (0 before accept), `u32 seq`, `u32 ackSeq`, `u32 ackBits`, then a
  payload (UTF-8 JSON or compact binary), with a maximum datagram size of 1200 bytes.
  Larger snapshots are fragmented, or their size is bounded by a 16-seat cap.
- **Reliability:** messages flagged reliable are retransmitted every 200 ms until
  acknowledged, and deduplicated per sender by `seq`. Reliable types: JOIN_ACCEPT,
  JOIN_REJECT, SNAPSHOT (on change), CLOSE, GAME_STARTING, CHAT, INTENT_*.
- **Types:** HELLO / JOIN_REQUEST {protocol, name, clientNonce};
  JOIN_ACCEPT {seat, peerToken, snapshot}; JOIN_REJECT {reason: full | phase | version | banned};
  INTENT_READY {ready}; INTENT_LEAVE; INTENT_CHAT {utf8}; SNAPSHOT {full host
  snapshot, with `countdownRemaining` instead of an absolute time};
  CHAT {id, seat, name, text}; PING/PONG; CLOSE {reason}; GAME_STARTING {nonce, map,
  connectTarget}; ACK.
- **Heartbeat:** PING every 1 s each way. A guest silent for 10 s is removed (roster
  change). A host silent for 10 s means the guest closes with "Host timed out".
  The reference online test required membership to stay stable for 12 s.
- **Repair:** the host re-sends a full SNAPSHOT every 2 s even without changes.
- **Discovery:** the host answers a LAN discovery query (broadcast to port
  `lobby_port` on the local subnet, plus unicast to `127.0.0.1:lobby_port`) with
  {protocol, name (host player name or `hostname`), map, players, max, phase, lobby:true}.
  This becomes a `gethostcache` row. Answer only in WAITING. Bound the reply size to
  limit amplification.
- **Online:** the host registers a random 6-character upper-case alphanumeric room
  code (for example `A41B9C`) with a broker (`net_ice_broker`, or Bulwark's own) as a
  *lobby* room, never as a game-server heartbeat. Guests resolve the code and establish
  a peer-to-peer (ICE-like) path, or relay. The same messages then flow. If no path is
  possible, fail closed with a precise error. Same-PC online joins must work. The
  reference needed per-session mDNS alias handling for this.
- Online is an acceptance surface in the plans, but it is the least-proven part of the
  reference. LAN and localhost are the must-have parts.

### 9.14 Unrelated "lobby" names (game code; do not implement in the engine)

`base/src/rules/lobby.qc` (an in-map RuleC warm-up lobby, serverinfo keys
`lobby_state`, `lobby_ready`, `lobby_total`, `lobby_roomcode`, `lobby_host`,
`lobby_countdown`, client commands `ready`/`notready`/`spectate`/`startgame`) and
`base/src/client/lobby_overlay.qc` are part of the game, not the engine.
`src/menu-vgui/ui_lobby.qc` and `src/menu-fn/m_multiplayer.qc` are MenuQC UIs that use
`lobby_create_lan`, `lobby_ready`, `lobby_start`, `lobby_close`, `lobby_maxplayers`,
`sv_background`, `lobby_defaultmap` and the legacy cvars. They are inactive with
`forceqmenu 1`.

## 10. Page summary

| Page (fte://data/web/…) | When shown | Bridge requests | Keys / mouse | Draws over / transparency | Fonts and images loaded from |
|---|---|---|---|---|---|
| `title-menu-v2/index.html` (shipped title) | Title context: startup, after disconnect, after lobby leave/close. URL from `webcore_menu_url`. | `cbuf:` {`lobby_create_lan`, `lobby_create_online`, `menu_webcore_lobby`, `menu_options`, `quit`}; `localsound:misc/menu1\|2\|3`; `hostcache_refresh`; `gethostcache`; `lobby_join:<addr\|code>` | ↑/W, ↓/S, Enter/Space, wheel; Esc closes the Create/Join dialog; mouse hover and click; text inputs (address, room code) with full editing; 180 ms transitions block input | Transparent body over the 3D backdrop (`webcore_menumap`). Its own masked red and pattern layers are semi-transparent. 1024×768 reference, scaled by %. | `style.css?v=16` (embedded `@font-face` data URLs: `standard_07_57`, `hooge_05_57`), inline SVG/PNG data URLs. `assets/` has `hooge_05_57.ttf`, logos and sounds (not referenced at runtime by JS). |
| `title-menu/index.html` (v1, alternate) | Same, if `webcore_menu_url` points to it (engine default) | Same as v2, without `localsound:` | Same (no transitions) | Transparent. 768-high reference. | `style.css?v=4` (three `@font-face`: Conduit ITC, Trade Gothic…), `assets/*.png/svg` via relative URL |
| `lobby-menu/index.html` | After create/join (`menu_webcore_lobby`), until INGAME or session end | `getlobby` (every 250 ms), `getlobbychat`, `lobby_action:*`, `cbuf:menu_webcore_title` (fallback `cbuf:menu_webcore`), `clipboard_copy:<roomCode>`, `localsound:misc/menu1\|2\|3` | ↑/↓/←/→/W/S, Tab/Shift+Tab, Enter/Space on buttons, wheel, Esc (Leave dialog / close dialog), chat input (Enter sends), number inputs; Ctrl/Alt/Meta pass through | Transparent. A 960×720 stage scaled uniformly and centred; only a left panel is semi-opaque. | `style.css?v=18`, `app.js?v=15` (Preact bundle; Esc and lock artwork embedded as data URLs). Fonts by family name only: Inter, `standard 07_57`, Trade Gothic Next LT Pro BdCn (section 2.2). |
| `pause-menu/index.html` | In game: Esc/`togglemenu` with no menu open | `cbuf:` {`webcore_closemenu`, `menu_hostmenu`, `menu_changeteam`, `menu_callvote`, `menu_options`, `lobby_close`, `disconnect`} | ↑/W, ↓/S, Enter/Space, Esc, wheel, hover and click | Transparent over the live world; semi-opaque gradient panel. 720-high reference. | `style.css?v=1`; system font stack (Bahnschrift…). |
| `ql-menu/index.html?mode=arena` | In game, duel/arena WARMUP, on `menu_webcore arena <sel>` from CSQC | `getarena` (every 120 ms), `arena_action:{join,play,choosehero,hero:<id>,ready,unready,spectate,close}` | Mouse (own drawn cursor; `cursor:none`), click; Esc → `arena_action:close`. Number keys are fixtures only in a browser. | Transparent canvas, 640×480 virtual, letterboxed. | `fonts.css` (`@font-face` data URLs for `standard_07_57`, `hooge_05_57`), `assets-data.js` (1.35 MB of `data:` images in `window.QL_ASSETS`), relative `assets/…` as browser fallback |
| `ql-menu/index.html` (no `mode`) | Only by explicit `menu_webcore <url>` (test) | `cbuf:` {`webcore_closemenu`, `menu_options`, `menu_callvote`, `disconnect`, `map_restart`, `ready`, `team …`} | Mouse; Esc in the esc view → close; otherwise Esc is left to the engine | Transparent | Same as above |
| `hud/index.html?v=9` | In game, as the CSQC `gecko` view `webcore_hud` (not the menu surface) when `webcore_hud 1` | `gethud` (fallback `getstats`), `cvar:`/`get_cvar ` for `webcore_hud_*`, every animation frame | None (no input) | `cmd:transparent`; drawn into `HUDMins/HUDSize` over the world. Its own 1440×1080 canvas. | `style.css?v=4` (3.7 MB, 7 embedded `@font-face`: 04b09, Helvetica Neue 97 Black Condensed (+Oblique), Helvetica Neue, Iosevka Charon Mono Bold/Medium, Trade Gothic Next LT Pro BdCn), `assets-data.js?v=3` (data-URL images), `app.js?v=11` |
| `h3-main-menu/index.html` | Only if `webcore_menu_url` is set to it | none (uses embedded `data.js`; `fetch` of `data.json` as fallback) | Own keyboard/mouse | Its own visuals | `style.css`, `data.js`, `assets/` |
| `webcore-test/*.html` | Debug: `menu_webcore <url>` or `playfilm webcore:<url>` | `getstats`; `fetch("smoke.json")` | Click | `hud-smoke.html` is transparent; `index.html` is opaque | Inline / local CSS |

`playfilm webcore:<url>` was a debug path in the reference (letterboxed, Esc skips).
It is not required. [SRC]

## 11. Open questions and risks for the implementer

1. **Start without all-ready** (9.7): the plans and the test disagree. The spec follows
   the test.
2. **`arena_action:join` mapping** (8.4): `cmd play` is proposed; `cmd joinTeam <auto>`
   is the alternative.
3. **`g_gametype` case** (9.12): lowercase is required on case-sensitive filesystems.
   `progs/duel.dat` is missing from this tree.
4. **`lobby_readytime` default** is unknown (5 proposed).
5. **Title backdrop during the lobby** (4.7): keep it local-only, or tear it down.
6. **Menu music playlist gap handling** (4.8).
7. **Pause page URL cvar name** (4.5): not documented.
8. **Spaceful `cbuf:` strings** used by the non-arena ql-menu demo (3.3).
9. **`lobby_snapshot.json` writing condition** (9.3).
10. **CSQC selector state vs engine close:** when the engine closes the arena page
    (Esc / `close`), CSQC still believes it is open. A later CSQC `webcore_closemenu`
    (on phase change) will close **whatever** menu is then open, including a pause menu
    the player just opened. [PROPOSED] Accept this, or let `webcore_closemenu` take an
    optional URL/route argument. QC currently passes none.
11. `menu_hostmenu`, `menu_changeteam` and `menu_callvote` have no defined targets
    under `forceqmenu 1`.

## 12. Sources consulted (all read-only, game side)

- Plans: `docs/plans/2026-07-16-001-feat-webcore-interactive-menu-surface-plan.md`,
  `2026-07-17-001-feat-webcore-lobby-ui-plan.md`, `2026-07-17-002-feat-serverless-lobby-session-plan.md`,
  `2026-07-17-003-feat-mapless-webcore-lobby-plan.md`, `2026-07-21-002-feat-wire-ql-ingame-menu-plan.md`,
  `2026-07-22-001-feat-ql-arena-webcore-vgui-replacement-plan.md`, `hero-actions-and-groove-plan.md` (HUD keys).
- Notes and verification: `docs/notes/2026-07-17-lobby-plan003-implementation.md`,
  `docs/notes/2026-07-17-lobby-u0-build-of-record.md`, `docs/verification/2026-09-12-figma-lobby.md`
  (prose only; the `.patch` beside it was **not** read), `docs/lobby-mode-registration.md`,
  `docs/brainstorms/2026-07-17-serverless-lobby-session-requirements.md`.
- Solutions: `docs/solutions/runtime-errors/{webcore-hud-icu-data-missing,webcore-input-editor-client,online-lobby-mdns-socket-ownership,editor-playtest-menus-disabled,global-audio-deleted-by-multiplayer-rules}.md`.
- Wiki: `wiki/concepts/{webcore-ui,lobby-session,hud-letterboxing}.md`, `wiki/entities/webcore.md`,
  `wiki/log.md` (prose entries), `AGENTS.md`.
- Configs: `base/autoexec.cfg`, `base/configs/Default.cfg`, `base/default_controls.cfg`,
  `base/lobby_*.cfg`, `base/u8h.cfg`, `base/u8j.cfg`.
- QC: `src/client/webcore_arena.qc`, `src/client/cmd.qc`, `src/client/entry.qc`,
  `base/src/hud/hud.qc`, `src/shared/game/HeroActions.qc`, `src/common/fteextensions.qc`
  (gecko declarations), `src/menu-vgui/ui_lobby.qc`, `src/menu-fn/m_multiplayer.qc`,
  `base/src/rules/{duel,arena,lobby}.qc`, `base/src/client/lobby_overlay.qc`,
  `src/server/{cmd_cl,GameRules}.qc`, `src/client/vgui_chooseteam.qc`.
- Pages: `base/data/web/{title-menu-v2,title-menu,pause-menu,ql-menu,hud,lobby-menu,h3-main-menu,webcore-test}/`
  (JS, HTML and CSS; the lobby `src/*.js(x)` and README).
- Game-side tests (drive the shipped binaries; they contain no engine source):
  `Tools/test_lobby_ui_engine.ps1`, `Tools/test_online_lobby.ps1`,
  `Tools/test_webcore_lobby.py`, `Tools/test_webcore_input.py` (only the request
  handling was read).
- Deliberately **not** read: `docs/verification/2026-09-12-figma-lobby-engine.patch`,
  `Tools/stiletto/install_engine.py`, `_lobby_diag/**/*.py` (these embed engine
  source), and the source excerpts inside `wiki/okf/log.md`. A keyword search of the
  last showed fragments of the plugin's `cbuf` allowlist. Section 3.3 is based on what
  the pages send, with those extra names noted as low-confidence.
