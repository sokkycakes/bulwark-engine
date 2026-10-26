# VGUI layout (`.res`) and scheme files

Read by `src/common/keyvalues.c` (the text syntax) and
`src/common/vgui_res.c` (colours, scheme fonts and panel positions).
Nothing here draws. These files turn text into a tree and numbers that a
menu or HUD renderer can use.

## How this was established

The general shape comes from public descriptions of Valve's KeyValues text
and of VGUI `.res` and scheme files: quoted keys and values, nested blocks
in braces, and `//` comments. No engine, SDK or tool source was read for
this file.

Every rule below was then checked against real content: 1,183 `.res` files
from Counter-Strike Online's `cstrike`, `dstrike`, `estrike`, `fstrike` and
`cstrike_na` archives, CRC-checked on extraction and never committed here.
Run `tools/rescheck` over a folder of them to repeat the check, or set
`BULWARK_RES_FIXTURES` and run `ctest -R vgui-fixtures`.

Each rule carries a label:

- **Observed**: seen in the fixtures.
- **Convention**: the publicly documented meaning in other games that use
  VGUI. These are not confirmed for CSO, because confirming them needs the
  original client running, and that has not been done.
- **Design**: our choice where the files don't settle it.

## Text and encoding

- **Bytes, not characters (observed).** 1,170 files are plain ASCII. 8 hold
  CP949 (EUC-KR) Korean label text, and 5 are UTF-8 without a BOM. No file
  is UTF-16 and none has a BOM. The reader keeps bytes as they are and does
  no conversion.
- **Line endings (observed).** 683 files use CRLF, 409 use LF, and 36 mix
  them. CR counts as whitespace.
- **NUL ends the text (observed rule, design behaviour).** 86 files have NUL
  bytes, always after the last closing brace, 1 to 3 bytes from the end. In
  7 of them a newline follows the NUL. The reader stops at the first NUL,
  as a C string would.

## Tokens

- **Whitespace.** Space, tab, CR, LF, VT and FF.
- **Comments (observed).** `//` runs to the end of the line. It can follow a
  value with no space between them: `"0 0 0 0"//227"` is the value
  `0 0 0 0` followed by a comment. 3,541 comment lines appear in the
  fixtures. No `/* */` comments were seen, and they are not supported.
- **Quoted strings (observed).** The reader runs from `"` to the next `"`.
- **No escapes (observed).** A backslash is an ordinary character:
  `"Resource\OptionsSubVideo.res"` keeps the backslash. Five files have a
  backslash, every one in a Windows path.
- **Unquoted tokens (observed).** An unquoted token runs until whitespace,
  `{`, `}`, `"` or `//`. Scheme files use them for keys such as `Scheme`
  and `Colors`. A token may hold at most `KV_MAX_TOKEN` (8,192) bytes.
- **Braces.** `{` and `}` are tokens of their own.
- **Not observed, not supported:** `#base` and `#include` directives, and
  `[$WIN32]`-style conditionals. No fixture contains them.

## Structure

- A file is a list of entries. Each entry is a key followed by a value
  string or by a `{ ... }` block of further entries.
- **Top level (observed).** 1,179 files have one top-level block. 3 have
  two. One of those is `clientscheme.res`, whose second entry is a stray
  `.` after the root, which the reader drops as a key without a value.
  `cstrike/maps/de_storm.res` is not a VGUI file: it is a GoldSrc map
  resource list, one path per line. The reader pairs its lines up as keys
  and values, so it means nothing as KeyValues.
- **Depth (observed).** Layout files nest 2 deep (root, then one block per
  panel). Schemes nest up to 4 deep (Scheme, Fonts, a font, one definition).
  The reader allows `KV_MAX_DEPTH` (64).
- **Duplicate keys (observed).** 5,002 duplicates appear in the same block,
  for example two `ReceivedUserLabel` panels. The reader keeps all of them,
  in file order. `KV_Find` returns the first. Which one the original client
  uses is **unresolved**. Taking the first is the convention, not confirmed.
- **Case (convention).** Key lookups ignore ASCII case, because the files
  mix `fieldName`/`FieldName` and `xpos`/`XPos`.

## Malformed input and how the reader repairs it

32 of the 1,183 files are malformed. The reader repairs each case below
the same way every time and counts every repair in `kv_report_t`. Under
`KV_STRICT` it rejects the file instead. A repaired file is never reported
as clean.

| Case | Files | Repair | Label |
| --- | --- | --- | --- |
| Quote left open at end of line, e.g. `"tall" "45` then newline | 24 | The string ends at the newline. A CR just before it is dropped. | Design |
| Key with no value before `}` or end of input, e.g. `"brighttext"` then `}` | 15 | The key is dropped. | Design |
| `{` where a key was expected (follows a broken quote, e.g. `2ndDamage"`) | 1 | The block is kept with the key `""`. | Design |
| `}` with no open block | 0 | Ignored | Design |
| Blocks still open at end of input | 1 | Closed | Design |
| Quote still open at end of input | 0 | The string ends there. | Design |

**Why strings end at the newline.** If a quoted string instead ran on to
the next `"` (as `KV_SPAN_LINES` does), every later quote in the file would
pair up the wrong way. Over the 32 repaired files, that reading makes 1,941
keys containing tabs or newlines. Ending at the newline makes none, and it
gives values such as `tall = 45` that match the neighbouring panels.

What the original client does with these files is **unresolved**. Some of
the panels may well be broken in the shipped game too. Among the repaired
files is the one HUD layout seen so far,
`cstrike/resource/hud/zbs/damagemeter.res`.

## Scheme files

A scheme is a top-level block (`"Scheme"`) holding these sections, found
by name with any case. The fixtures are `clientscheme`, `trackerscheme`
and `tutorscheme` from `cstrike_na`, and `voxelscheme` from `estrike`.

- **`Colors` (observed).** Entries such as `"BaseText" "255 255 255 255"`.
  A colour is 3 or 4 integers separated by spaces or tabs; a missing alpha
  is 255 (convention), and values are clamped to 0-255 (design).
  `Colors` can hold nested blocks (`"Menu" { "BgColor" ... }`). These are
  reached with a `/` path, `Menu/BgColor`.
- **`BaseSettings` (observed).** Entries whose value is either a colour
  name, a literal colour, an image path or a number. A colour name can
  lead to another `BaseSettings` entry. The reader follows at most 8 such
  steps.
- **Fallback scheme (observed need, design rule).** `tutorscheme.res` uses
  15 colour names, such as `BaseText` and `ListBG`, that it does not define
  itself. All 15 are defined in `clientscheme.res`. `vgui_scheme_t.fallback`
  lets the caller name the scheme to search next, at most 4 schemes deep.
  With `clientscheme` as the fallback, every colour reference in the three
  `cstrike_na` schemes resolves. Their only non-colour entries are the two
  `TitleBarIcon` image paths in each.
- **`Fonts` (observed).** Each font has numbered definitions:

  ```
  "Default" { "1" { "name" "Verdana" "tall" "12" "weight" "0" "yres" "480 599" } ... }
  ```

  Keys read: `name`, `tall`, `weight`, `yres`, and the flags `antialias`,
  `dropshadow`, `underline`, `italic`, `outline`, `symbol` and `additive`.
  - **Picking a definition (convention).** The first definition whose
    `yres` range holds the screen height wins. A definition with no `yres`
    fits every height.
  - **No range fits (design).** The definition with the nearest range is
    used.
- **`CustomFontFiles` (observed).** Numbered font paths, e.g.
  `resource/font/CalibriFZY4JW.ttf`.
- **`Borders` (observed, not interpreted).** Only `tutorscheme` has this
  section. The reader exposes the block but does not interpret it yet.

## Layout files

- **Panels (observed).** The root block holds one block per panel. The keys
  seen most often, in order of frequency, are `tall`, `wide`, `ypos`,
  `xpos`, `ControlName`, `fieldName`, `visible`, `AutoResize`, `enabled`,
  `PinCorner`, `tabPosition`, `textAlignment`, `paintbackground`,
  `labelText`, `dulltext`, `brighttext`, `wrap`, `scaleImage`, `image` and
  `zpos`. CSO adds its own, such as `threepicstgapath`, `ninepicstgapath`
  and image paths with an `@n` suffix.
- **Name (convention).** A panel's name is its `fieldName`, or the block
  key when `fieldName` is missing.
- **Positions (observed forms, conventional meaning).** Of 76,000 position
  and size values:
  - all but 85 are a plain integer
  - 35 are `c-N` and 2 are `cN`
  - 22 are `-N` and 14 are `rN`
  - 10 have trailing whitespace
  - 1 is empty

  The meanings used are the VGUI convention: `rN` is N from the far edge
  of the parent, and `cN` is N from the parent's centre. Both are
  **unconfirmed for CSO**. The one empty value (`tall` in
  `popup_mapselect_list1.res`) is reported as a bad coordinate and read
  as 0.
- **Defaults (design).** `visible` and `enabled` default to 1 when absent.
- **Scale (design).** The base resolution CSO lays panels out against is
  **unresolved**. For example, the damage meter's `xpos` is `r170` from a
  parent whose size the file does not give. `VGUI_LayoutPanels` therefore
  takes the parent size and a scale from the caller.

## Limits and failure

These make the reader fail instead of repairing:

- a token over 8,192 bytes
- nesting over 64 blocks
- more than 2^20 entries

The reader never reads outside the buffer it is given. `rescheck --fuzz`
feeds it truncated and randomly corrupted copies of every fixture. It
checks that the tree agrees with the report's counts, and that strict mode
accepts exactly the inputs that parse with no repair.
