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
