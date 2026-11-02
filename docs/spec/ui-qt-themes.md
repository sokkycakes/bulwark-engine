# Qt look-alike themes for Panorama

Two stylesheets make Panorama widgets look like Qt's widget styles:

| Theme | Root class | Stylesheet | Imitates |
| --- | --- | --- | --- |
| Fusion | `QtFusion` | `data/panorama/styles/qt/fusion.css` | Qt 5.15 Fusion, standard palette |
| QtCurve | `QtCurve` | `data/panorama/styles/qt/qtcurve.css` | QtCurve 1.9 (Qt 5) with its default settings |

Include the stylesheet and put the root class on any panel; everything
inside takes the look. A gallery of every widget is
`layout/qt/gallery.xml` (`ui_open layout/qt/gallery.xml input`); its
buttons switch between the themes, as does `ui_eval <view> SetTheme('QtCurve')`.

## How the values were found

Black-box, like the GDI text work: `tools/qtref/gallery.py` draws the same
widgets at the same positions with real Qt (PyQt5, `-platform offscreen`,
DejaVu Sans 12px, the style's `standardPalette()`), and
`tools/qtref/probe.py` prints pixel rows and columns through each widget.
Colours, sizes, radii and gradient ends in the stylesheets come from those
measurements. No Qt or QtCurve source was read. The check marks, arrows,
radio dots, grips and the QtCurve groove are our own drawings
(`tools/qtref/glyphs.py`, output in `data/panorama/images/qt/`), sized and
coloured to match.

To compare, render the gallery at 640x400 with `+set ui_designheight 400`
(one design pixel per screen pixel) and set it beside `gallery.py`'s output.
On 2026-10-31 the mean absolute difference per pixel was 3.7 (Fusion) and
4.9 (QtCurve) out of 255; most of what's left is text antialiasing.

## Running QtCurve's own settings editor

QtCurve's editor was a System Settings module, which current KDE no longer
hosts. `tools/qtref/qtcurve-config.py` opens it as a plain window. It needs
PyQt5, QtCurve's Qt 5 style and a D-Bus session (without one the editor
shows "Already Open" instead of its pages). Ubuntu 24.04 still packages
QtCurve, so on another distro run it in a distrobox:

```
distrobox create -n qtcurve -i ubuntu:24.04
distrobox enter qtcurve -- sudo apt install -y python3-pyqt5 kde-style-qtcurve-qt5
distrobox enter qtcurve -- python3 tools/qtref/qtcurve-config.py
```

Save writes `~/.config/qtcurve/stylerc` (the home folder is shared with the
box); presets can be imported and exported from its first page.

## Using your own QtCurve settings

`qtcurve.css` reads every colour, gradient, radius, margin and glow from
custom properties. The block between its `BEGIN qtcurve2panorama` and
`END` markers holds them for QtCurve's defaults, and
`tools/qtref/qtcurve2panorama.py` writes that block from any stylerc:

```
python3 tools/qtref/qtcurve2panorama.py ~/.config/qtcurve/stylerc --update data/panorama/styles/qt/qtcurve.css
python3 tools/qtref/qtcurve2panorama.py mine.stylerc --name Mine -o mine.css   # a variant: class="QtCurve Mine"
```

It draws probe widgets with real QtCurve and that stylerc, measures them
(border colours per side, the fill down each widget as a gradient with only
the stops it needs, corner radius, etch and glow lines, sizes from Qt's own
geometry calls) and writes the values. Everything it writes can also be
edited by hand. Checked on 2026-10-31 with the defaults (mean difference
5.0/255) and with appearance=shinyglass, round=slight, defBtnIndicator=tint,
buttonEffect=none (4.1/255).

It also writes small images into `images/qt/<variant>/` (`qtcurve-gen/` for
the default block). Some it draws itself in measured colours: the check mark
(a tick, or an X with `xCheck=true`), a tile for a patterned window background
(`bgndAppearance=striped`) and the fading side line of a faded group box
(`groupBox=faded`). Others are copied from the QtCurve render as they are:
thumb grips (`handles`, `sliderThumbs`: lines, dots), one period of a striped
progress fill (`stripedProgress`), the default button's corner triangle and
the scrollbar arrow buttons and groove (the groove sits on `.ScrollTrack`, so
it stops at the arrows and the window shows around them).
Those are pixels of sokky's own private render, fine for this private work,
but not something to ship publicly.

Measurements compare each pixel with what the window draws behind it at that
spot, so a striped window doesn't read as widget pixels. A coloured mouse-over
ring inside the frame (`coloredMouseOver`) becomes an inset `box-shadow`,
which replaces the etch line in that state because Panorama draws one shadow
per box. A group box title that sits on the frame line gets the window's
colour and tile behind it to cut the line. Bold labels on default buttons and
group boxes are measured, and tab sizes, heights, side colours and inner
highlight lines come from the render; where the pane's top line runs inside
the tab bar, the active tab covers it so the two join flush.

`qtcurve-sokky.css` is a variant made this way from sokky's own settings
(`qtcurve-sokky.stylerc`, 2026-10-31): class="QtCurve Sokky", or the gallery's
Sokky button. Mean difference from QtCurve with the same stylerc: 5.8/255
(most of what's left is text antialiasing and one-pixel offsets); the defaults
block: 5.05/255.

What it does not carry over yet: the radio dot and the combo arrow stay our
fixed images (`images/qt/qtcurve-*`); the combo box's split arrow button,
triangular slider handles and other non-rectangular shapes are drawn as plain
rounded boxes; the faded group box's fill and the window stripes under its
title don't line up exactly; `scrollbarType` layouts with two buttons at one
end show one button at each end. Settings for widgets the themes don't cover
(menus, toolbars, window borders, item views) have no effect.

## Dark palettes and shaped windows

`qtcurve2panorama.py --palette FILE` draws the probes with a Qt palette
given as `Role=#rrggbb` lines (`Group:Role=` for one colour group), read by
`tools/qtref/qtpalette.py`; `gallery.py <style> <out> [palette]` takes the
same file for the reference render. `styles/qt/adwaita-dark.palette` holds
GTK 3 Adwaita-dark's colours. A dark palette draws frames with much smaller
colour steps, so the converter scales its edge thresholds by how far the
palette's Dark role sits from Window, relative to QtCurve's own palette
(1 for light palettes, so their output is unchanged). Variant `SokkyDark`
(`qtcurve-sokky-dark.css`, class `QtCurve SokkyDark`) is sokky's stylerc
with that palette.

QtCurve frames only rectangles. For a panel with another outline,
`tools/qtref/qtshape.py shape.svg out.png --tile <variant>/window.png
--border <c> --light <c>` paints the SVG paths' union as a window: the
variant's window tile inside, and QtCurve's frame edges (a dark line, a
lighter line inside it) following the outline, anti-aliased. The JTF main
menu (`layout/qt/jtf-menu.xml`, opened by `exec panorama/jtf-menu.cfg`) uses
it for the outline of sokky's Figma group, `tools/qtref/shapes/jtf-menu.svg`
(paths copied verbatim). Each Figma square is itself a square QtCurve push
button, with its turned label beside it. The menu is laid out at 960x540
(Figma's 1080p values halved) and drawn with `ui_pixelated 1`, so the
window stripes and frame lines come out 2 screen pixels thick on 1080p; the
frame image is painted with `qtshape.py --scale 0.5 --x0 99` (geometry
scaled, lines and tile kept 1 pixel; `--x0` crops the part of the tab that
hangs off screen). The layout asks for that itself (`bwdesignheight="540"
bwpixelated="1"` on `#JtfScreen`), so other views are unaffected. The cfg
registers the Figma fonts (Iceberg, Kode Mono, SIL OFL, in
`panorama/fonts-jtf/`) before opening the view, because a family must exist
when the styles are read.

## Palette

Each theme declares Qt's palette roles as custom properties on its root
class (`--window`, `--window-text`, `--base`, `--button`, `--button-text`,
`--text`, `--highlight`, `--highlighted-text`, `--disabled-text`) plus the
shades the style derives from them (`--outline`, `--focus-outline`,
`--button-top` and so on). A dark or custom palette is a rule that sets new
values, for example `.QtFusion.Dark { --window: #353535; ... }`. The derived
shades are not computed from the roles; set them too.

## Widget structure

| Qt widget | Panorama markup | Parts the themes style |
| --- | --- | --- |
| QPushButton | `<Button><Label text=".."/></Button>` | `.default`, `:hover`, `:active`, `:checked` (class `checked`), `:disabled` |
| QCheckBox | `<ToggleButton text=".."/>` | `.TickBox`, `:checked`, `:disabled` |
| QRadioButton | `<RadioButton group=".." text=".."/>` | `.RadioBox`, `:checked` |
| QLineEdit | `<TextEntry/>` | `:focus`, `:disabled` |
| QComboBox | `<DropDown>` with `Label` options | closed state only (the arrow is a background image) |
| QSlider (horizontal) | `<Slider min max value/>` | `#SliderTrack`, `#SliderTrackProgress`, `#SliderThumb` |
| QProgressBar | `<ProgressBar value=".."/>` (0..1 unless `min`/`max`), optional `Label.ProgressBarText` | `#ProgressBarLeft`, `#ProgressBarRight` |
| QScrollBar | any `overflow: scroll` panel | `VerticalScrollBar` / `HorizontalScrollBar`, `.ScrollThumb`, `.ScrollButtonBack`, `.ScrollButtonForward` |
| QGroupBox | `Panel.GroupBox` > `Label.GroupBoxTitle` + `Panel.GroupBoxFrame` | |
| QTabWidget | `Panel.TabWidget` > `Panel.TabBar` (of `RadioButton.Tab`) + `Panel.TabPane` | `.Tab:checked` |

`ProgressBar`, the scrollbar buttons and the `:checked`, `:pressed`,
`:first-child`, `:last-child` and `:only-child` pseudo-classes were added to
Bulwark's PanoramaEngine for these themes (`third_party/README.md`).

## Known differences

- SokkyDark: the inactive tabs measure as white (the converter's fill
  column lands on QtCurve's light edge line), and the default button's bold
  label isn't detected.

- QtCurve's default scrollbars have a second up arrow next to the down one
  (KDE layout); Panorama scrollbars have one button at each end.
- Qt draws a progress bar's text in two colours, split where the fill ends.
  The themes use one colour (the highlighted text colour).
- Open combo box popups, menus, tooltips, spin boxes, vertical sliders, tree
  and list views, and focus rectangles are not styled yet.
- The group box frame in QtCurve fades out downward in Qt; here it is a
  plain gradient with no bottom border.
- Tabs are radio buttons, so switching pages is up to the layout's script.
