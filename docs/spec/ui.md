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
| `panorama/fonts/*.ttf\|otf` | every font here is loaded at start; weight is guessed from the file name (`Bold`, `Light`, ...); each is also a `font-family` of its file name without the extension |
| `{game}/<path>` | in an image `src`, a CSS `url()` or a script include: any game file, used in place (a game's own art and scripts) |

Bulwark's own data directory (`data/` in the source tree, or beside the
executable; `-enginedata <dir>` picks another) is searched after the game
directories, so the engine can ship layouts a game doesn't have, such as the
WebCore page ports in `data/panorama/layout/web/` ([webcore.md](webcore.md)).

## Images and fonts

Images are PNG, TGA, JPEG, BMP and SVG files, and `data:` URLs of any of them
(base64, or percent-encoded SVG text). SVG is drawn by nanosvg at twice its
size, small pictures at 512 pixels across. Pictures are clamped at their
edges.

CSS `font-family` picks the first listed family that was registered: each
file in `panorama/fonts/`, plus any added with
`ui_font <family> <game file> [weight]`. Weights pick among a family's faces
by nearest match. Views open before a font is registered are restyled.
PanoramaEngine's own font atlas knew no families; Bulwark carries the family
in the font weight key (family × 1024 + weight), see `third_party/README.md`.

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
- `Bulwark.Query(request)` and the global `fte_query(request)` ask the engine
  something and return its reply string at once (the WebCore bridge, see
  [webcore.md](webcore.md); other games may answer it with their own
  handler). Requests over 4096 bytes get `""`.
- `Bulwark.Width()`, `Bulwark.Height()`: the view's size in layout pixels.
  `Bulwark.MouseX()`, `Bulwark.MouseY()`: the pointer, in the same units.
- `location.search` and `location.href` exist when the view was opened for a
  page address (`menu_webcore`, `gecko_create`); `window` is the global
  object.
- The engine posts `"key"` events to every view that takes input, with
  `{key: "<key name>", down: true|false, shift, ctrl, alt}`, before the key
  is handled by Panorama's own focus navigation.

A script may close its own view (asking for it through a command or the
bridge): the view stops drawing and taking input at once and is freed at
the start of the next frame.

## Embedded views

A view opened with flag 2 (`UI_EMBED`) is not drawn with the others; the
engine draws it where something asks, laid out at the size it is drawn. The
WebCore layer uses this for `gecko_create` views, which the client game draws
with `drawpic(pos, name, size, rgb, alpha)`; `drawgetimagesize(name)` gives
their size.

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

Hit-testing ignores transforms, so a scaled panel is clicked where it would
be unscaled. Script `Panel.style` setters cover only background, size,
opacity and transform; set other properties in the `style` attribute given
to `$.CreatePanel`. CSS filters (`blur`, `drop-shadow`), `mask-image` and
backdrop blur are not drawn.

Layout behaviours that differ from a browser and that ports must allow for:

- A transform that transitions from `none` to a translate jumps at the
  halfway point (the operation lists differ); give the resting state an
  explicit `translateY( 0px )`.
- The default `overflow: squish` shrinks children larger than their parent;
  use `overflow: noclip` (or `clip`) for pages that letterbox a stage.
- A `Button` without a background is not hit; create rows with
  `hittest: "true"`.
- A wrapped `Label`'s height does not reach a `fit-children` parent; put
  wrapped labels straight into the flowing container.
- Children are placed inside their parent's border, not at its outer edge.
- A label's baseline sits at its top plus `max( 0, ( line-height - size ) / 2 )`
  plus the font's ascender, lower than a browser's for fonts with tall
  ascenders; ports offset labels by the difference.

Not done yet: releasing textures in the renderer, releasing textures in the renderer, a geometry cache, UTF-8
text entry beyond Latin-1, and single draws larger than 65535 vertices.
