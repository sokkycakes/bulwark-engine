# Murrine (GTK 2) look-alike theme for Panorama

`data/panorama/styles/gtk/murrine.css` makes Panorama widgets look like GTK 2
with the Murrine engine, the Cairo theme engine behind Shiki, Bluebird, NOX
and many 2008-2012 themes. It works like the QtCurve theme
([ui-qt-themes.md](ui-qt-themes.md)): every rule reads custom properties, and
a tool measures real GTK 2 renders to fill them in for any Murrine gtkrc.

| Class | Stylesheet | Made from |
| --- | --- | --- |
| `Murrine` | `styles/gtk/murrine.css` | `styles/gtk/murrine-defaults.gtkrc`: every engine option at its default, GNOME 2 default palette |
| `Murrine Bluebird` | `styles/gtk/murrine-bluebird.css` | `/usr/share/themes/Bluebird` (Ubuntu package `bluebird-gtk-theme`) |
| `Murrine MurrinaCandy` | `styles/gtk/murrine-murrinacandy.css` | `/usr/share/themes/MurrinaCandy` (`murrine-themes`) |
| `Murrine NOX` | `styles/gtk/murrine-nox.css` | `/usr/share/themes/NOX` (`murrine-themes`, dark) |
| `Murrine Orangine` | `styles/gtk/murrine-orangine.css` | `/usr/share/themes/Orangine` (`murrine-themes`) |

Include `murrine.css` (then any variant files) and put the classes on a
panel. The gallery is `layout/gtk/gallery.xml`
(`ui_open layout/gtk/gallery.xml input`); its buttons switch variants, as
does `ui_eval <view> "SetTheme('Murrine NOX')"`.

## How it is made

Black-box. `tools/gtkref/gallery.c` is a small GTK 2 program that draws the
gallery's widgets at the same positions as `layout/gtk/gallery.xml` (which
are the Qt gallery's), with DejaVu Sans 9pt at 96 dpi (12px) and whatever
engine the gtkrc in `GTK2_RC_FILES` selects. Under Xvfb it saves the window
and prints each widget's allocation, the slider and stepper extents from
`GtkRange`, indicator sizes, notebook label and frame child positions, and
the style's colours as JSON. With `GTKREF_BLANK=1` buttons, checks, entries,
the combo and the progress bar carry no text, so faces can be measured.

`tools/gtkref/gtkrc2panorama.py` runs it twice (with and without text) and
measures:

- Framed boxes (buttons in six states, entries, combo, progress trough,
  scrollbar thumb): margin, border colour per side, corner radius, a relief
  line outside the frame (one `box-shadow`), and the face's gradient (each
  row's most common colour, so a partial highlight doesn't count).
- What a straight gradient can't carry, such as Murrine's curved, concave or
  glassy glaze and its inner light border: an overlay image of white or black
  at the alpha that turns the measured gradient into the measured pixel,
  stretched over the face (`background-size: 100% 100%`). Flat themes get no
  overlay.
- Progress stripes: the overlay of the full bar, cut to one period (found by
  matching the alpha against itself shifted) and repeated along the fill.
- Fixed-size parts, copied from the render: check and radio indicators per
  state, the slider thumb (normal and hover), the scrollbar steppers and the
  combo arrow.
- Text placement: entry and combo insets from where GTK put the text,
  corrected by Panorama's own offsets for 12px DejaVu Sans; the label colour
  on buttons and progress bars; bold default buttons and frame titles.
- GtkFrame: an etched frame (a dark rectangle and a light one moved down and
  right by a pixel) becomes a dark border, one pixel smaller, with the light
  one as an offset shadow; the title sits on the top line with the window
  colour behind it; the child inset comes from GTK's allocation.
- GtkNotebook: active and inactive tab heights, colours, radius, padding from
  the label positions, the gap between tabs and the pane's border.

Murrine's options (`glazestyle`, `roundness`, `contrast`, `reliefstyle`,
`lightborder_shade`, `glowstyle`, `progressstyle`, `scrollbarstyle`,
`gradient_shades` and so on) are not translated one by one. Real themes set
them per widget class, mix them with colour shades and sometimes pixmaps, so
the tool measures whatever the gtkrc produces instead. No Murrine or GTK
code was read; only GTK's public API is used.

```
sudo apt install gtk2-engines-murrine murrine-themes bluebird-gtk-theme libgtk2.0-dev xvfb python3-pil   # Ubuntu 24.04
python3 tools/gtkref/gtkrc2panorama.py --update data/panorama/styles/gtk/murrine.css          # the defaults
python3 tools/gtkref/gtkrc2panorama.py ~/.themes/Foo/gtk-2.0/gtkrc --name Foo -o foo.css       # a variant: class="Murrine Foo"
python3 tools/gtkref/gtkrc2panorama.py theme.gtkrc --debug /tmp/foo   # also saves the GTK renders
```

Images go to `data/panorama/images/gtk/<name lower-cased>/`
(`murrine-gen/` for the default block). They are pixels of GTK renders of
the theme (indicators, steppers, thumbs, arrows, overlays). The themes in
Ubuntu's packages are GPL, so the variant images are for private use, not
for shipping publicly.

## Checked

2026-11-01, Bulwark at 640x400 with `+set ui_designheight 400` beside the
GTK render, mean absolute difference per pixel over the widget area
(y < 330), out of 255:

| Theme | All widgets | Without hover/pressed |
| --- | --- | --- |
| Murrine (defaults) | 4.09 | 3.58 |
| Bluebird | 4.78 | 3.67 |
| MurrinaCandy | 5.16 | 3.98 |
| NOX | 3.71 | 3.30 |
| Orangine | 6.78 | 3.80 |

The gallery's Hover and Pressed widgets are drawn hovered and pressed in the
GTK reference only; Bulwark's gallery shows them in their normal state, so
the second column leaves them out. Their CSS states were not compared.
Changing single engine options on the defaults (glazestyle 0, 2, 3, 4,
roundness 4, contrast 1.5, reliefstyle 2, progressstyle 0, lightborder,
glowstyle, scrollbarstyle 2) and regenerating stayed at 4.0 to 4.7. Most of
what's left is text antialiasing (GTK hints glyphs to whole pixels).

## Widget structure

The same markup as the Qt themes ([ui-qt-themes.md](ui-qt-themes.md#widget-structure)).
The combo arrow is drawn on the shown option's `Label`, since a box takes
one background image and the combo's face uses it for the glaze.

## Known differences

- Panorama margins sit outside a widget's given size, GTK's inside the
  allocation. Themes with a relief ring round buttons (margin 1px, as in
  Orangine and MurrinaCandy) draw buttons two pixels larger than GTK does for
  the same size.
- Overlays are stretched, so a glaze curve keeps its proportions rather than
  its pixel size on much larger widgets; indicators, thumbs and steppers keep
  their GTK size.
- Murrine's progress stripes start at a different offset depending on the
  fill width; here they start at the fill's left end.
- The slider thumb lands a pixel lower than GTK's on the default theme
  (Panorama centres it on the track).
- An etched frame's inner light line at the top and left is not drawn (one
  shadow per box).
- Hover and pressed states are measured but were not compared in Bulwark.
- Open combo popups, menus, tooltips, spin buttons, tree views, toolbars and
  vertical scales and progress bars are not styled; a pixmap window
  background is not reproduced.
