# Draws the small glyph images the Qt look-alike themes use (check marks,
# arrows, grips) into data/panorama/images/qt/. Our own drawings, sized and
# coloured from measurements of Qt screenshots (see gallery.py); no Qt art.
#   python3 tools/qtref/glyphs.py data/panorama/images/qt
import os, sys
from PIL import Image, ImageDraw

SS = 8  # supersampling
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def rgba(hexs, a=255):
    hexs = hexs.lstrip('#')
    return tuple(int(hexs[i:i + 2], 16) for i in (0, 2, 4)) + (a,)

def canvas(w, h):
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)

def save(im, w, h, name):
    im.resize((w, h), Image.LANCZOS).save(os.path.join(out, name))

def S(pts):
    return [(x * SS, y * SS) for x, y in pts]

def triangle(name, w, h, box, direction, color):
    # box = (x0, y0, x1, y1) of the triangle inside a w x h canvas
    im, d = canvas(w, h)
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    pts = {'down': [(x0, y0), (x1, y0), (cx, y1)], 'up': [(x0, y1), (x1, y1), (cx, y0)],
           'right': [(x0, y0), (x0, y1), (x1, cy)], 'left': [(x1, y0), (x1, y1), (x0, cy)]}[direction]
    d.polygon(S(pts), fill=rgba(color))
    save(im, w, h, name)

def chevron(name, w, h, box, direction, color, width):
    im, d = canvas(w, h)
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    pts = {'down': [(x0, y0), (cx, y1), (x1, y0)], 'up': [(x0, y1), (cx, y0), (x1, y1)],
           'right': [(x0, y0), (x1, cy), (x0, y1)], 'left': [(x1, y0), (x0, cy), (x1, y1)]}[direction]
    d.line(S(pts), fill=rgba(color), width=int(width * SS), joint='curve')
    save(im, w, h, name)

def check(name, size, pts, color, width):
    im, d = canvas(size, size)
    d.line(S(pts), fill=rgba(color), width=int(width * SS), joint='curve')
    save(im, size, size, name)

def grip(name, w, h, vertical_lines, color, count=3, gap=3):
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    span = (count - 1) * gap
    for i in range(count):
        if vertical_lines:
            x = (w - 1 - span) // 2 + i * gap
            d.line([(x, 0), (x, h - 1)], fill=rgba(color))
        else:
            y = (h - 1 - span) // 2 + i * gap
            d.line([(0, y), (w - 1, y)], fill=rgba(color))
    im.save(os.path.join(out, name))

# Fusion: filled triangles; check mark drawn with the text colour.
fusion_check = [(3.2, 7.0), (5.8, 10.0), (10.8, 3.6)]
check('fusion-check.png', 14, fusion_check, '#000000', 2.0)
check('fusion-check-disabled.png', 14, fusion_check, '#bebebe', 2.0)
triangle('fusion-combo-arrow.png', 22, 8, (7.5, 2, 14.5, 6), 'down', '#5b5b5b')
for dname in ('up', 'down'):
    triangle('fusion-scroll-%s.png' % dname, 16, 16, (4.5, 6, 11.5, 10), dname, '#5c5c5c')
for dname in ('left', 'right'):
    triangle('fusion-scroll-%s.png' % dname, 16, 16, (6, 4.5, 10, 11.5), dname, '#5c5c5c')

# QtCurve: chevrons, a heavier check, three-line grips.
qtc_check = [(3.0, 6.8), (5.6, 9.6), (10.6, 3.4)]
check('qtcurve-check.png', 14, qtc_check, '#000000', 2.4)
check('qtcurve-check-disabled.png', 14, qtc_check, '#a8a9aa', 2.4)
chevron('qtcurve-combo-arrow.png', 22, 8, (8, 2, 14, 5.5), 'down', '#232629', 1.6)
for dname in ('up', 'down'):
    chevron('qtcurve-scroll-%s.png' % dname, 14, 14, (4.2, 5.5, 9.8, 8.5), dname, '#232629', 1.6)
for dname in ('left', 'right'):
    chevron('qtcurve-scroll-%s.png' % dname, 14, 14, (5.5, 4.2, 8.5, 9.8), dname, '#232629', 1.6)
grip('qtcurve-grip-v.png', 7, 5, True, '#b3b3b3')
grip('qtcurve-grip-h.png', 5, 7, False, '#b3b3b3')

def dot(name, size, diameter, color):
    im, d = canvas(size, size)
    r = diameter / 2
    c = size / 2
    d.ellipse(S([(c - r, c - r), (c + r, c + r)]), fill=rgba(color))
    save(im, size, size, name)

dot('fusion-radio-dot.png', 15, 6.5, '#2e2e2e')
dot('fusion-radio-dot-disabled.png', 15, 6.5, '#bebebe')
dot('qtcurve-radio-dot.png', 15, 5.5, '#000000')
dot('qtcurve-radio-dot-disabled.png', 15, 5.5, '#a8a9aa')

def groove(name, vertical, thickness, offset, colors):
    # a straight groove line `colors` (outer to outer) across a thickness-px bar
    n = len(colors)
    w, h = (thickness, 4) if vertical else (4, thickness)
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for i, c in enumerate(colors):
        for j in range(4):
            im.putpixel((offset + i, j) if vertical else (j, offset + i), rgba(c))
    im.save(os.path.join(out, name))

qtc_groove = ['#e2e3e4', '#b3b3b3', '#d1d1d1', '#d7d7d7', '#dddddd', '#b3b3b3', '#fefefe']
groove('qtcurve-groove-v.png', True, 15, 4, qtc_groove)
groove('qtcurve-groove-h.png', False, 15, 4, qtc_groove)
