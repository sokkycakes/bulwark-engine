# Web export tests

`webtest.mjs` plays an exported game in Chrome and Firefox the way a
playtester would. Each test is named for what a player should see:

1. loads and offers Play (with no console errors)
2. Play starts the game and draws the map (and sound runs)
3. keyboard moves the player and the mouse turns the view
4. Esc frees the mouse and opens the menu; the game takes it back
5. losing the pointer lock opens the menu, as Esc does on the desktop
6. the canvas follows the window and high-DPI screens
7. F11 and vid_fullscreen go fullscreen; Esc in the menu leaves it
8. settings are saved and come back on the next visit
9. a controller starts the game from the Play card and plays it
10. the Back button doesn't leave a game
11. quitting shows the end card
12. a broken export says what went wrong
13. joins a desktop server through the WebSocket relay (`bulwark-ded` and
    `tools/web/wsrelay.py`)

Firefox refuses fullscreen under automation, so that check is skipped there.

## Running

```sh
# an export that starts a map
tools/web/build.sh build-web
python3 tools/web/export.py --basedir ~/nuclide --game base --engine build-web \
    --out out/web-base -- +map ai

cd tests/web
npm ci
node webtest.mjs --export ../../out/web-base --browser all
```

Options:

- `--browser chrome|firefox|all` (default all)
- `--chrome PATH`, `--firefox PATH` (otherwise the usual places are tried)
- `--only Esc,Back` runs the tests whose names contain those words
- `--headed` shows the browser windows
- `--out DIR` for screenshots and `results.json` (default `out/`)
- `--ded PATH --basedir DIR` runs the online test against a desktop
  `bulwark-ded` with that game folder; without them it is skipped

Firefox needs a real display for WebGL, and a sound server for Web Audio to
start. On Linux without a desktop, run it as CI does:

```sh
pulseaudio --start --exit-idle-time=-1
xvfb-run -a -s "-screen 0 1280x800x24" node webtest.mjs ...
```
