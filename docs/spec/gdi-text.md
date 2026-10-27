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
| 40465569113 | variants: point-sampled grey, hinting mode, sample phase, no-gridfit, ABC at q1 |
| 40466309200 | variants: ABC from sampled boxes, sample nudges, 1-bit by centre sampling |
| 40467000659 | variants: forced dropout modes, mono-mode hinting for grey |
| 40467469320 | variants: v40 interpreter at small ppems |
| 40467896104 | variants: sample phase on one axis; final scores |

**How variants were scored.** From 40465569113 on, each run scores
several rule variants at once.
- `gdifont_tuning_t` (gdifont.h) holds one switch per rule under test,
  and `gdidump --tune` sets them.
- The workflow renders each variant from the same capture, and
  `tools/gdiref/score.py` prints identical fractions per face, both overall
  and for heights 8–10.
- score.py also shows how Windows' quality 1 output relates to its own
  quality 0 output.
- `tools/gdiref/sheets.py` prints Windows-vs-gdifont text strips as base64
  PNGs (`SHEET <label> <data>` lines) into the job log.

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

### Greyscale glyphs, GGO_GRAY8 (§4.3)  [F]

- **Rule:** each pixel's value is the number of 8×8 sample points, at
  sub-pixel centres ((i + 0.5)/8 of the pixel), that fall inside the hinted
  outline, with no dropout control. That gives 0–64. The black box is the
  tight box of non-zero pixels, and rows are DWORD-aligned. GGO_GRAY8
  ignores gasp.
  - The hinting is grayscale mode (`FT_LOAD_TARGET_NORMAL`).
  - The sampler scales a copy of the hinted outline by 8, renders it
    with FreeType's 1-bit rasterizer (`FT_OUTLINE_IGNORE_DROPOUTS`), and
    counts per pixel.
- **Why sampling:** run 40463306407 found that Windows' misses against
  FreeType's area coverage were mostly 62/63 → 64 and 1/2 → 0, and that
  its values take all 65 levels. That pattern points to counted sample
  points rather than area.
- **Result** (run 40465569113): 12.3% → 76.0% identical, and 96–98% of
  pixels equal within glyphs of the same box (run 40466309200).
- **Alternatives tested:**

| alternative | GGO_GRAY8 identical | run |
|---|---|---|
| mono-mode hinting (GETINFO without grayscale) | 75.2% | 40465569113, 40467000659 |
| samples at i/n | 0.8% | 40465569113 |
| samples at i/n on x only | 1.1% | 40467896104 |
| samples at i/n on y only | 1.0% | 40467896104 |
| 1/64 sub-pixel nudge, each of 4 directions | 27.8–30.0% | 40466309200 |

  The nudge result shows that FreeType's handling of samples that fall
  exactly on an edge matches Windows.

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
- **Greyscale output:** a 4×4 sample count c (same sampler, sub-pixel
  centres) maps to a level. [F]
  - Counts 0 and 1 draw nothing; 2…16 map to 104, 124, 140, 154, 167,
    178, 189, 199, 208, 217, 225, 233, 240, 248 and 255.
  - These 15 levels are the only non-zero values Windows produced
    (run 40463306407).
  - Set against Windows' own GGO_GRAY8, a value near 4c lands on level c.
  - The levels probably depend on the ClearType contrast setting (1200 on
    the runner). [U]
  - Result: greyscale ExtTextOut 48.5% → 84.2% (run 40465569113), and
    88.3% with the ABC change below.
- **Underline and strikeout:** drawn from `post` and OS/2 metrics. Not
  measured. [U]
- **Synthetic bold and italic:** not implemented. Pass the real bold or
  italic file. [U]

### ABC widths (§5)

A = bitmap_left of the 1-bit render, B = its width, C = rounded advance
− A − B, at the §3 ppem.

**At ANTIALIASED quality, where gasp turns greyscale on**, A and B come
from the box of the 4×4 sampled glyph (count ≥ 1). The advance still comes
from the hinted load. [F]

- **Evidence that this differs from q0** (run 40465569113, score.py):
  - Windows' GGO_BITMAP at quality 1 equals quality 0 in 99.9–100% of
    glyphs.
  - Its ABC is identical too in configs whose text is 1-bit, but only
    70–97% identical in configs whose text is greyscale.
- **Result:** ABC at q1 88.3% → 98.1% (run 40466309200).
- **Alternatives tested:**

| alternative | ABC at q1 identical |
|---|---|
| FreeType area-coverage box, gasp-grey ppems only | 95.2% |
| the same box at every ppem | 91.9% |
| 8×8 sampled box | 97.1% |
| 4×4 sampled visible pixels only (count ≥ 2) | 96.6% |
| grayscale-mode hinting for ABC and GGO_BITMAP | no gain, and GGO_BITMAP got worse |

The 1-bit glyphs (GGO_BITMAP, 1-bit text) stay as at quality 0.

### Small sizes: hypotheses tested and rejected

1-bit at heights 8–10 is the weakest area (Tahoma 400 at 8–10:
143–149 of 261). None of these helped:

| hypothesis | effect | run |
|---|---|---|
| Unhinted loading where gasp lacks GASP_GRIDFIT (Tahoma, Verdana and Arial have 0x000A at ppem ≤ 8), for 1-bit | 70.6% → 44.7% at heights 8–10 | 40465569113 |
| The same, for greyscale | 71.5% → 56.0% (sampled) | 40465569113 |
| Forced dropout modes for 1-bit (smart ± stubs, simple ± stubs) | 70.6% → 44.8–50.8% at 8–10; 90.6% → 83.5–84.9% overall | 40467000659 |
| 1-bit by plain centre sampling (no dropout control) | 70.6% → 40.3% at 8–10 | 40466309200 |
| v40 interpreter, all ppems or only below ppem 9 or 11 | 1-bit unchanged at 8–10, greyscale worse everywhere | 40467469320 |

best_ppem.py confirms the ppem is right at these sizes. The remaining
differences are most likely in the hinting itself; see open items.

## Measured match rates

Run 40467896104, gdifont against Windows build 26100, per face and weight.
tm is tmHeight, tmAscent, tmDescent and tmMaxCharWidth together, over 17
sizes. 1-bit is GGO_BITMAP (the same at both qualities).

| face | weight | tm | ABC q0 | ABC q1 | 1-bit | ExtTextOut q0 | ExtTextOut q1 | GGO_GRAY8 |
|---|---|---|---|---|---|---|---|---|
| Tahoma | 400 | 17/17 | 100.0% | 98.8% | 89.2% | 89.2% | 90.4% | 76.6% |
| Tahoma | 700 | 17/17 | 99.9% | 99.2% | 92.2% | 92.2% | 89.2% | 75.6% |
| Arial | 400 | 17/17 | 98.4% | 96.8% | 94.8% | 94.8% | 89.1% | 75.6% |
| Arial | 700 | 17/17 | 98.1% | 96.7% | 96.7% | 96.8% | 88.4% | 80.3% |
| Courier New | 400 | 17/17 | 99.7% | 99.3% | 95.2% | 95.2% | 89.6% | 64.3% |
| Courier New | 700 | 17/17 | 99.9% | 98.4% | 90.1% | 90.5% | 85.8% | 70.7% |
| Times New Roman | 400 | 17/17 | 98.2% | 97.0% | 88.7% | 88.8% | 84.6% | 73.1% |
| Times New Roman | 700 | 17/17 | 97.7% | 96.0% | 87.0% | 87.1% | 82.8% | 73.4% |
| Trebuchet MS | 400 | 17/17 | 99.3% | 98.8% | 85.5% | 85.7% | 92.4% | 83.8% |
| Trebuchet MS | 700 | 17/17 | 99.1% | 98.9% | 87.8% | 88.1% | 89.5% | 82.1% |
| Verdana | 400 | 17/17 | 99.5% | 99.0% | 86.5% | 86.5% | 89.4% | 80.1% |
| Verdana | 700 | 17/17 | 99.9% | 98.4% | 93.3% | 93.3% | 88.6% | 76.8% |

All faces together:

| measure | ftref.py (run 40462351632) | gdifont, run 40464365435 | gdifont, run 40467896104 |
|---|---|---|---|
| ABC q0 | 91.6–100% per face | 99.1% | 99.1% |
| ABC q1 | – | 88.3% | 98.1% |
| 1-bit | 79.0–94.1% per face | 90.6% | 90.6% |
| ExtTextOut q0 | 69.5–88.8% per face | 90.7% | 90.7% |
| ExtTextOut q1 | 23.9–66.1% per face | 48.5% | 88.3% |
| GGO_GRAY8 | 2–22% per face | 12.3% | 76.0% |
| size choice | 193/204 | 204/204 | 204/204 |

## Open items

1. **Small sizes (heights 8–10).** 1-bit is 52–94% there depending on the
   face, and every hypothesis in the table above failed.
   - The best_ppem and diagnose numbers show the ppem is right. The glyph
     shapes differ.
   - That suggests the font programs behave differently under Windows'
     interpreter than under FreeType's v35, for example in what GETINFO
     or MPS report. FreeType does not expose either of those as a
     setting. [U]
2. **Remaining greyscale misses.** Most are ±1 counts. They cluster in
   glyphs whose 1-bit glyph also differs (about 35% gray8-identical there,
   against about 80% where 1-bit is identical, run 40467000659). That
   points at sub-pixel differences in the hinted outline, not at the
   sampler.
3. **Greyscale text levels at other contrast settings:** only contrast
   1200 was measured.
4. **Remaining 1-bit misses at ordinary sizes:** mostly "same box size,
   other pixels", single pixels. [U]
5. **Not covered by the capture:** underline and strikeout geometry,
   synthetic bold and italic, fonts with `hdmx`/`LTSH` or embedded bitmaps,
   and TrueType collections (gdidump opens face 0).
6. **otm fields:** the `GetOutlineTextMetrics` fields that gdiref records
   are not produced by gdidump. compare.py doesn't use them.
