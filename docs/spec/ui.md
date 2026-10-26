# UI views (Panorama-style)

Bulwark draws menus and HUD panels with the vendored
[PanoramaEngine](https://github.com/CitroenGames/PanoramaEngine) library (MIT,
see `third_party/README.md`). A *view* is one XML layout with its CSS and
JavaScript. Views are drawn after the game's own 2D, in the order they were
opened, through the renderer's 2D API (`R_Draw2DTriangles`).

This is Bulwark's own interface. Nothing here comes from another engine; the
builtin names are new and are not FTEQW extensions.

## Files

Everything is read through the file system under `panorama/` in a game dir:

| Path | Contents |
| --- | --- |
| `panorama/layout/*.xml` | layouts, opened by path relative to `panorama/` |
| `panorama/styles/*.css` | Panorama CSS, included from a layout |
| `panorama/scripts/*.js` | scripts, included from a layout |
| `panorama/images/...` | PNG/TGA images; `{images}/x.png` in CSS or XML means `panorama/images/x.png` |
| `panorama/fonts/*.ttf\|otf` | every font here is loaded at start; weight is guessed from the file name (`Bold`, `Light`, ...) |

## Scale

Layouts are authored for a design height, `ui_designheight` (default 1080,
archived). The UI scale is framebuffer height ÷ design height, so a layout
fills the same share of the screen at any resolution. Widths follow the aspect
ratio, so percentage positions are the safe way to place things horizontally.

## QuakeC builtins (CSQC)

```
float(string layout, float flags) ui_open;            // flags: 1 = takes input; returns a view number, 0 on failure
void(float view) ui_close;
float(float view) ui_isopen;
void(float view, string event, string json) ui_post;  // json may be "" for no value
```

Scripts call back into the client game with

```
void(float view, string action, string arg) CSQC_UIAction;
```

which is called when the progs defines it. The action `"cmd"` never reaches
QC: its argument is run as a console command.

## JavaScript API

Each view gets a `Bulwark` object:

- `Bulwark.Send(action, arg)` sends an action to the client game (`CSQC_UIAction`).
- `Bulwark.Cmd(command)` runs a console command.
- `Bulwark.On(event, fn)` registers a handler for `ui_post` events. `fn` gets
  the decoded JSON value.
- The engine posts `"key"` events to every view that takes input, with
  `{key: "<key name>", down: true|false}`, before the key is handled by
  Panorama's own focus navigation.

## Input

A view opened with flag 1 takes keys and the mouse before CSQC. While any such
view is open the mouse is not grabbed and the console is not forced open when
no game is running.

## Console commands

| Command | Use |
| --- | --- |
| `ui_open <layout> [input]` | open a layout; `input` (or `1`) makes it take input |
| `ui_close [view\|all]` | close one view, or all of them |
| `ui_post <view> <event> [json]` | post an event to a view's `Bulwark.On` handlers |
| `ui_eval <view> <js>` | run JavaScript in a view |
| `ui_list` | list open views |
| `ui_click <x> <y>` / `ui_key <name>` | synthetic input, for tests and headless runs |
| `ui_stats` | per-frame update and draw times |

## Not done yet

Backdrop blur, releasing textures in the renderer, a geometry cache, UTF-8
text entry beyond Latin-1, and single draws larger than 65535 vertices.
