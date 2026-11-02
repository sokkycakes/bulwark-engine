#!/usr/bin/env python3
# Bulwark engine - draws skelreduce's -poses output: the full skeleton in
# grey, the cut-down one in orange, a few frames of each chosen clip in a
# row. ISC licence, see LICENSE.
#
#   skelplot.py <poses file> <out.png> <clip>[,<clip>...] [frames a row] [title]
import math
import sys

from PIL import Image, ImageDraw


def load(path):
    frames = {}
    cur = None
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "F":
                cur = []
                frames.setdefault(p[1], []).append((int(p[2]), cur))
            elif p[0] == "J":
                cur.append((int(p[2]), int(p[3]), [float(v) for v in p[4:7]], [float(v) for v in p[7:10]]))
    return frames


def main():
    frames = load(sys.argv[1])
    clips = sys.argv[3].split(",")
    per = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    title = sys.argv[5] if len(sys.argv) > 5 else ""
    cell, top = 220, 22
    img = Image.new("RGB", (cell * per, top + (cell + 18) * len(clips)), (24, 24, 30))
    d = ImageDraw.Draw(img)
    d.text((6, 4), title + "   grey: full skeleton   orange: cut down", fill=(220, 220, 220))
    yaw = math.radians(40)
    for row, clip in enumerate(clips):
        fr = frames.get(clip, [])
        if not fr:
            continue
        pick = [fr[min(len(fr) - 1, round(i * (len(fr) - 1) / max(1, per - 1)))] for i in range(per)]
        # one scale for the row, from every joint of the frames shown
        pts = []
        for _, js in pick:
            for _, _, a, b in js:
                pts += [a, b]

        def proj(p):
            x = p[0] * math.cos(yaw) - p[1] * math.sin(yaw)
            return x, -p[2]

        xs = [proj(p)[0] for p in pts]
        ys = [proj(p)[1] for p in pts]
        span = max(max(xs) - min(xs), max(ys) - min(ys), 1)
        s = (cell - 24) / span
        y0 = top + row * (cell + 18)
        d.text((6, y0), clip, fill=(200, 200, 200))
        for col, (num, js) in enumerate(pick):
            ox = col * cell + 12 - min(xs) * s + (cell - 24 - (max(xs) - min(xs)) * s) / 2
            oy = y0 + 16 - min(ys) * s

            def sp(p):
                x, y = proj(p)
                return (ox + x * s, oy + y * s)

            for which, colour, width in ((2, (130, 130, 140), 3), (3, (255, 150, 40), 2)):
                for parent, kept, a, b in js:
                    if parent < 0:
                        continue
                    d.line([sp(js[parent][which]), sp((a, b)[which - 2])], fill=colour, width=width)
            d.text((col * cell + 6, y0 + cell), "frame %d" % num, fill=(150, 150, 150))
    img.save(sys.argv[2])


main()
