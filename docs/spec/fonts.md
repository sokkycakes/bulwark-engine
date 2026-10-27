# Fonts

How Bulwark draws text that has no font of its own. This is a design
choice, not something learned from another engine.

## The default face

Source draws VGUI text with Windows fonts named in its scheme files, and
its default control font is Tahoma. Bulwark does the same for any text
without a TrueType font of its own:

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

Tahoma belongs to Microsoft and cannot be shipped. When it isn't found,
the bundled **DejaVu Sans Condensed** (`data/fonts/`, Bitstream Vera and
Arev licences, permissive and GPL-free) stands in. It is built into the
executable by `cmake/embed.cmake`. Like Tahoma, it is a narrow humanist
sans in the Verdana style. `listfonts` prints which file is in use.

`r_quakefont 1` brings back the 8x8 bitmap font (font8x8, public domain)
for that text, exactly as before. Fonts a game loads itself are not
affected.

## Drawing

- Each size is baked when first used, at the exact screen pixel height
  (virtual height times the 2D scale), into the smallest atlas it fits.
  Glyphs are then put on whole screen pixels, so small text stays sharp
  the way GDI-drawn VGUI text does.
- Source's `dropshadow` and `outline` font flags have counterparts here.
  A shadow is the glyph in black one screen pixel down and right, and an
  outline is the glyph in black one pixel out in all eight directions. Both
  are drawn under the whole string before the text, and the offset grows
  by a pixel for every 24 screen pixels of height.
  - Text in the default face gets `r_font_shadow` (default 1) and
    `r_font_outline` (default 0), except in the console, which has its
    own dark background.
  - A game asks for them per font through loadfont's sizes string:
    `outline=1` (documented in fteextensions.qc), `shadow=1` or
    `dropshadow=1`.
- `con_textsize 0` picks 8 for the Quake font and 12 for a TrueType face.
  `8` counts as `0` because every config saved before this change holds
  it.
- The engine scoreboard used to line its columns up with spaces. Now
  each field is placed on its own, which works with proportional fonts.
