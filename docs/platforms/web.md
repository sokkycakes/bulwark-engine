# Bulwark on the web

The web build is the engine itself, compiled to WebAssembly with Emscripten.
It draws with WebGL 2 and plays sound through Web Audio. Players join servers
over WebSockets. A game exported for the web is one folder of static files,
so any web host can serve it: itch.io, GitHub Pages, nginx, or
`tools/web/serve.py`.

The web build runs the same progs and data as the desktop build and draws the
same frame, pixel for pixel (CI checks this with Nuclide's `ai` map). What a
browser does differently is listed under "What a browser can't do".

## In short

```sh
# once: the Emscripten SDK
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install 7.0.1 && ~/emsdk/emsdk activate 7.0.1
. ~/emsdk/emsdk_env.sh

# the engine
tools/web/build.sh build-web

# a game, as a folder ready to host
python3 tools/web/export.py --basedir ~/nuclide --game base --engine build-web \
    --out out/web-base --title "Nuclide" -- +map ai

# try it
python3 tools/web/serve.py out/web-base      # http://localhost:8080/
```

## Building the engine

`tools/web/build.sh <dir>` runs `emcmake cmake` and builds the `bulwark`
target. It produces `bulwark.js` and `bulwark.wasm`. The web build:

- uses WebGL 2 (OpenGL ES 3.0 shaders), with no WebGL 1 fallback;
- runs the frame loop from the browser's animation frame, with no threads,
  so a host doesn't need COOP/COEP headers;
- links the Source plugin in, since a browser can't load plugins at run
  time;
- fetches over http(s) with the browser's own `fetch`;
- leaves out the host tools, the tests, `qcrun` and `bulwark-ded`.

Memory starts at 256 MB and grows up to 2 GB.

## Exporting a game

```
tools/web/export.py --basedir DIR --game NAME|MANIFEST.fmf --engine BUILD --out DIR
                    [--title T] [--exclude PATTERN]... [--pack-mb 32]
                    [--fullscreen auto|always|never] [--max-pixel-ratio N]
                    [--no-confirm-leave] [--zip] [--level 6]
                    [-- engine arguments]
```

Engine arguments after `--` are passed to the engine on every start, just as
on the desktop. Use them to open a map (`+map ai`) or set cvars.

The export folder holds:

| File | What it is |
|---|---|
| `index.html`, `bulwark-shell.js`, `bulwark-shell.css` | the page (`tools/web/shell/`) |
| `engine/bulwark-<hash>.js`, `.wasm` | the engine |
| `data/<name>-<hash>.bwpack` | the game's files and Bulwark's `data/`, in packs of up to `--pack-mb` MB, gzipped when that saves space |
| `bulwark-web.json` | what the page loads, and the engine arguments |
| `manifest.webmanifest` | so a phone or handheld can add the game to its home screen |

Every name carries a content hash. A host can cache these files forever, and
a new export never mixes with an old one in a player's browser. The page keeps
downloaded packs in the browser's Cache Storage, so a second visit starts at
once.

Sources and editor files are left out by default: `.git`, `mapsrc/`, `*.blend`,
`*.psd`, `*.qc`, `*.py`, `*.prt`, autosaves and logs. Add your own with
`--exclude`. `--zip` also writes `<out>.zip`, ready to upload to itch.io.

Saves live in the browser's IndexedDB under `/user`: configs, bindings,
screenshots, and anything else the game writes. They survive reloads and new
exports. The engine's new `-userdir` argument does this, and works on the
desktop too, where it makes the engine write to a folder of its own instead
of the game folder.

## Hosting

- **Any static host works.** The page needs no special headers. Serve
  `.wasm` as `application/wasm`; most hosts already do.
- **itch.io:** upload the `--zip` file as an HTML game and tick
  "SharedArrayBuffer support" off (it isn't needed). Set the viewport to
  1280×720 or larger and turn on itch's fullscreen button.
- **Your own server:** `tools/web/serve.py out/web-base --lan` serves the
  folder to your network with the right types and caching. Use a real web
  server (Caddy, nginx) for the public internet.
- **https is required** for fullscreen on some phones, for controllers in
  some browsers, and for anything not on `localhost`.

### Page options

Add these to the address:

| Option | Effect |
|---|---|
| `?autostart` | skip the Play button (the browser may still hold back sound until a click) |
| `?connect=host[:port]` | join that server instead of running the export's map |
| `?args=+map%20x` | extra engine arguments |
| `?nocache` | download everything again |
| `?debug` | show the engine's log in the browser console |

For tests and tools the page also offers `window.bulwark.command("...")`,
`window.bulwark.state()` and `window.bulwark.log`.

## Playing online

A web page can't send UDP. The web engine sends each game packet as a
WebSocket message to `ws://<host>:<port>/` instead, or `wss://` when the page
is on https. `tools/web/wsrelay.py` accepts those connections and passes the
packets to a normal server's UDP port. Browser players and desktop players
then share one server:

```sh
bulwark-ded -game base +map ai &                       # UDP 27500
python3 tools/web/wsrelay.py --listen 27500 --server 127.0.0.1:27500   # TCP 27500
```

Browser players type `connect myserver.example` in the console, or open the
page with `?connect=myserver.example`. Desktop players connect to the same
address as before.

### Lobbies (Stiletto's Create Lobby and Join Lobby)

A lobby hosted in the desktop game uses two ports: the lobby's (27501) and,
once the host starts, the game's (27500). The host runs one relay for both:

```sh
python3 tools/web/wsrelay.py --listen 27500 --listen 27501
```

Browser players open Join Lobby and type the host's address in Address/IP.
They see the lobby, chat, ready up, and join the game when the host starts
it, just as desktop players do; desktop players join the same lobby as
before.

- Room codes only reach lobbies on the host's own network, so in a browser
  they don't find anything; the page says to join by address instead.
- A lobby created in a browser works for playing alone, and its page says
  that others can't join it.

- For a page on https, put the relay behind a TLS proxy on the same port,
  since browsers refuse `ws://` from https pages. With Caddy:
  `myserver.example:27500 { reverse_proxy 127.0.0.1:28500 }`, and run the
  relay with `--listen 127.0.0.1:28500 --server 127.0.0.1:27500`.
- `--origin https://mygame.example` lets only your own page join. Repeat it
  for more origins.
- `--max 64` caps the number of browser players.
- The relay opens one UDP socket for each browser player, so the server sees
  each player as its own address. It uses only Python's standard library.

## Playing in the browser

The page is built so a playtester never has to troubleshoot:

- **Loading:** a progress card, and a clear message if the browser has no
  WebGL 2 or a file is missing, with Reload and Copy log buttons.
- **Play:** click Play, or press Enter, or press A or Start on a controller.
- **Mouse:** clicking Play captures the mouse with pointer lock, using raw
  input where the browser offers it. Esc frees the mouse and opens the game's
  menu, as on the desktop. If the browser lets the mouse go on its own
  (alt-tab, a notification), the game opens its menu too. Going back into the
  game shows "Click to resume", because browsers only give the mouse back on
  a click.
- **Fullscreen:** F11, the button shown over menus, or the game's own
  `vid_fullscreen` and video menu. In fullscreen the page locks the keyboard
  where the browser allows it (Chrome), so Esc reaches the game, and on a
  phone it locks the screen to landscape. By default (`--fullscreen auto`)
  Play goes fullscreen on phones and handhelds only; `always` does it
  everywhere and `never` leaves it to the player.
- **Window size:** the game follows the page's size and the screen's pixel
  density, up to about 1440p worth of pixels (`--max-pixel-ratio` changes
  that).
- **Keys:** while playing, the browser's own shortcuts (Ctrl+S, Ctrl+F,
  F5, Backspace, Firefox's quick find) go to the game instead.
- **Sound:** starts on the first click, tap or key, as browsers require.
- **Leaving:** closing the tab mid-game asks first (`--no-confirm-leave`
  turns that off). Settings are saved when the tab is hidden or closed.
  Quitting from the game's menu shows "Thanks for playing" with Play again.

### Handhelds and controllers

Android handhelds with built-in controls (Ayn Thor, AYANEO, Anbernic,
Retroid, Logitech G Cloud) and the Steam Deck play the web build in Chrome or
Firefox with no mouse or keyboard:

- the built-in controls are a standard gamepad, which the engine reads as on
  the desktop;
- A or Start on the cover card starts the game, and the cursor hides once a
  controller or touch is in use;
- a toast says when a controller connects or disconnects;
- Android's Back button opens the game's menu instead of leaving the page;
- "Add to Home screen" gives the game its own fullscreen, landscape icon.

Browsers only allow fullscreen and sound after a touch or click, not after a
button press. So after a controller start the page asks for one tap on the
screen.

## What a browser can't do

- **No LAN discovery or master server lists.** A page can't broadcast or
  reach UDP masters, so the server browser stays empty on the web. Join with
  `connect` or `?connect=`, or give players a link.
- **No hosting from the browser.** A listen server or lobby in a tab only
  takes players from the same tab. Host in the desktop game or `bulwark-ded`,
  with the relay.
- **A background tab pauses.** Browsers stop the animation frame in hidden
  tabs, so the game stops too, and an online player times out after a while.
- **No plugins at run time.** Plugins have to be linked in when the engine
  is built, as the Source plugin is.
- **Firefox under automation refuses fullscreen.** This only affects the
  tests, not players.

## Tests

`tests/web/` plays an export in Chrome and Firefox. CI runs it on every
change (`.github/workflows/web.yml`) and keeps the export as a downloadable
artifact. See `tests/web/README.md`.
