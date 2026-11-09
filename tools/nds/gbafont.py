#!/usr/bin/env python3
"""Bulwark engine - cut a bitmap font out of a GBA ROM for the DS's HUD. ISC licence, see LICENSE.

    gbafont.py ROM --offset HEX --bpp 1|2|4 --glyph WxH --count N
               (--first C | --map STRING) [--lz77] [--msb] [--layout tiles|rows]
               [--sheet-width TILES] [--stride BYTES] [--ink 1,2,...]
               [--widths HEX] [--fixed ADV] [--spacing 1] [--crop-left]
               [--parts REL:N,...] [--rows Y0:Y1] [--merge IN.dsf]
               [--text STRING [--split columns|parts]]
               [--space-advance N] [--fold-case] [--dsf OUT.dsf] [--preview OUT.png]

Reads `count` glyphs at `offset` (or, with --lz77, from the GBA BIOS LZ77
block, header 0x10, that starts there) and writes the DS's .dsf (the format is
described in dsfont.py) and/or a preview PNG (the glyphs in their own colours,
then as the .dsf holds them, 1 bit, 4x).

Layout of the glyphs:
  tiles  (default) 8x8 tiles, 4bpp the GBA's (low nibble = the left pixel),
         2bpp/1bpp packed the same way (low bits = the left pixel; --msb flips
         1bpp to bit 7 = the left pixel). A glyph of WxH pixels is
         ceil(W/8) x ceil(H/8) tiles: consecutive in row-major order (1D, an
         OBJ sheet in 1D mapping), or with --sheet-width T in a sheet T tiles
         wide (2D: glyph g sits at column g*tw, its tile rows a sheet row
         apart, wrapping to the next band of rows; the offset may point into
         the middle of a row).
  rows   each glyph H rows of ceil(W*bpp/8) bytes, same bit order.
--stride sets the bytes from one glyph to the next (default: its own size).
--parts REL:N,REL:N,... reads the glyphs in runs, N glyphs from each hex
offset REL past --offset (a font split across a sheet); they add up to --count.
--rows Y0:Y1 keeps only those rows of every glyph (cells taller than the ink).

Pre-rendered text (a game that ships its text as pictures): --text STRING lays
the glyphs read side by side as one line, cuts it into letters (with --split
columns, the default, at blank columns; with --split parts, into the ink's
8-connected parts, for kerned text whose letters share columns; either way,
parts sharing most of their columns, as an i and its dot, are one) and names
them by the characters of STRING (spaces skipped; the counts must match, else
the tool prints where the letters it found are). A space's advance is measured
from the gaps between words. --merge IN.dsf starts from a .dsf already made
(the same height), so several lines can build up one font; --count 0 reads all
the data.

Characters: --first C gives glyph i the code C+i (C a number or one character);
--map gives glyph i the i-th character of STRING ('\\u00b7' or any character
outside 32-126 skips that glyph; Python escapes are understood).

Ink: a pixel is ink when its colour index is in --ink (default: any non-zero),
so the fill of an outlined or shadowed font can be picked without its edge.

Width: from a width table (--widths: one byte per glyph, at that ROM offset),
else --fixed ADV (every glyph that advance, its width the ink's right edge),
else the ink's right edge + --spacing (proportional). --crop-left first moves
each glyph's ink to column 0. The .dsf holds at most 8 columns and 16 rows:
wider or taller glyphs are cut and a warning names them; the preview still
shows them whole. A character inside the .dsf's range that the font lacks gets
its '?' (the DS draws a character outside the range as '?'); --fold-case lets
capitals stand in for missing small letters and the other way. A space with no
glyph gets --space-advance (default: the advance of 'n' or of the narrowest
digit).

The tool holds no game data. What it writes from a ROM is that game's art: keep
the .dsf and the previews out of git (.gitignore has *.dsf).
"""
import argparse
import sys
from PIL import Image


def lz77(d, off):
    if d[off] != 0x10:
        sys.exit(f"no LZ77 header (0x10) at {off:#x}")
    size = d[off + 1] | d[off + 2] << 8 | d[off + 3] << 16
    out, p = bytearray(), off + 4
    while len(out) < size:
        flags = d[p]
        p += 1
        for b in range(8):
            if len(out) >= size:
                break
            if flags & 0x80 >> b:
                x = d[p] << 8 | d[p + 1]
                p += 2
                n, disp = (x >> 12) + 3, (x & 0xFFF) + 1
                if disp > len(out):
                    sys.exit(f"bad LZ77 data at {p - 2:#x}")
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out[:size])


def unpack(byte_seq, bpp, n, msb):
    """n pixels from packed bytes, the first pixel in the low bits (msb: the high)"""
    px, per, mask = [], 8 // bpp, (1 << bpp) - 1
    for b in byte_seq:
        for k in range(per):
            sh = (per - 1 - k) * bpp if msb else k * bpp
            px.append(b >> sh & mask)
    return px[:n]


def glyph_pixels(d, a, g):
    """glyph g as rows of colour indexes"""
    for start, n in a.parts:
        if g < n:
            a.base = start
            break
        g -= n
    w, h, bpp = a.gw, a.gh, a.bpp
    if a.layout == "rows":
        rb = (w * bpp + 7) // 8
        base = a.base + g * (a.stride or rb * h)
        return [unpack(d[base + y * rb:base + (y + 1) * rb], bpp, w, a.msb) for y in range(h)]
    tw, th, tb = (w + 7) // 8, (h + 7) // 8, 8 * bpp
    rows = [[0] * (tw * 8) for _ in range(th * 8)]
    for ty in range(th):
        for tx in range(tw):
            if a.sheet_width:
                col, band = (g * tw) % a.sheet_width, (g * tw) // a.sheet_width
                t = (band * th + ty) * a.sheet_width + col + tx
                addr = a.base + t * tb
            else:
                addr = a.base + g * (a.stride or tw * th * tb) + (ty * tw + tx) * tb
            tile = d[addr:addr + tb]
            for y in range(8):
                r = unpack(tile[y * bpp:(y + 1) * bpp], bpp, 8, a.msb)
                rows[ty * 8 + y][tx * 8:tx * 8 + 8] = r + [0] * (8 - len(r))
    return [r[:w] for r in rows[:h]]


def split_text(a, native, ink):
    """--text: the glyphs read are pieces of a line of pre-rendered text, left to
    right; cut it into its letters (the ink's connected parts, those that share
    most of their columns joined, as an i's dot) and name them by the text's
    characters"""
    strip = [sum((px[y] for px in native), []) for y in range(a.gh)]
    H, W = a.gh, len(strip[0])
    on = [[bool(v in ink if ink else v) for v in row] for row in strip]
    seen, comps = set(), []
    if a.split == "columns":  # runs of columns with ink, cut at blank columns
        x = 0
        while x < W:
            if any(on[y][x] for y in range(H)):
                x0 = x
                while x < W and any(on[y][x] for y in range(H)):
                    x += 1
                comps.append([(i, y) for i in range(x0, x) for y in range(H) if on[y][i]])
            else:
                x += 1
    for x in range(W if a.split == "parts" else 0):
        for y in range(H):
            if on[y][x] and (x, y) not in seen:
                todo, pix = [(x, y)], []
                seen.add((x, y))
                while todo:
                    px, py = todo.pop()
                    pix.append((px, py))
                    for nx, ny in ((px + i, py + j) for i in (-1, 0, 1) for j in (-1, 0, 1)):
                        if 0 <= nx < W and 0 <= ny < H and on[ny][nx] and (nx, ny) not in seen:
                            seen.add((nx, ny))
                            todo.append((nx, ny))
                comps.append(pix)
    groups = []  # [x0, x1, pixels]
    for pix in sorted(comps, key=lambda p: min(x for x, _ in p)):
        x0, x1 = min(x for x, _ in pix), max(x for x, _ in pix) + 1
        if groups:
            g = groups[-1]
            over = min(x1, g[1]) - max(x0, g[0])
            if over * 2 >= min(x1 - x0, g[1] - g[0]):
                g[0], g[1] = min(g[0], x0), max(g[1], x1)
                g[2] += pix
                continue
        groups.append([x0, x1, pix])
    chars = [c for c in a.text if c != " "]
    if len(groups) != len(chars):
        sys.exit(f"--text has {len(chars)} characters but the strip has {len(groups)} letters at columns "
                 + " ".join(f"{g[0]}-{g[1]}" for g in groups))
    gaps, k = [], 0  # a space's advance: the gaps between words, less the gap between letters
    for i, c in enumerate(a.text.rstrip()):
        if c == " " and i and a.text[i - 1] != " " and 0 < k < len(groups):
            gaps.append(groups[k][0] - groups[k - 1][1])
        elif c != " ":
            k += 1
    space = max(1, sorted(gaps)[len(gaps) // 2] - a.spacing) if gaps else None
    a.gw = max(g[1] - g[0] for g in groups)
    glyphs = []
    for x0, x1, pix in groups:
        mine = set(pix)
        glyphs.append([[strip[y][x] if x < x1 and (not on[y][x] or (x, y) in mine) else 0
                        for x in range(x0, x0 + a.gw)] for y in range(H)])
    return [ord(c) for c in chars], glyphs, space


def load_dsf(path, h):
    f = open(path, "rb").read()
    if f[:4] != b"DSF1" or f[4] != h:
        sys.exit(f"{path}: not a .dsf of {h} rows")
    first, count, out = f[5], f[6], {}
    for i in range(count):
        g = f[8 + i * (2 + h):8 + (i + 1) * (2 + h)]
        if g[0] or g[1]:
            out[first + i] = (g[0], g[1], [[r >> x & 1 for x in range(8)] for r in g[2:]])
    q = out.get(63)
    out = {c: g for c, g in out.items() if c == 63 or g != q}  # not the '?'s standing in for a missing one
    if 32 in range(first, first + count) and 32 not in out:
        out[32] = (f[8 + (32 - first) * (2 + h)], 0, [[0] * 8 for _ in range(h)])
    return out


def parse_args(argv):
    p = argparse.ArgumentParser(description="cut a GBA ROM's bitmap font into a DS .dsf")
    p.add_argument("rom")
    p.add_argument("--offset", required=True, type=lambda s: int(s, 16))
    p.add_argument("--bpp", type=int, choices=(1, 2, 4), required=True)
    p.add_argument("--glyph", required=True, help="WxH in pixels")
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--first")
    p.add_argument("--map")
    p.add_argument("--lz77", action="store_true")
    p.add_argument("--msb", action="store_true")
    p.add_argument("--layout", choices=("tiles", "rows"), default="tiles")
    p.add_argument("--parts")
    p.add_argument("--sheet-width", type=int, default=0)
    p.add_argument("--stride", type=int, default=0)
    p.add_argument("--ink")
    p.add_argument("--rows")
    p.add_argument("--widths", type=lambda s: int(s, 16))
    p.add_argument("--fixed", type=int)
    p.add_argument("--spacing", type=int, default=1)
    p.add_argument("--crop-left", action="store_true")
    p.add_argument("--space-advance", type=int)
    p.add_argument("--fold-case", action="store_true")
    p.add_argument("--dsf")
    p.add_argument("--preview")
    p.add_argument("--text")
    p.add_argument("--merge")
    p.add_argument("--split", choices=("columns", "parts"), default="columns")
    a = p.parse_args(argv)
    a.gw, a.gh = (int(v) for v in a.glyph.lower().split("x"))
    a.parts = [tuple(int(v, 16) if i == 0 else int(v) for i, v in enumerate(pt.split(":")))
               for pt in a.parts.split(",")] if a.parts else [(0, a.count)]
    if sum(n for _, n in a.parts) != a.count:
        p.error("--parts must add up to --count glyphs")
    if a.text is not None:
        a.first = a.first or "0"
    elif (a.first is None) == (a.map is None):
        p.error("give one of --first and --map (or --text)")
    return a


def main(argv=None):
    a = parse_args(argv)
    rom = open(a.rom, "rb").read()
    d = lz77(rom, a.offset) if a.lz77 else rom[a.offset:]
    if a.count == 0:  # as many glyphs as the data holds
        per = a.stride or ((a.gw + 7) // 8) * ((a.gh + 7) // 8) * 8 * a.bpp
        a.count = len(d) // per
        a.parts = [(0, a.count)]
    ink = {int(v) for v in a.ink.split(",")} if a.ink else None
    if a.map is not None:
        chars = a.map.encode("latin-1", "backslashreplace").decode("unicode_escape")
        codes = [ord(c) for c in chars[:a.count]]
    else:
        c0 = ord(a.first) if len(a.first) == 1 and not a.first.isdigit() else int(a.first, 0)
        codes = [c0 + i for i in range(a.count)]
    native = [glyph_pixels(d, a, g) for g in range(len(codes))]
    if a.rows:
        y0, y1 = (int(v) for v in a.rows.split(":"))
        native = [px[y0:y1] for px in native]
        a.gh = y1 - y0
    tab = rom[a.widths:a.widths + len(codes)] if a.widths is not None else None
    space = None
    if a.text is not None:
        codes, native, space = split_text(a, native, ink)

    glyphs = load_dsf(a.merge, a.gh) if a.merge else {}  # code -> (advance, width, rows of bits)
    if space is not None and 32 not in glyphs:
        glyphs[32] = (space, 0, [[0] * a.gw for _ in range(a.gh)])
    for g, (code, px) in enumerate(zip(codes, native)):
        bits = [[1 if (v in ink if ink else v) else 0 for v in row] for row in px]
        if code in glyphs and a.text is not None and code != 32:
            old = [r[:8] + [0] * (8 - len(r[:8])) for r in glyphs[code][2]]
            if old != [r[:8] + [0] * (8 - len(r[:8])) for r in bits]:
                print(f"warning: two different glyphs for {chr(code)!r}; keeping the first", file=sys.stderr)
        if not 32 <= code <= 126 or code in glyphs:
            continue
        if a.crop_left:
            left = min((r.index(1) for r in bits if 1 in r), default=0)
            bits = [r[left:] + [0] * left for r in bits]
        right = max((len(r) - r[::-1].index(1) for r in bits if 1 in r), default=0)
        if tab is not None:
            adv = tab[g]
        elif a.fixed:
            adv = a.fixed
        else:
            adv = right + a.spacing if right else 0
        glyphs[code] = (adv, right, bits)

    if 32 not in glyphs:
        ref = glyphs.get(ord("n")) or min((glyphs[c] for c in range(48, 58) if c in glyphs),
                                          key=lambda t: t[0], default=(4, 0, None))
        sp = a.space_advance if a.space_advance is not None else (a.fixed or ref[0])
        glyphs[32] = (sp, 0, [[0] * a.gw for _ in range(a.gh)])
    elif a.space_advance is not None:
        glyphs[32] = (a.space_advance,) + glyphs[32][1:]

    if a.fold_case:  # a font of one case stands in for the other
        for c in range(26):
            up, low = 65 + c, 97 + c
            if up in glyphs and low not in glyphs:
                glyphs[low] = glyphs[up]
            elif low in glyphs and up not in glyphs:
                glyphs[up] = glyphs[low]

    h = min(a.gh, 16)
    wide = [chr(c) for c, (_, w, _) in glyphs.items() if w > 8]
    if wide:
        print(f"warning: {len(wide)} glyphs wider than 8 px are cut to 8 in the .dsf: {''.join(wide)}",
              file=sys.stderr)
    if a.gh > 16:
        print(f"warning: glyphs are {a.gh} rows; the .dsf keeps the first 16", file=sys.stderr)

    if a.dsf:
        first, last = min(glyphs), max(glyphs)
        count = last - first + 1
        out = bytearray(b"DSF1" + bytes([h, first, count, 0]))
        for c in range(first, last + 1):
            # a character the font lacks draws as its '?', as the DS draws one outside the range
            adv, w, bits = glyphs.get(c, glyphs.get(63, (0, 0, None)))
            out += bytes([min(adv, 255), min(w, 8)])
            for y in range(h):
                v = 0
                for x in range(min(w, 8)):
                    v |= bits[y][x] << x
                out.append(v)
        open(a.dsf, "wb").write(out)
        have = "".join(chr(c) for c in range(33, 127) if c in glyphs)
        print(f"{a.dsf}: {len(glyphs)} glyphs ({first}-{last}), {h} rows, {len(out)} bytes; has {have}")

    if a.preview:
        S, cols = 4, 16
        cw, ch = a.gw + 2, a.gh + 2
        rows = (len(native) + cols - 1) // cols
        mx = (1 << a.bpp) - 1
        im = Image.new("RGB", (cols * cw * S, rows * ch * S * 2 + S), (60, 0, 60))
        for g, px in enumerate(native):
            ox, oy = (g % cols) * cw + 1, (g // cols) * ch + 1
            for y, row in enumerate(px):
                for x, v in enumerate(row):
                    c = 30 if v == 0 else 90 + v * 165 // mx
                    im.paste((c, c, c), ((ox + x) * S, (oy + y) * S, (ox + x + 1) * S, (oy + y + 1) * S))
            code = codes[g]
            if code in glyphs:
                adv, w, bits = glyphs[code]
                oy2 = oy + rows * ch
                for y in range(a.gh):
                    for x in range(a.gw):
                        on = bits[y][x] if x < len(bits[y]) else 0
                        col = (255, 255, 255) if on else ((20, 20, 70) if x < adv else (20, 20, 20))
                        if on and (x >= 8 or y >= 16):
                            col = (255, 80, 80)  # cut from the .dsf
                        im.paste(col, ((ox + x) * S, (oy2 + y) * S + S, (ox + x + 1) * S, (oy2 + y + 1) * S + S))
        im.save(a.preview)


if __name__ == "__main__":
    main()
