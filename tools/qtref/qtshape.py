#!/usr/bin/env python3
# Paints an arbitrary outline (SVG paths) as a QtCurve-themed window, for
# Panorama panels whose shape CSS can't express (a slanted tab, say).
#
#   qtshape.py shape.svg out.png --tile window.png --border #282828 --light #3d3d3d
#              [--scale 0.5] [--x0 99]
#
# --scale sizes the outline for a layout authored at another resolution
# (0.5 for a 540-high layout from a 1080p design); the tile and the frame
# lines stay one output pixel each, as QtCurve draws them at any size.
# --x0 drops the part of the shape left of that x (off screen).
#
# The inside is the theme's window background (the tile qtcurve2panorama.py
# measured, e.g. images/qt/<variant>/window.png), and the outline gets the
# frame edges QtCurve draws around a window pane: a dark border line and a
# lighter line just inside it, both following the shape's union outline and
# anti-aliased. Path commands M L H V C Q Z (absolute) are understood.
import argparse, os, re, sys
import xml.etree.ElementTree as ET

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PyQt5.QtCore import Qt, QPointF, QRectF
from PyQt5.QtGui import QBrush, QColor, QGuiApplication, QImage, QPainter, QPainterPath, QPen, QPixmap, QTransform

ap = argparse.ArgumentParser()
ap.add_argument('shape')
ap.add_argument('out')
ap.add_argument('--tile', help='window background tile (else --fill)')
ap.add_argument('--fill', default='#353535')
ap.add_argument('--border', default='#282828')
ap.add_argument('--light', default='#3d3d3d', help="line inside the border ('none' to skip)")
ap.add_argument('--scale', type=float, default=1.0)
ap.add_argument('--x0', type=float, default=0.0)
args = ap.parse_args()

app = QGuiApplication(['qtshape'])


def parse_path(d):
    path = QPainterPath()
    toks = re.findall(r'[MLHVCQZmlhvcqz]|-?[\d.]+(?:e-?\d+)?', d)
    i, cmd, cur = 0, None, QPointF()
    num = lambda: float(toks[i])
    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                path.closeSubpath()
                continue
        if cmd.islower():
            sys.exit('relative path commands are not supported: ' + cmd)
        n = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'C': 6, 'Q': 4}[cmd]
        v = [float(t) for t in toks[i:i + n]]
        i += n
        if cmd == 'M':
            path.moveTo(*v)
            cmd = 'L'
        elif cmd == 'L':
            path.lineTo(*v)
        elif cmd == 'H':
            path.lineTo(v[0], path.currentPosition().y())
        elif cmd == 'V':
            path.lineTo(path.currentPosition().x(), v[0])
        elif cmd == 'C':
            path.cubicTo(*v)
        elif cmd == 'Q':
            path.quadTo(*v)
    return path


root = ET.parse(args.shape).getroot()
w = float(root.get('width'))
h = float(root.get('height'))
shape = QPainterPath()
for el in root.iter():
    if el.tag.endswith('path'):
        shape = shape.united(parse_path(el.get('d')))
shape = shape.simplified()

sc = args.scale
shape = QTransform().scale(sc, sc).translate(-args.x0, 0).map(shape)
img = QImage(round((w - args.x0) * sc), round(h * sc), QImage.Format_ARGB32_Premultiplied)
img.fill(Qt.transparent)
p = QPainter(img)
p.setRenderHint(QPainter.Antialiasing)
brush = QBrush(QPixmap(args.tile)) if args.tile else QBrush(QColor(args.fill))
p.fillPath(shape, brush)
# frame lines: strokes centred on the outline, clipped to the inside, so a
# 2px pen leaves a 1px line just inside the edge
p.setClipPath(shape)
if args.light != 'none':
    p.strokePath(shape, QPen(QColor(args.light), 4.0))
p.strokePath(shape, QPen(QColor(args.border), 2.0))
p.end()
img.save(args.out)
print('%s: %dx%d' % (args.out, img.width(), img.height()))
