# Fonts

How Bulwark draws text, aiming for the look of Source's VGUI text.

**Sources.** Which fonts Source uses (Tahoma as the default VGUI font,
Verdana for chat) is general knowledge plus a CS:S chat screenshot. How it
rasterises and draws them comes from a behaviour-only spec, made in a
two-party clean room: one session read Valve's code and wrote the spec, and
the implementer worked only from the spec (see CLEANROOM.md). No Source code
was read by the implementer or copied into Bulwark.

## The default face

Text without a TrueType font of its own uses the default face:

- font slot 0, which `drawstring` uses when the client game never set
  `drawfont`;
- a game font whose file `loadfont` could not find;
- the console, unless `con_font` names a font;
- the engine's own HUD, scoreboard, menus and overlays.

`r_font` (default `Tahoma`) names the face. It may be a file in the game
(`fonts/x.ttf`), an absolute path, or a family name. A family name is
looked for, with any case, as `name.ttf`, `name.otf`, the name without
spaces, `name-Regular`, and a short list of Windows file names (`tahoma.ttf`,
`verdana.ttf`, `lucon.ttf` for Lucida Console, and so on) in:

- Windows: `%WINDIR%\Fonts`
- macOS: `~/Library/Fonts`, `/Library/Fonts`, `/System/Library/Fonts`
- Linux and others: `~/.local/share/fonts`, `~/.fonts`,
  `/usr/local/share/fonts`, `/usr/share/fonts` and a Wine prefix's
  `~/.wine/drive_c/windows/Fonts`, four folders deep.

A game font named by family (`loadfont("", "Verdana", ...)`) is looked for
the same way. When it's missing it falls back to the default face, as
Source's Windows fallback chain ends at Tahoma.

Tahoma belongs to Microsoft and cannot be shipped. When it isn't found,
the bundled **DejaVu Sans** stands in (`data/fonts/`, Bitstream Vera and
Arev licences, permissive and GPL-free). Source on Linux falls back to
DejaVu Sans too. It is built into the executable by `cmake/embed.cmake`. A
fallback font with "bold" in its name (`Verdana Bold`, `fonts/x-bold.ttf`)
uses the bold: `r_font` plus " Bold" (`tahomabd.ttf`), else DejaVu Sans
Bold. `listfonts` prints which file is in use.

`r_quakefont 1` brings back the 8x8 bitmap font (font8x8, public domain)
for that text, exactly as before. Fonts a game loads itself are not
affected.

## Rasterising

- **Size.** The size is the character cell height (ascent plus descent), as
  Windows reads a positive font height. The OS/2 table's usWinAscent and
  usWinDescent give the cell (hhea ascender/descender if missing), and the
  baseline sits `ascent` pixels below the top. Each size is baked when first
  used, at the exact screen pixel height (virtual height times the 2D
  scale).
- **Hinting.** Glyphs are rasterised by FreeType with the font's hinting
  when it is built in (PanoramaEngine's copy, else the system's), otherwise
  by stb_truetype without hinting.
- **Antialiasing.**
  - Source draws scheme fonts without `antialias` as one bit per pixel. The
    default face, installed fonts and fallbacks are drawn that way (FreeType
    mono target). `r_font_antialias 1` smooths them.
  - Fonts from game files are smoothed, since games design for that.
  - `loadfont` takes `antialias=0` or `antialias=1` in its sizes string.
  - Smoothed coverage maps linearly to alpha, with no gamma.
- **Characters.** Latin-1, Greek, Cyrillic, general punctuation and
  currency signs are baked for each size.
- **Metrics.** Advances are whole pixels from the hinted glyphs. There is
  no kerning, and line height is the cell height.

## Drawing

- Atlases are point-sampled with no mipmaps. Glyphs baked at the drawn size
  are put on whole screen pixels, so each texel lands on one pixel.
- **Effects.** These work like Source's font flags:
  - `dropshadow`: the glyph in black at full glyph alpha, one screen pixel
    down and right.
  - `outline`: the glyph in black one pixel out in all eight directions.
  - Both stay one pixel at every size, and are drawn under the whole
    string before the text.
  - The default face gets `r_font_shadow` (default 1) and `r_font_outline`
    (default 0), except in the console.
  - A game asks per font through loadfont's sizes string: `outline=1`
    (documented in fteextensions.qc), `shadow=1` or `dropshadow=1`.
  - The same file loaded with different flags gets a slot of its own.
- Text colour multiplies the white glyph, so effects stay black.
- **Console.** With a TrueType face the console draws in Source's console
  text colour, (216, 222, 211). `con_textsize 0` picks 8 for the Quake
  font and 12 for a TrueType face. `8` counts as `0`, because every config
  saved before this change holds it.
- The engine scoreboard places each field on its own instead of padding
  with spaces, which works with proportional fonts.

## Not done

- Source's `blur`, `scanlines` and `additive` flags.
- Proportional sizing (Source scales `tall` by screen height / 480; Bulwark
  uses its whole-number 2D scale).
- Source leaves characters above U+00FF unsmoothed on Windows.
- Chat layout belongs to the game. Source's chat line spacing is the font
  height plus one pixel.
