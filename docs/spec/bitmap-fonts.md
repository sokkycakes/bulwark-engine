# Bitmap fonts and shared fonts

Bulwark draws X11 bitmap fonts (BDF, PCF, gzipped PCF) and TrueType fonts
made of square pixels ("pixel TTFs") crisp: 1-bit glyphs, scaled by whole
numbers, placed on whole screen pixels. This covers engine text (console,
`loadfont`, the client game's fonts) and Panorama UI text (CSS
`font-family`).

Sources: the BDF 2.1 specification (Adobe), the X.Org PCF format notes, and
probing files. No X server, FreeType or FTE code was read. FreeType was only
*run* as an oracle (see Verification).

## Shared fonts

`data/fonts/` in Bulwark's data directory (beside the executable, or the
source tree's `data/`) is searched after every game directory, like the rest
of the engine data. Every game can use a font placed there:

- engine text: by family name (`"Visitor TT1 BRK"`), by file name without
  extension (`visitor1`), or by path (`fonts/visitor1.ttf`), in `con_font`,
  `loadfont` and the client game's font paths;
- Panorama: by family name or file name in CSS `font-family`
  (`font-family: anorexia;`). Only the engine data directory's `fonts/` is
  registered this way, after the game's own `panorama/fonts/` and `ui_font`
  faces, so a game's fonts stay the fallback for unknown families.

A game's own `fonts/` folder is indexed for engine text too (as TrueType face
names already were). Panorama also loads `.bdf`, `.pcf` and `.pcf.gz` files
from `panorama/fonts/`.

Fonts whose licence doesn't allow redistribution with the engine are kept out
of git: `data/fonts/.gitignore` lists them, and they ship in a private bundle
unpacked over the build (see the README of the test build).

## Formats (src/common/bmfont.c)

Every glyph becomes a 1-byte-per-pixel image (0 or 255) with X metrics: ink
box `w h`, offset `xoff` (right of the pen) and `yoff` (bottom edge above the
baseline), and an advance. A font has a family, weight (CSS scale), italic,
ascent, descent and a nominal pixel size.

**BDF.** Header keywords `FONTBOUNDINGBOX`, `SIZE`; properties `FAMILY_NAME`,
`WEIGHT_NAME`, `SLANT`, `PIXEL_SIZE`, `FONT_ASCENT`, `FONT_DESCENT`,
`CHARSET_REGISTRY`, `CHARSET_ENCODING`, `DEFAULT_CHAR`. Per glyph `ENCODING`
(`-1 n` uses `n`), `DWIDTH`, `BBX` (default `FONTBOUNDINGBOX`), and `BITMAP`
rows of hex, leftmost pixel in the high bit. Missing ascent and descent come
from the bounding box.

**PCF.** `"\1fcp"`, a table count and table records `type format size
offset` (little-endian). Each table starts with its own format word
(little-endian); bit 2 of it selects big-endian for the rest of the table.
Used tables: properties (1), accelerators (2) and BDF accelerators (0x100) for
ascent/descent when the properties lack them, metrics (4; format 0x100 is
the compressed 5-byte form, each byte biased by 0x80), bitmaps (8) and BDF
encodings (0x20). Bitmap rows are padded to `1 << (format & 3)` bytes; with
bit 3 clear the leftmost pixel is the low bit; when the byte order differs
from the bit order, bytes are swapped within scan units of
`1 << (format >> 4 & 3)` bytes. Glyph `w = rsb - lsb`, `h = ascent + descent`,
`xoff = lsb`, `yoff = -descent`.

**gzip.** RFC 1952 header, then raw deflate (stb_image's inflate).

**Code points.** ISO10646 and ISO8859-1 codes are Unicode code points. Other
registries keep their codes, which is right for ASCII; other 8-bit sets would
need tables (not done).

**XLFD weights.** `Medium` is the regular X weight, so it maps to 400, as
do `Regular`, `Normal` and `Book`; `Bold` 700 and so on.

## Pixel TrueType fonts

A TrueType font counts as a pixel font when its printable ASCII glyphs (at
least 20 of them) use only straight horizontal and vertical lines (no
curves, no diagonals) and every coordinate and advance lies on one grid:

1. the gcd of all of them, when that is at least 1/64 em, else
2. the smallest whole pixels-per-em from 4 to 64 at which every value is
   within one font unit (or 2.5% of a pixel) of a grid line: editors round
   a grid that doesn't divide the em (TallPixelFont: 2048 units at 9 px,
   stored as 227, 455, 682), and some hand-placed points are a unit or two
   off (04b09's `e`).

The grid gives the font's own pixels per em. Engine text rasterises each
glyph once at exactly that size (stb_truetype) and thresholds at half
coverage, then treats the font as a bitmap font. Panorama sets FreeType to
that size and renders 1-bit, unhinted. Files over 2 MB aren't checked.

Observed on sokky's fonts: visitor1 10 ppem, CWEBL 13, tallpixelfont 9, 04b09
8, Minusio 16. None of the DejaVu, Liberation, JetBrains Mono, FreeFont,
Carlito, Caladea, IPA Gothic, Iceberg or Kode Mono faces in the container is
taken for a pixel font.

`r_pixelfonts 0` (archived, default 1) turns the detection off: TrueType
pixel fonts then go to gdifont / stb_truetype or Panorama's normal path as
before. BDF and PCF are always bitmap fonts.

## Drawing

**Scale.** A bitmap font has one size. Text asked for at `tall` screen
pixels (engine: the cell height, `sy` × framebuffer scale; Panorama:
`font-size` × UI scale against the font's pixels per em) is drawn at
`k = round(tall / own size)`, at least 1 and at most 16. So text keeps
square pixels and can come out a little bigger or smaller than asked.
`r_bitmapfont_scale N` (engine text only) forces `k = N`.

**Engine text** (`src/render/r_bmfont.c`, `draw_text_gdi` in `r_draw.c`):
glyphs are scaled k times into a 512×512 nearest-sampled atlas per k, placed
at whole framebuffer pixels from the top of the line's cell, advanced by
`adv × k`. No kerning. `listfonts` shows such a font as `(bitmap)`;
`bitmapfonts [name]` lists the indexed fonts and what a name resolves to.
Font name lookup order per entry of a font list: bitmap/pixel font, then
gdifont, then stb_truetype.

**Panorama** (`third_party/PanoramaEngine`, marked `Bulwark:`):
`PanoramaFontAtlasFace::pixel_ppem` (> 0 pixel TTF, < 0 BDF/PCF strike) is
set by the host (`face_pixel_ppem` in `src/ui/ui_panorama.cpp`). The atlas
keeps such a face at its own size (`FT_Select_Size` / `FT_Set_Char_Size`),
loads glyphs `FT_LOAD_TARGET_MONO | FT_LOAD_NO_HINTING`, scales bitmap,
bearings and advances by k, and skips kerning. `PanoramaGlyph::pixel_grid`
tells paint the size of a screen pixel, and paint rounds the glyph quad to it
when the text's transform only translates. With texels then mapping one to
one, the atlas's linear filter samples texel centres and the text stays
1-bit.

## Verification

- `tests/bmfont` (ctest `bmfont`): a BDF fixture with bold/italic
  properties, a descender, a code above 0xFF (U+263A), a
  zero-size space and a default glyph; the same font written as PCF in 12
  layouts by `tests/bmfont/mkpcf.py` (byte/bit order, padding 1/2/4/8, scan
  unit 1/2/4, compressed metrics, gzip). FreeType 2.13 reads the generated
  PCF files with the same pixels, so the writer is not just agreeing with
  the reader.
- 54 real BDF/PCF/.pcf.gz files from sokky's bitmap-fonts collection (one per
  family folder, 34,907 glyphs) compared glyph by glyph against FreeType's
  1-bit rendering: bitmaps, bearings, advances, ascent and descent all equal.
- On screen (Xvfb, Mesa llvmpipe): console text in each of the six shared
  fonts at 1× and 2×, and `layout/fonts/shared-fonts.xml` at UI scales 1 and
  1.33. Screenshots contain exactly two colours (background and text).
- `bmfdump <file> [text]` prints what the loader makes of a font.
