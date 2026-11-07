#!/usr/bin/env python3
"""Bulwark engine - bake a pixel font for the DS's HUD. ISC licence, see LICENSE.

    dsfont.py font.ttf|.bdf|.pcf size out.dsf|out.h

Renders printable ASCII (32-126) one bit deep at `size` pixels per em (a
pixel TrueType font's own size: 8 for 04b09, see docs/spec/bitmap-fonts.md)
and writes the DS's .dsf format, which the link client draws its top-screen
HUD with (link_nds.c, hud_*):

    "DSF1", u8 height, u8 first (32), u8 count, u8 reserved,
    count x { u8 advance, u8 width, height x u8 row (bit 0 = the left pixel) }

Glyphs are cut to 8 pixels wide. The fonts sokky uses are mostly not free to
redistribute, so a baked .dsf is game data: keep it out of git (the
.gitignore has *.dsf) and ship it in a private package.

An out.h gets the same bytes as a C array (hudnum_dsf[]), for a font free to
ship that is built into the ROM: src/platform/nds/hudnum_dsf.h is
TallPixelFont (SIL OFL 1.1, data/fonts/TallPixelFont-OFL.txt) at 9.
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def load_font(path, size):
    low = path.lower()
    if low.endswith((".ttf", ".otf")):
        return ImageFont.truetype(path, size)
    if low.endswith((".bdf", ".pcf", ".pcf.gz")):  # PIL reads these only through its own .pil/.pbm pair
        import gzip, os, tempfile
        from PIL import BdfFontFile, PcfFontFile
        with (gzip.open if low.endswith(".gz") else open)(path, "rb") as f:
            ff = BdfFontFile.BdfFontFile(f) if low.endswith(".bdf") else PcfFontFile.PcfFontFile(f)
        tmp = tempfile.mkdtemp()
        ff.save(os.path.join(tmp, "font"))
        return ImageFont.load(os.path.join(tmp, "font.pil"))
    return ImageFont.load(path)  # a .pil


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    path, size, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    font = load_font(path, size)
    if hasattr(font, "getmetrics"):  # TrueType
        ascent, descent = font.getmetrics()
        height = ascent + descent
        length = font.getlength
    else:  # a BDF/PCF strike (PIL's bitmap ImageFont has neither): its line box and advances
        height = max(font.getbbox(chr(c))[3] for c in range(32, 127))
        length = lambda ch: font.getbbox(ch)[2]
    first, count = 32, 95
    data = bytearray(b"DSF1" + bytes([height, first, count, 0]))
    for c in range(first, first + count):
        ch = chr(c)
        adv = int(round(length(ch)))
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
    if out.endswith(".h"):
        name = sys.argv[1].replace("\\", "/").split("/")[-1]
        rows = [", ".join("0x%02x" % b for b in data[i:i + 16]) for i in range(0, len(data), 16)]
        open(out, "w").write(
            f"/* {name} at {size} px, baked by tools/nds/dsfont.py (the .dsf format is described there).\n"
            f" * Generated: do not edit. */\n"
            f"static const uint8_t hudnum_dsf[{len(data)}] = {{\n\t" + ",\n\t".join(rows) + ",\n};\n")
    else:
        open(out, "wb").write(data)
    print(f"{out}: {count} glyphs, {height} rows, {len(data)} bytes")


if __name__ == "__main__":
    main()
