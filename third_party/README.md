# Third-party code

Code here is not covered by Bulwark's clean-room rules (it is not engine code
written for Bulwark), but every library must be under a licence that keeps
Bulwark ISC-compatible: ISC, MIT, BSD, zlib or similar. No GPL-only code.

| Library | Version | Licence | Used for |
| --- | --- | --- | --- |
| PanoramaEngine | upstream commit `f3c780e84159a64d0975234420f09933cd8a82c8` | MIT (`PanoramaEngine/LICENSE`) | UI views, see `docs/spec/ui.md` |
| QuickJS (inside PanoramaEngine/Thirdparty) | 0.15.0 | MIT | JavaScript for UI views and entity scripts |
| Lua (`lua-5.4.7`) | 5.4.7, tag `v5.4.7` of github.com/lua/lua (commit `1ab3208a`) | MIT (`lua-5.4.7/LICENSE`) | Lua scripting, see `docs/spec/scripting.md` |
| nanosvg (`nanosvg/`) | commit `239e102ec2c691f2902e20ace2ed36ee4a35cfe6` of github.com/memononen/nanosvg | zlib (`nanosvg/LICENSE.txt`) | SVG images (UI art), `src/render/r_svg.c` |
| FreeType (inside PanoramaEngine/Thirdparty) | bundled | FreeType License (FTL), chosen from its FTL/GPLv2 dual licence | font rasterising |

PanoramaEngine was vendored without its examples, tests and renderer adapters.
Its claim to be an independent implementation of Panorama has not been
checked against Valve's code.

Lua is the interpreter's own sources only: the standalone `lua.c`, the
amalgamation `onelua.c` and the test harness `ltests.*` were left out, and
nothing was changed.

### Changes to PanoramaEngine

PanoramaEngine had no `font-family`: every font was one family told apart by
weight. Bulwark's copy carries a family in the weight value (family × 1024
plus the CSS weight) so the text pipeline, which passes weights everywhere,
needs no new parameters:

- `include/ui/panorama/panorama_style.hpp`, `src/panorama_style.cpp`: the
  family registry (`panorama_register_font_family`,
  `panorama_find_font_family`, `panorama_clear_font_families`), the
  `font-family` property (the first registered name in the list), and
  `font-weight`, bold runs and the user-agent bold keeping the family.
- `src/panorama_font_atlas.cpp`: faces are chosen within the asked family
  (then the default one) by nearest weight, and glyph cache keys hold the
  wider value.

Controls and selectors added for the Qt look-alike themes
(`docs/spec/ui-qt-themes.md`):

- `src/panorama_style.cpp`: the pseudo-classes `:checked` (the `checked`
  class or a selected panel), `:pressed` (same as `:active`), `:first-child`,
  `:last-child` and `:only-child`. Rules with the last three count as sibling
  rules, so siblings don't share computed styles.
- `src/panorama_dom.cpp`, `src/panorama_layout.cpp`: a `ProgressBar`
  control with `#ProgressBarLeft` (filled) and `#ProgressBarRight` children,
  split by `value` / `min` / `max`; scrollbars gain `.ScrollButtonBack` and
  `.ScrollButtonForward` children at the two ends of the track, with no size
  unless styled, and the thumb travels between them
  (`panorama_scrollbar_track`). A `.ScrollTrack` child covers the track
  between the buttons, under the thumb, so a groove can stop at the arrows;
  the thumb is found by class (`panorama_scrollbar_thumb`), not position.
- `src/panorama_paint.cpp`: a linear, axis-aligned gradient with interior
  stops on a rounded box is drawn as rounded end caps plus one square band
  per pixel between them (`add_rounded_gradient_banded`); the single
  triangle fan smeared such gradients diagonally.
- `src/panorama_paint.cpp`: a rounded border takes its side colour for the
  band and lays a different top or bottom colour over the straight run; a
  side of zero width is left open: the band is drawn past that side and
  clipped back, so the other sides run to the end (a tab's open bottom). Inset box-shadows
  are drawn inside the border (the padding box), as CSS draws them.
  A single background image smaller than its box no longer takes the box's
  rounded corners (only a tile that fills the box does).
- `src/panorama_paint.cpp`: a box's own rounded fill, rounded border and
  sharp inset ring are anti-aliased: a solid core half a pixel inside each
  edge and a fringe fading out half a pixel outside it (contours offset with
  sharp corners kept sharp), so edges on whole pixels stay crisp. A rounded
  border with an open (zero-width) side keeps the full arc at a rounded
  corner next to a drawn side, and lets the drawn side run to the end at a
  square corner.
- `src/panorama_layout.cpp`: slider and scrollbar thumbs sit on whole
  pixels, so their border and decoration don't blur.
- `src/panorama_paint.cpp`: a background image drawn at its natural size is
  placed on whole pixels, so small pixel art (grip dots) stays crisp.
- `src/panorama_input.cpp`, `src/panorama_view.cpp`: pressing a scrollbar
  button scrolls 20px with the wheel's smooth-scroll spring, and a pointer
  change keeps the scroll animation walk running.

### Changes to nanosvg

nanosvg ignores `<mask>`, so masked art drew its mask shapes as ordinary
ones. Bulwark's `nanosvg.h` marks them instead (each change is commented
`Bulwark:`): `NSVGshape` gains `mask` (the mask applied to the shape or its
group), `maskDef` (the mask the shape belongs to) and `maskAlpha`
(`mask-type: alpha`, else luminance); the parser reads `mask="url(#id)"`,
`mask-type` and the content of `<mask id=...>` as marked shapes. Drawing the
masks is Bulwark's own code in `src/render/r_svg.c`, which rasterises each
mask and applies it to the runs of shapes that use it.
