#!/usr/bin/env python3
"""Bulwark engine - bake a pixel font for the DS's HUD. ISC licence, see LICENSE.

    dsfont.py font.ttf|.bdf|.pcf size out.dsf

Renders printable ASCII (32-126) one bit deep at `size` pixels per em (a
pixel TrueType font's own size: 8 for 04b09, see docs/spec/bitmap-fonts.md)
and writes the DS's .dsf format, which the link client draws its top-screen
HUD with (link_nds.c, hud_*):

    "DSF1", u8 height, u8 first (32), u8 count, u8 reserved,
    count x { u8 advance, u8 width, height x u8 row (bit 0 = the left pixel) }

Glyphs are cut to 8 pixels wide. The fonts sokky uses are mostly not free to
redistribute, so a baked .dsf is game data: keep it out of git (the
.gitignore has *.dsf) and ship it in a private package.
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    path, size, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    font = ImageFont.truetype(path, size) if path.lower().endswith((".ttf", ".otf")) else ImageFont.load(path)
    ascent, descent = font.getmetrics()
    height = ascent + descent
    first, count = 32, 95
    data = bytearray(b"DSF1" + bytes([height, first, count, 0]))
    for c in range(first, first + count):
        ch = chr(c)
        adv = int(round(font.getlength(ch)))
        img = Image.new("1", (16, height), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((0, 0), ch, font=font, fill=1)
        bbox = img.getbbox()
        width = min(8, bbox[2] if bbox else 0)
        data += bytes([min(adv, 255), width])
        for y in range(height):
            bits = 0
            for x in range(width):
                if img.getpixel((x, y)):
                    bits |= 1 << x
            data.append(bits)
    open(out, "wb").write(data)
    print(f"{out}: {count} glyphs, {height} rows, {len(data)} bytes")


if __name__ == "__main__":
    main()
