#!/usr/bin/env python3
"""Lay out a line of text from each side's per-glyph ExtTextOut captures
(pen advanced by A+B+C, as ExtTextOut does) and print both as text art,
with each glyph's ABC, so the spacing of a face at one size can be read
straight from the CI log.

usage: textline.py <windows_dir> <candidate_dir> <face> <tall> <weight> <aa> <text>
"""
import base64, json, os, sys

def load(d, face, tall, weight, aa):
    gl, cfg = {}, None
    for line in open(os.path.join(d, "configs.jsonl"), encoding="utf-8"):
        c = json.loads(line)
        if (c["face"], c["tall"], c["weight"], c["aa"]) == (face, tall, weight, aa):
            cfg = c
    for line in open(os.path.join(d, "glyphs.jsonl"), encoding="utf-8"):
        g = json.loads(line)
        if (g["face"], g["tall"], g["weight"], g["aa"]) == (face, tall, weight, aa):
            gl[g["cp"]] = g
    return cfg, gl

def layout(gl, text):
    ink, pen = {}, 0
    for ch in text:
        g = gl.get(ord(ch))
        if not g:
            continue
        e = g.get("eto", {})
        if "rgb" in e:
            raw = base64.b64decode(e["rgb"])
            for j in range(e["h"]):
                for i in range(e["w"]):
                    v = raw[(j * e["w"] + i) * 3]
                    if v:
                        k = (pen + g["a"] + e["x"] + i, e["y"] + j)
                        ink[k] = max(ink.get(k, 0), v)
        pen += g["a"] + g["b"] + g["c"]
    return ink, pen

def art(ink, width):
    if not ink:
        return []
    y0 = min(y for _, y in ink)
    y1 = max(y for _, y in ink)
    ramp = " .:-=+*#%@"
    return ["".join(ramp[min(9, ink.get((x, y), 0) * 10 // 256)] for x in range(width)) for y in range(y0, y1 + 1)]

def main():
    wdir, cdir, face, tall, weight, aa, text = sys.argv[1:8]
    tall, weight, aa = int(tall), int(weight), int(aa)
    sides = [("windows", *load(wdir, face, tall, weight, aa)), ("gdifont", *load(cdir, face, tall, weight, aa))]
    for name, cfg, _ in sides:
        if cfg:
            print("%s: ppem %s height %s ascent %s internal leading %s" % (
                name, cfg.get("ppem", "-"), cfg.get("tmHeight"), cfg.get("tmAscent"), cfg.get("tmInternalLeading")))
        else:
            print("%s: no config %s %d %d %d" % (name, face, tall, weight, aa))
    print("char  windows A/B/C   gdifont A/B/C")
    for ch in dict.fromkeys(text):
        cells = []
        for _, _, gl in sides:
            g = gl.get(ord(ch))
            cells.append("%3d %3d %3d" % (g["a"], g["b"], g["c"]) if g else "    -      ")
        mark = "" if cells[0] == cells[1] else "  <"
        print(" %r  %s     %s%s" % (ch, cells[0], cells[1], mark))
    lines = [layout(gl, text) for _, _, gl in sides]
    width = max(p for _, p in lines) + 2
    for (name, _, _), (ink, pen) in zip(sides, lines):
        print("--- %s (advance %d)" % (name, pen))
        for row in art(ink, width):
            print("|" + row + "|")

if __name__ == "__main__":
    main()
