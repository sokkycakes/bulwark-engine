# GDI-compatible TrueType text (gdifont)

`src/client/gdifont.c` reproduces what Windows GDI returns for TrueType
fonts (TEXTMETRIC, ABC widths, `GetGlyphOutline` bitmaps and
`ExtTextOutW` output) using FreeType on every platform. It is
self-contained and not used by the renderer.

## How it was established

Everything here comes from black-box measurements of real Windows. No Wine,
ReactOS or Source Engine material was used (see `CLEANROOM.md`).

- **Starting point:** a behavioural spec measured from Windows build 26100
  on a GitHub `windows-latest` runner (font smoothing on, ClearType,
  contrast 1200). The section numbers (§) used below and in code comments
  refer to it.
  - Grid: Tahoma, Verdana, Arial, Courier New, Trebuchet MS and Times New
    Roman; weights 400 and 700; cell heights 8–24; quality
    NONANTIALIASED and ANTIALIASED; 261 codepoints (ASCII, Latin-1,
    Cyrillic and a few symbols).
- **CI:** `.github/workflows/gdiref.yml` runs the check on every change to
  the module or tools, or by hand.
  1. It builds `tools/gdiref/gdiref.c` with mingw and runs it on
     `windows-latest`.
  2. It renders the same font files (which GDI hands back through
     `GetFontData`) with `tools/gdidump`.
  3. It scores the two with `tools/gdiref/compare.py`.
  4. `best_ppem.py` scores ppem −1/0/+1, and `diagnose.py` prints the
     detail behind each percentage.
- **Labels:** **[O]** observed, **[F]** a rule fitted to the captures,
  **[D]** from public documentation, **[U]** unknown.

| run | what changed |
|---|---|
| 40463306407 | the spec ported to C (baseline) |
| 40463949748 | VDMX "step down" rule (wrong, reverted), empty-glyph records |
| 40464365435 | final VDMX rule, tmMaxCharWidth from the head bbox |

## Rules

### Interpreter

The TrueType bytecode interpreter v35 is set with
`FT_Property_Set(lib, "truetype", "interpreter-version", 35)`, not with an
environment variable (§4.1).

### Size selection and line metrics (§3)  [F, 204/204 configs exact in run 40464365435]

1. **Usable VDMX.** VDMX counts only through a ratio record with
   bCharSet 1 covering 1:1 (§3 rule 1). [O]
2. **With usable VDMX:** walk the entries in ascending ppem and keep the last
   one whose `yMax − yMin ≤ tall`, but **stop at the first one whose height
   equals `tall`**. tmAscent = yMax, tmDescent = −yMin, tmHeight =
   yMax − yMin (this can be less than `tall`).
   - This changes the spec, whose rule was "the smallest ppem with the
     largest fitting height" (193/204). That rule's 11 misses were all ties
     whose height was `tall − 1`, where Windows took the *largest* ppem.
     For example, Arial 700 has VDMX 13: (13, −3) and 14: (13, −3): `tall`
     17 gives ppem 14, but `tall` 16 gives 13.
   - A "start at the win-metrics ppem and step down" rule explained those
     11 but missed 33 others (run 40463949748, diagnose.py section 3). In
     all 33, the height equalled `tall` and Windows took the smallest ppem
     (Courier New 700 at 16: ppem 12, 13 and 14 all have height 16, and
     Windows took 12). The final rule fits both runs.
3. **Without usable VDMX, or when no entry fits:** ppem = the largest p
   with `round(p·(winAscent+winDescent)/unitsPerEm) ≤ tall`. When nothing
   fits, ppem also stays below the table's first entry.
   - tmHeight = `tall`
   - tmAscent = `round(winAscent·tall/(winAscent+winDescent))`
   - tmDescent = the rest
4. **The other metrics:**
   - tmInternalLeading = tmHeight − ppem (§3 rule 4).
   - tmExternalLeading = `max(0, lineGap − ((winA+winD) − (hheaAsc−hheaDesc)))`
     scaled to the ppem. [D] This is the OpenType spec's description of
     Windows line spacing; 204/204.
   - tmAveCharWidth = `round(xAvgCharWidth·ppem/upem)`. [F] 204/204.
   - tmMaxCharWidth = `round((head.xMax − head.xMin)·ppem/upem)`. [F] 204/204.
     The scaled hhea `advanceWidthMax` (the spec's FreeType value) and the
     largest hinted advance over all glyphs were both too small in every
     config. Arial's bbox is 5457 units, giving 19 at ppem 7 and 21 at
     ppem 8, as Windows reports.
   - tmOverhang = 0, as measured for bold faces.
   - tmWeight is the requested weight; 0 means 400. [D]

### 1-bit glyphs, GGO_BITMAP (§4.2)

- FreeType `FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME` at the §3 ppem.
- Rows are DWORD-aligned, MSB first. The origin is (bitmap_left,
  bitmap_top).
- **Empty glyphs** (space, U+00A0): GDI returns size 0, no data, a 1×1
  black box and origin (0, 0). [O] run 40463306407, diagnose.py section 8.

### Greyscale glyphs, GGO_GRAY8 (§4.3)

- An FreeType grey render, levels `round(v·64/255)`, DWORD-aligned rows. It
  ignores gasp. [U] mapping, see open items.

### ExtTextOut (§4.4)

- **Quality 0:** always 1-bit.
- **Quality 1:** greyscale only where the gasp range covering the ppem has
  GASP_DOGRAY; otherwise 1-bit.
- **1-bit output is the GGO_BITMAP glyph drawn at pen + A.**
  - [O] Windows' own 1-bit ExtTextOut equals its GGO_BITMAP in
    8803–8874 of 8874 glyphs per face (99.2–100%, diagnose.py section 1,
    runs 40463306407 and later).
  - Spec open item 4 ("ExtTextOut q0 sits below GGO_BITMAP") was a bug in
    the Python reference, not a GDI behaviour. `ftref.py` read
    `glyph.bitmap_left/top` after reloading the slot for the grey render
    (freetype-py's `face.glyph` is a live view of one slot), so it placed
    mono pixels at the grey render's origin. With correct placement,
    ExtTextOut q0 tracks GGO_BITMAP within 0.4 points for every face.
- **Greyscale output:** 17 evenly spaced levels, as the spec says. [U]
- **Underline and strikeout:** drawn from `post` and OS/2 metrics. Not
  measured. [U]
- **Synthetic bold and italic:** not implemented. Pass the real bold or
  italic file. [U]

### ABC widths (§5)

A = bitmap_left of the 1-bit render, B = its width, C = rounded advance
− A − B, at the §3 ppem.

## Measured match rates

Run 40464365435, gdifont against Windows build 26100, per face and weight.
tm is tmHeight, tmAscent, tmDescent and tmMaxCharWidth together, over 17
sizes. ABC and 1-bit are at quality 0.

| face | weight | tm | ABC | 1-bit (GGO_BITMAP) | ExtTextOut q0 | ExtTextOut q1 | GGO_GRAY8 |
|---|---|---|---|---|---|---|---|
| Tahoma | 400 | 17/17 | 100.0% | 89.2% | 89.2% | 61.5% | 15.4% |
| Tahoma | 700 | 17/17 | 99.9% | 92.2% | 92.2% | 61.2% | 15.3% |
| Arial | 400 | 17/17 | 98.4% | 94.8% | 94.8% | 59.7% | 22.5% |
| Arial | 700 | 17/17 | 98.1% | 96.7% | 96.8% | 33.0% | 20.7% |
| Courier New | 400 | 17/17 | 99.7% | 95.2% | 95.2% | 74.4% | 4.8% |
| Courier New | 700 | 17/17 | 99.9% | 90.1% | 90.5% | 35.2% | 2.6% |
| Times New Roman | 400 | 17/17 | 98.2% | 88.7% | 88.8% | 47.8% | 6.8% |
| Times New Roman | 700 | 17/17 | 97.7% | 87.0% | 87.1% | 31.5% | 6.2% |
| Trebuchet MS | 400 | 17/17 | 99.3% | 85.5% | 85.7% | 50.1% | 13.1% |
| Trebuchet MS | 700 | 17/17 | 99.1% | 87.8% | 88.1% | 25.3% | 12.8% |
| Verdana | 400 | 17/17 | 99.5% | 86.5% | 86.5% | 51.4% | 14.0% |
| Verdana | 700 | 17/17 | 99.9% | 93.3% | 93.3% | 51.4% | 14.1% |

For comparison, the spec's Python reference (`ftref.py`, run 40462351632)
scored:

| measure | ftref.py | gdifont |
|---|---|---|
| 1-bit | 79.0–94.1% | 85.5–96.7% |
| ABC | 91.6–100% | 97.7–100% |
| ExtTextOut q0 | 69.5–88.8% | 85.7–96.8% |
| size choice | 193/204 | 204/204 |

## Open items

1. **The greyscale mapping** (GGO_GRAY8 and greyscale ExtTextOut). The
   coordinator is working on this in the Python reference. Evidence from
   run 40463306407 that may help:
   - Windows' greyscale ExtTextOut uses exactly 15 non-zero levels:
     104, 124, 140, 154, 167, 178, 189, 199, 208, 217, 225, 233, 240, 248
     and 255.
   - Set against Windows' own GGO_GRAY8 at the same pixel, a GGO_GRAY8
     value near 4c maps to level c of a 4×4 sample count, with counts 0
     and 1 drawing nothing. This is consistent with a 4×4 sampled coverage
     put through a contrast or gamma table.
   - The GGO_GRAY8 misses against FreeType's area coverage are mostly
     62/63 → 64 and 1/2 → 0, which suggests point sampling (8×8) rather
     than area coverage.
   - Not tested in CI.
2. **ABC widths at ANTIALIASED quality** come out 81.8–96.4%, against
   97.7–100% at quality 0, so GDI seems to measure A and B differently
   when greyscale is on. Not investigated (1-px width mismatches are being
   handled with the greyscale work).
3. **Small sizes:** 1-bit at heights 8–10 is 114–223 of 261 for Tahoma,
   Verdana, Times New Roman and Trebuchet MS. best_ppem confirms the ppem
   is right, so it is hinting or rasterization (gasp gridfit is being
   looked at upstream).
4. **Remaining 1-bit misses at ordinary sizes:** mostly "same box size,
   other pixels", single pixels. [U] Dropout control or rounding.
5. **Not covered by the capture:** underline and strikeout geometry,
   synthetic bold and italic, fonts with `hdmx`/`LTSH` or embedded bitmaps,
   and TrueType collections (gdidump opens face 0).
6. **otm fields:** the `GetOutlineTextMetrics` fields that gdiref records
   are not produced by gdidump. compare.py doesn't use them.
