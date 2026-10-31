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

It also draws a few small images of its own in measured colours, into
`images/qt/<variant>/` (`qtcurve-gen/` for the default block): the check
mark (a tick, or an X with `xCheck=true`), the default button's corner
triangle (`defBtnIndicator=corner`) and a tile for a patterned window
background (`bgndAppearance=striped` and the like). A coloured mouse-over
ring inside the frame (`coloredMouseOver`) becomes an inset `box-shadow`,
which replaces the etch line in that state because Panorama draws one shadow
per box. Bold labels on default buttons and group boxes are measured too.

`qtcurve-sokky.css` is a variant made this way from sokky's own settings
(`qtcurve-sokky.stylerc`, 2026-10-31): class="QtCurve Sokky", or the gallery's
Sokky button. Mean difference from QtCurve with the same stylerc: 5.7/255.

What it does not carry over yet: the radio dot, arrows, grips and the
scrollbar groove stay our fixed images (`images/qt/qtcurve-*`);
`scrollbarType` only switches arrows on or off; striped progress fills,
faded group-box frames, the combo box's split arrow button, triangular
slider handles and other non-rectangular shapes are drawn as plain rounded
boxes; one-pixel inner shadow lines are dropped. Settings for widgets the
themes don't cover (menus, toolbars, window borders, item views) have no
effect.

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

- QtCurve's default scrollbars have a second up arrow next to the down one
  (KDE layout); Panorama scrollbars have one button at each end.
- Qt draws a progress bar's text in two colours, split where the fill ends.
  The themes use one colour (the highlighted text colour).
- Open combo box popups, menus, tooltips, spin boxes, vertical sliders, tree
  and list views, and focus rectangles are not styled yet.
- The group box frame in QtCurve fades out downward in Qt; here it is a
  plain gradient with no bottom border.
- Tabs are radio buttons, so switching pages is up to the layout's script.
