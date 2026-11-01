#!/usr/bin/env python3
# Turns a QtCurve settings file (stylerc) into Panorama CSS for
# data/panorama/styles/qt/qtcurve.css.
#
# Black-box, like the rest of tools/qtref: it draws probe widgets with the
# real QtCurve style using the given stylerc, measures their pixels (border
# colours, fill gradients, corner radius, etch and glow lines) and writes the
# results as CSS custom properties. qtcurve.css's rules only read those
# properties, so the output block re-skins every widget. No QtCurve code is
# read or copied, and no QtCurve-drawn pixels are written out as images.
#
#   python3 qtcurve2panorama.py [stylerc] [-o out.css] [--name Variant]
#
# Without a stylerc it uses QtCurve's defaults. Without --name the block is
# for `.QtCurve` itself (what qtcurve.css ships); with --name Foo it is for
# `.QtCurve.Foo`, so several variants can sit side by side. Needs PyQt5 and
# QtCurve's Qt 5 style (kde-style-qtcurve-qt5); see docs/spec/ui-qt-themes.md.
import argparse, os, shutil, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('stylerc', nargs='?')
ap.add_argument('-o', '--output')
ap.add_argument('--name')
ap.add_argument('--debug', help='save the probe render here')
ap.add_argument('--update', help='replace the generated block of this stylesheet (qtcurve.css) in place')
ap.add_argument('--images', help='where to write this variant\'s glyph images '
                '(default: data/panorama/images/qt/<name lower-cased>, or qtcurve-gen without --name)')
args = ap.parse_args()

# A few settings choose a shape rather than a colour (where the arrow
# buttons go, an X or a tick, a corner triangle on the default button);
# those are read from the file instead of measured.
settings = {}
if args.stylerc:
    for line in open(args.stylerc, encoding='utf-8', errors='replace'):
        if '=' in line:
            k, v = line.split('=', 1)
            settings[k.strip()] = v.strip()


def setting(key, default):
    return settings.get(key, default).lower()

# QtCurve reads $XDG_CONFIG_HOME/qtcurve/stylerc when the style is created.
cfg_home = tempfile.mkdtemp(prefix='qtc-')
os.makedirs(os.path.join(cfg_home, 'qtcurve'))
if args.stylerc:
    shutil.copy(args.stylerc, os.path.join(cfg_home, 'qtcurve', 'stylerc'))
os.environ['XDG_CONFIG_HOME'] = cfg_home
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from PyQt5.QtCore import Qt, QRect
from PyQt5.QtGui import QFont, QPalette
from PyQt5.QtWidgets import (QApplication, QCheckBox, QComboBox, QGroupBox, QLineEdit, QProgressBar,
                             QPushButton, QRadioButton, QScrollBar, QSlider, QStyle, QStyleFactory,
                             QStyleOptionButton, QStyleOptionSlider, QStyleOptionGroupBox, QTabWidget,
                             QWidget)

app = QApplication(['qtcurve2panorama'])
style = QStyleFactory.create('QtCurve')
if style is None:
    sys.exit('QtCurve style not found: install kde-style-qtcurve-qt5')
app.setStyle(style)
pal = style.standardPalette()
app.setPalette(pal)
font = QFont('DejaVu Sans')
font.setPixelSize(12)
app.setFont(font)

root = QWidget()
root.resize(900, 520)
probes = {}


def put(name, w, x, y, ww, hh, parent=None):
    w.setParent(parent or root)
    w.setGeometry(x, y, ww, hh)
    probes[name] = w
    return w


def hover(w):
    w.setAttribute(Qt.WA_UnderMouse, True)
    return w


# Buttons: the gallery's size, every state.
for i, st in enumerate(('normal', 'hover', 'pressed', 'default', 'disabled', 'checked')):
    b = put('btn-' + st, QPushButton(''), 10 + i * 100, 10, 90, 28)
    if st == 'hover':
        hover(b)
    elif st == 'pressed':
        b.setDown(True)
    elif st == 'default':
        b.setDefault(True)
    elif st == 'disabled':
        b.setEnabled(False)
    elif st == 'checked':
        b.setCheckable(True)
        b.setChecked(True)
for i, st in enumerate(('normal', 'hover', 'checked', 'disabled')):
    c = put('chk-' + st, QCheckBox(''), 10 + i * 60, 50, 40, 22)
    r = put('rad-' + st, QRadioButton(''), 250 + i * 60, 50, 40, 22)
    for w in (c, r):
        if st == 'hover':
            hover(w)
        elif st == 'checked':
            w.setChecked(True)
        elif st == 'disabled':
            w.setChecked(True)
            w.setEnabled(False)
for i, st in enumerate(('normal', 'focus', 'disabled')):
    e = put('entry-' + st, QLineEdit(''), 10 + i * 150, 90, 140, 26)
    if st == 'disabled':
        e.setEnabled(False)
for i, st in enumerate(('normal', 'hover')):
    cb = put('combo-' + st, QComboBox(), 470 + i * 150, 90, 140, 26)
    if st == 'hover':
        hover(cb)
s = put('slider', QSlider(Qt.Horizontal), 10, 130, 200, 22)
s.setRange(0, 100)
s.setValue(40)
p = put('progress', QProgressBar(), 230, 130, 200, 22)
p.setValue(50)
p.setTextVisible(False)
sb = put('scroll-v', QScrollBar(Qt.Vertical), 10, 170, 16, 200)
sb.setRange(0, 100)
sb.setPageStep(30)
sb.setValue(0)
sbh = put('scroll-h', QScrollBar(Qt.Horizontal), 400, 440, 200, 16)
sbh.setRange(0, 100)
sbh.setPageStep(30)
sbh.setValue(0)
g = put('group', QGroupBox('Group box'), 40, 170, 180, 110)
t = put('tabs', QTabWidget(), 240, 170, 380, 140)
for n in ('First', 'Second', 'Third'):
    t.addTab(QWidget(), n)

# Text probes: is the default button's label bold?
put('txt-normal', QPushButton('Default'), 10, 400, 90, 28)
put('txt-default', QPushButton('Default'), 110, 400, 90, 28).setDefault(True)
from PyQt5.QtWidgets import QLabel
put('txt-label', QLabel('Group box'), 220, 400, 120, 28)

root.show()
app.processEvents()
probes['entry-focus'].setFocus()
app.processEvents()
img = root.grab().toImage()
if args.debug:
    img.save(args.debug)

WINDOW = pal.color(QPalette.Window)
WIN = (WINDOW.red(), WINDOW.green(), WINDOW.blue())


def px(x, y):
    c = img.pixelColor(x, y)
    return (c.red(), c.green(), c.blue())


def dist(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


# The window background can be patterned (bgndAppearance=striped): compare
# against what the window draws at that spot, taken from a column and a row
# no probe covers, instead of one flat colour.
BG_X = root.width() - 6
BG_Y = root.height() - 6


def bgc(x, y):
    a, b, c = px(BG_X, y), px(x, BG_Y), px(BG_X, BG_Y)
    # horizontal stripes vary down the column, vertical ones along the row
    return tuple(max(0, min(255, a[k] + b[k] - c[k])) for k in range(3))


def hexc(c):
    return '#%02x%02x%02x' % c


def gradient(colors, vertical=True):
    """A Panorama gradient through `colors` (top to bottom), keeping only the
    stops needed to stay within 2 levels of every sample."""
    if not colors:
        return 'none'
    # A one-pixel inner line at either end (QtCurve's inner shadow or
    # highlight) can't be drawn as a gradient stop: Panorama interpolates
    # gradients across a rounded box's triangle fan, so a sharp stop smears
    # diagonally. Drop such lines and keep the body of the fill.
    colors = list(colors)
    for _ in range(2):
        if len(colors) > 4 and dist(colors[0], colors[1]) > 6 and dist(colors[1], colors[2]) <= 3:
            colors = colors[1:]
        if len(colors) > 4 and dist(colors[-1], colors[-2]) > 6 and dist(colors[-2], colors[-3]) <= 3:
            colors = colors[:-1]
    if all(dist(c, colors[0]) <= 1 for c in colors):
        return hexc(colors[0])
    n = len(colors)
    keep = {0, n - 1}

    def split(a, b):
        worst, at = 0, None
        for i in range(a + 1, b):
            f = (i - a) / (b - a)
            lerp = tuple(colors[a][k] + (colors[b][k] - colors[a][k]) * f for k in range(3))
            d = max(abs(lerp[k] - colors[i][k]) for k in range(3))
            if d > worst:
                worst, at = d, i
        if at is not None and worst > 2:
            keep.add(at)
            split(a, at)
            split(at, b)

    split(0, n - 1)
    idx = sorted(keep)
    end = '0% 100%' if vertical else '100% 0%'
    parts = ['from( %s )' % hexc(colors[idx[0]])]
    for i in idx[1:-1]:
        # pixel centres: sample i covers (i + 0.5) / n of the box
        parts.append('color-stop( %.3f, %s )' % ((i + 0.5) / n, hexc(colors[i])))
    parts.append('to( %s )' % hexc(colors[idx[-1]]))
    return 'gradient( linear, 0%% 0%%, %s, %s )' % (end, ', '.join(parts))


def box(rect, margin=None, column_x=None):
    """Measures a framed box drawn inside `rect` (window coordinates).
    Returns margin, top/side/bottom border colours, the fill gradient down a
    column clear of any label, the radius and what is drawn in the margin."""
    x0, y0, w, h = rect.x(), rect.y(), rect.width(), rect.height()
    cy = y0 + h // 2
    if margin is None:
        margin = 0
        # the border is the first clearly dark line; etch and glow lines
        # (light, or drawn only in some states) sit outside it
        while margin < min(w, h) // 2 and dist(px(x0 + w // 2, y0 + margin), bgc(x0 + w // 2, y0 + margin)) <= 20:
            margin += 1
    m = margin
    top = px(x0 + w // 2, y0 + m)
    bottom = px(x0 + w // 2, y0 + h - 1 - m)
    side = px(x0 + m, cy)
    # radius: along the top border row, how far in the border colour starts
    r = 0
    while r < min(w, h) // 2 and dist(px(x0 + m + r, y0 + m), top) > 24:
        r += 1
    cx = column_x if column_x is not None else x0 + m + max(r, 1) + 2
    fill = [px(cx, y) for y in range(y0 + m + 1, y0 + h - 1 - m)]
    xm = x0 + w // 2
    above = [px(xm, y) for y in range(y0, y0 + m) if dist(px(xm, y), bgc(xm, y)) > 3]
    below = [px(xm, y) for y in range(y0 + h - m, y0 + h) if dist(px(xm, y), bgc(xm, y)) > 3]
    return dict(margin=m, top=top, side=side, bottom=bottom, fill=fill, radius=r, above=above, below=below)


def shadow(b):
    if b['above'] and b['below']:
        a, c = b['above'][-1], b['below'][0]
        if dist(a, c) <= 12:
            return '0px 0px 0px 1px %s' % hexc(a)
        # different lines above and below: keep the stronger one
        if dist(a, WIN) >= dist(c, WIN):
            return '0px -1px 0px 0px %s' % hexc(a)
        return '0px 1px 0px 0px %s' % hexc(c)
    if b['below']:
        return '0px 1px 0px 0px %s' % hexc(b['below'][0])
    if b['above']:
        return '0px -1px 0px 0px %s' % hexc(b['above'][-1])
    return 'none'


V = {}  # variable name -> value


def saturated(c):
    return max(c) - min(c) > 40


def put_box(prefix, b, with_radius=True, with_margin=False, ref=None):
    V[prefix + '-bt'] = hexc(b['top'])
    V[prefix + '-bs'] = hexc(b['side'])
    V[prefix + '-bb'] = hexc(b['bottom'])
    fill = b['fill']
    V[prefix + '-shadow'] = shadow(b)
    # A coloured line just inside the border that the plain state (ref)
    # doesn't have: QtCurve's "coloured" mouse-over. Panorama takes one
    # shadow per box, so it becomes an inset ring in place of the etch.
    if ref is not None and len(fill) > 4:
        ends = [c for c, r in ((fill[0], ref['fill'][0]), (fill[-1], ref['fill'][-1]))
                if saturated(c) and dist(c, r) > 30]
        if ends:
            V[prefix + '-shadow'] = 'inset 0px 0px 0px 1px %s' % hexc(ends[0])
            fill = fill[1:-1]
    V[prefix + '-bg'] = gradient(fill)
    if with_radius:
        V[prefix + '-radius'] = '%dpx' % b['radius']
    if with_margin:
        V[prefix + '-margin'] = '%dpx' % b['margin']


def wrect(name):
    w = probes[name]
    return QRect(w.mapTo(root, w.rect().topLeft()), w.size())


def palette_vars():
    def role(r, group=QPalette.Active):
        c = pal.color(group, r)
        return hexc((c.red(), c.green(), c.blue()))
    V['window'] = role(QPalette.Window)
    V['window-text'] = role(QPalette.WindowText)
    V['base'] = role(QPalette.Base)
    V['text'] = role(QPalette.Text)
    V['button-text'] = role(QPalette.ButtonText)
    V['highlight'] = role(QPalette.Highlight)
    V['highlighted-text'] = role(QPalette.HighlightedText)
    V['disabled-text'] = role(QPalette.WindowText, QPalette.Disabled)


palette_vars()

# Buttons: the normal state sets the margin; the others are measured with it.
nb = box(wrect('btn-normal'))
put_box('btn', nb, with_margin=True)
for st in ('hover', 'pressed', 'default', 'disabled', 'checked'):
    put_box('btn-' + st, box(wrect('btn-' + st), nb['margin']), with_radius=False,
            ref=nb if st == 'hover' else None)

# ---- images: small glyphs we draw ourselves in measured colours ----------
from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
img_sub = (args.name or 'qtcurve-gen').lower()
img_dir = args.images or os.path.join(REPO, 'data', 'panorama', 'images', 'qt', img_sub)
images = {}
SS = 8


def image_var(var, fname, im):
    images[fname] = im
    V[var] = 'url( "file://{images}/qt/%s/%s" )' % (os.path.basename(img_dir.rstrip('/')), fname)


def rgba(c, a=255):
    return tuple(c) + (a,)


def draw_lines(size, segments, color, width):
    im = Image.new('RGBA', (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for seg in segments:
        d.line([(x * SS, y * SS) for x, y in seg], fill=rgba(color), width=int(width * SS), joint='curve')
    return im.resize((size, size), Image.LANCZOS)


def bold_text(name):
    r = wrect(name)
    # rows clear of the frame, which can be dark itself
    return sum(1 for y in range(r.y() + 6, r.bottom() - 5) for x in range(r.x() + 8, r.right() - 8)
               if sum(px(x, y)) < 300)


V['btn-default-weight'] = 'bold' if bold_text('txt-default') > bold_text('txt-normal') * 1.15 else 'normal'

# Default button marked by a corner triangle instead of a recoloured frame.
V['btn-default-image'] = 'none'
V['btn-default-image-pos'] = '0px 0px'
if setting('defBtnIndicator', 'fontcolor') == 'corner':
    dr, nr = wrect('btn-default'), wrect('btn-normal')
    m = nb['margin']
    diff = [(x, y) for y in range(m + 1, 16) for x in range(m + 1, 16)
            if dist(px(dr.x() + x, dr.y() + y), px(nr.x() + x, nr.y() + y)) > 30]
    if diff:
        x0, y0 = min(p[0] for p in diff), min(p[1] for p in diff)
        x1, y1 = max(p[0] for p in diff), max(p[1] for p in diff)
        w, h = x1 - x0 + 1, y1 - y0 + 1
        # the triangle's own pixels, edges included (QtCurve draws it crisp)
        tri = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        for x, y in diff:
            tri.putpixel((x - x0, y - y0), rgba(px(dr.x() + x, dr.y() + y)))
        image_var('btn-default-image', 'default-corner.png', tri)
        # Panorama places backgrounds inside the border
        V['btn-default-image-pos'] = '%dpx %dpx' % (x0 - m - 1, y0 - m - 1)
        V['btn-default-image-size'] = '%dpx %dpx' % (w, h)
        for k in ('bg', 'bt', 'bs', 'bb', 'shadow'):
            V['btn-default-' + k] = V['btn-' + k]
V.setdefault('btn-default-image-size', '1px 1px')


def indicator(name, element):
    w = probes[name]
    opt = QStyleOptionButton()
    opt.initFrom(w)
    r = style.subElementRect(element, opt, w)
    return QRect(w.mapTo(root, r.topLeft()), r.size())


ci = indicator('chk-normal', QStyle.SE_CheckBoxIndicator)
cn = box(ci)
V['check-size'] = '%dpx' % (ci.width() - 2 * cn['margin'])
put_box('check', cn)
for st in ('hover', 'disabled'):
    put_box('check-' + st, box(indicator('chk-' + st, QStyle.SE_CheckBoxIndicator), cn['margin']), with_radius=False,
            ref=cn if st == 'hover' else None)
# Check marks would cover the fill column; the unchecked boxes give the fill.
di = indicator('chk-disabled', QStyle.SE_CheckBoxIndicator)
V['check-disabled-bg'] = hexc(px(di.x() + cn['margin'] + 2, di.y() + cn['margin'] + 2))
# A checked box can have its own fill (crButton/crColor): a column just
# inside the border, clear of the mark.
cc = indicator('chk-checked', QStyle.SE_CheckBoxIndicator)
put_box('check-checked', box(cc, cn['margin'], column_x=cc.x() + cn['margin'] + 2), with_radius=False)


def mark_colour(r):
    inner = [px(x, y) for y in range(r.y() + 3, r.bottom() - 2) for x in range(r.x() + 3, r.right() - 2)]
    return min(inner, key=sum)


mark = mark_colour(cc)
mark_dis = mark_colour(di)
size = 14
if setting('xCheck', 'false') == 'true':
    segs = [[(3.5, 3.5), (10.5, 10.5)], [(10.5, 3.5), (3.5, 10.5)]]
    width = 2.6
else:
    segs = [[(3.0, 6.8), (5.6, 9.6), (10.6, 3.4)]]
    width = 2.4
image_var('check-image', 'check.png', draw_lines(size, segs, mark, width))
image_var('check-disabled-image', 'check-disabled.png', draw_lines(size, segs, mark_dis, width))

ri = indicator('rad-normal', QStyle.SE_RadioButtonIndicator)
rn = box(ri)
V['radio-size'] = '%dpx' % (ri.width() - 2 * rn['margin'])
V['radio-border'] = hexc(rn['side'])
V['radio-bg'] = gradient([px(ri.x() + ri.width() // 2, y) for y in range(ri.y() + rn['margin'] + 2, ri.y() + ri.height() - rn['margin'] - 2)])
rh = box(indicator('rad-hover', QStyle.SE_RadioButtonIndicator), rn['margin'])
V['radio-hover-border'] = hexc(rh['side'])

en = box(wrect('entry-normal'))
put_box('entry', en, with_margin=True)
for st in ('focus', 'disabled'):
    put_box('entry-' + st, box(wrect('entry-' + st), en['margin']), with_radius=False)

cbn = box(wrect('combo-normal'))
put_box('combo', cbn, with_margin=True)
put_box('combo-hover', box(wrect('combo-hover'), cbn['margin']), with_radius=False, ref=cbn)


def slider_parts():
    w = probes['slider']
    opt = QStyleOptionSlider()
    opt.initFrom(w)
    opt.orientation = Qt.Horizontal
    opt.minimum, opt.maximum, opt.sliderPosition, opt.sliderValue = 0, 100, 40, 40
    groove = style.subControlRect(QStyle.CC_Slider, opt, QStyle.SC_SliderGroove, w)
    handle = style.subControlRect(QStyle.CC_Slider, opt, QStyle.SC_SliderHandle, w)
    off = w.mapTo(root, w.rect().topLeft())
    return groove.translated(off), handle.translated(off)


def groove_at(x, y0, y1):
    """The drawn run of non-window pixels down column x: its colours."""
    col = [px(x, y) for y in range(y0, y1)]
    idx = [i for i, c in enumerate(col) if dist(c, bgc(x, y0 + i)) > 3]
    if not idx:
        return []
    return col[idx[0]:idx[-1] + 1]


def put_groove(prefix, run, inner=True):
    # run = [etch?, border, fill..., border, etch?]: the border is the
    # darkest pair at the two ends.
    if len(run) < 3:
        return
    lum = [sum(c) for c in run]
    a = min(range(0, min(2, len(run))), key=lambda i: lum[i])
    b = max(range(len(run) - min(2, len(run)), len(run)), key=lambda i: -lum[i])
    V[prefix + '-border'] = hexc(run[a])
    V[prefix + '-bg'] = gradient(run[a + 1:b])
    V[prefix + '-height'] = '%dpx' % (b - a + 1)
    V[prefix + '-shadow'] = ('0px 1px 0px 0px %s' % hexc(run[b + 1])) if b + 1 < len(run) else 'none'


groove, handle = slider_parts()
put_groove('groove', groove_at(handle.right() + 12, groove.top(), groove.bottom() + 1))


def groove_radius(x_mid, y0, y1):
    # the groove's drawn rows down x_mid; then how far in from the groove's
    # right end its top border row starts
    col = [y for y in range(y0, y1) if dist(px(x_mid, y), bgc(x_mid, y)) > 3]
    if len(col) < 3:
        return 0
    yc = (col[0] + col[-1]) // 2
    x_end = x_mid
    while dist(px(x_end + 1, yc), bgc(x_end + 1, yc)) > 3:
        x_end += 1
    # top border row: darkest of the first two drawn rows
    yt = min(col[:2], key=lambda y: sum(px(x_mid, y)))
    border = px(x_mid, yt)
    r = 0
    while r < (col[-1] - col[0]) and dist(px(x_end - r, yt), border) > 24:
        r += 1
    return r


V['groove-radius'] = '%dpx' % groove_radius(handle.right() + 12, groove.top(), groove.bottom() + 1)
put_groove('slider-fill', groove_at(handle.left() - 12, groove.top(), groove.bottom() + 1))
def grip(rect, b, along_x, fname, var):
    """The decoration in the middle of a thumb (lines, dots): every pixel
    that differs from the thumb's plain fill at the same position across the
    gradient. Saved as an image the size of the decoration."""
    m, r = b['margin'], b['radius']
    # inner box, clear of the border and of the rounded ends
    x0, y0, x1, y1 = rect.x() + m + 1, rect.y() + m + 1, rect.right() - m, rect.bottom() - m
    if along_x:
        x0, x1 = x0 + r, x1 - r
    else:
        y0, y1 = y0 + r, y1 - r

    def median(cs):
        return tuple(sorted(c[k] for c in cs)[len(cs) // 2] for k in range(3))

    hits = []
    if along_x:  # fill varies down the thumb: compare with the row's median
        for y in range(y0, y1):
            ref = median([px(x, y) for x in range(x0, x1)])
            hits += [(x, y) for x in range(x0, x1) if dist(px(x, y), ref) > 6]
    else:
        for x in range(x0, x1):
            ref = median([px(x, y) for y in range(y0, y1)])
            hits += [(x, y) for y in range(y0, y1) if dist(px(x, y), ref) > 6]
    V[var] = 'none'
    V[var + '-size'] = '1px 1px'
    if not hits:
        return
    gx0, gy0 = min(h[0] for h in hits), min(h[1] for h in hits)
    gx1, gy1 = max(h[0] for h in hits), max(h[1] for h in hits)
    im = Image.new('RGBA', (gx1 - gx0 + 1, gy1 - gy0 + 1), (0, 0, 0, 0))
    for x, y in hits:
        im.putpixel((x - gx0, y - gy0), rgba(px(x, y)))
    image_var(var, fname, im)
    V[var + '-size'] = '%dpx %dpx' % im.size


hb = box(handle)
V['thumb-width'] = '%dpx' % (handle.width() - 2 * hb['margin'])
V['thumb-height'] = '%dpx' % (handle.height() - 2 * hb['margin'])
put_box('thumb', hb)
grip(handle, hb, True, 'slider-grip.png', 'thumb-grip')

pr = wrect('progress')
pt = box(pr)
pt = box(pr, pt['margin'], column_x=pr.right() - pt['margin'] - pt['radius'] - 3)
put_box('trough', pt, with_margin=True)
pf = box(QRect(pr.x(), pr.y(), pr.width() // 2, pr.height()), pt['margin'])
put_box('pfill', pf)
# Striped fills (stripedProgress): one period of the stripes, copied as a
# tile that repeats along the bar under the fill's border.
V['pfill-image'] = 'none'
V['pfill-image-size'] = '1px 1px'
if setting('stripedProgress', 'none') != 'none':
    fm = pt['margin'] + 1
    fx0, fx1 = pr.x() + fm + 6, pr.x() + pr.width() // 2 - fm - 6
    fy0, fy1 = pr.y() + fm, pr.bottom() - fm + 1
    yc = (fy0 + fy1) // 2

    # The fill also brightens along the bar (glowProgress), so match the
    # steps between neighbours rather than the colours themselves.
    def step(x, y):
        return sum(px(x + 1, y)) - sum(px(x, y))

    rows = range(fy0 + 2, fy1 - 2, 2)

    def mismatch(p):
        d = [abs(step(x, y) - step(x + p, y)) for y in rows for x in range(fx0, fx1 - p - 1)]
        return sum(d) / len(d)

    best = min(range(6, min(40, (fx1 - fx0) // 2)), key=mismatch)
    sys.stderr.write('progress stripe period %d (mismatch %.1f)\n' % (best, mismatch(best)))
    # one period from the middle, with the brightening along the bar taken out
    tx0 = (fx0 + fx1) // 2 - best // 2
    tile = Image.new('RGBA', (best, fy1 - fy0))
    for y in range(fy0, fy1):
        slope = [(px(fx1 - best + k, y)[c] - px(fx0 + k, y)[c]) / max(1, fx1 - best - fx0) for k in range(best) for c in range(3)]
        sl = [sum(slope[c::3]) / best for c in range(3)]
        for x in range(best):
            c = px(tx0 + x, y)
            tile.putpixel((x, y - fy0), tuple(max(0, min(255, round(c[k] - sl[k] * (x - best / 2)))) for k in range(3)) + (255,))
    image_var('pfill-image', 'progress-stripes.png', tile)
    V['pfill-image-size'] = '%dpx 100%%' % best
# the trough's left edge and corners are covered by the fill
V['trough-bs'] = V['trough-bt']
V['trough-radius'] = V['pfill-radius']


def scroll_parts():
    w = probes['scroll-v']
    opt = QStyleOptionSlider()
    opt.initFrom(w)
    opt.orientation = Qt.Vertical
    opt.minimum, opt.maximum, opt.pageStep = 0, 100, 30
    opt.sliderPosition = opt.sliderValue = 0
    off = w.mapTo(root, w.rect().topLeft())
    get = lambda sc: style.subControlRect(QStyle.CC_ScrollBar, opt, sc, w).translated(off)
    return w, get(QStyle.SC_ScrollBarSlider), get(QStyle.SC_ScrollBarSubLine), get(QStyle.SC_ScrollBarAddLine), \
        get(QStyle.SC_ScrollBarGroove)


sw, sthumb, ssub, sadd, sgroove = scroll_parts()
if args.debug:
    sys.stderr.write('scrollbar: bar %s thumb %s sub %s add %s groove %s\n' % (wrect('scroll-v'), sthumb, ssub, sadd, sgroove))
V['scroll-size'] = '%dpx' % sw.width()
# Where the arrow buttons go is a setting, not something to measure; Panorama
# has one button at each end, so every layout but "none" maps to that.
sbtype = setting('scrollbarType', 'kde')
V['scroll-button'] = '0px' if sbtype == 'none' else '%dpx' % max(1, sadd.height())  # one button's length
# groove: across the bar below the thumb
y = (sthumb.bottom() + sgroove.bottom()) // 2
row = [px(x, y) for x in range(sw.x(), sw.x() + sw.width())]
idx = [i for i, c in enumerate(row) if dist(c, bgc(sw.x() + i, y)) > 3]
V['scroll-groove'] = ' '.join('%d:%s' % (i, hexc(row[i])) for i in idx)  # read back by the image writer
tb = box(QRect(sthumb.x(), sthumb.y(), sthumb.width(), sthumb.height()))
V['sthumb-width'] = '%dpx' % (sthumb.width() - 2 * tb['margin'])
V['sthumb-length'] = '%dpx' % (sthumb.height() - 2 * tb['margin'])
V['sthumb-bt'] = hexc(tb['side'])
V['sthumb-bg'] = gradient([px(x, sthumb.y() + tb['margin'] + tb['radius'] + 3)
                           for x in range(sthumb.x() + tb['margin'] + 1, sthumb.right() - tb['margin'])], vertical=False)
V['sthumb-radius'] = '%dpx' % tb['radius']
grip(sthumb, tb, False, 'scroll-grip-v.png', 'sthumb-grip-v')
# the horizontal bar: same thumb turned, with its own fill and grip
hw = probes['scroll-h']
hopt = QStyleOptionSlider()
hopt.initFrom(hw)
hopt.orientation = Qt.Horizontal
hopt.minimum, hopt.maximum, hopt.pageStep = 0, 100, 30
hopt.sliderPosition = hopt.sliderValue = 0
hthumb = style.subControlRect(QStyle.CC_ScrollBar, hopt, QStyle.SC_ScrollBarSlider, hw).translated(hw.mapTo(root, hw.rect().topLeft()))
htb = box(hthumb)
hfill = [px(hthumb.x() + htb['margin'] + htb['radius'] + 3, y)
         for y in range(hthumb.y() + htb['margin'] + 1, hthumb.bottom() - htb['margin'])]


def thumb_corner(r, b):
    """The thumb's corner radius, counting the anti-aliased pixels that a
    coarse 24-level test treats as border, and QtCurve's one-pixel light ring
    inside the border (drawn as an inset shadow, so the fill stays a plain
    gradient)."""
    m = b['margin']
    rad = 0
    while rad < min(r.width(), r.height()) // 2 and dist(px(r.x() + m + rad, r.y() + m), b['top']) > 8:
        rad += 1
    cy = r.y() + r.height() // 2
    ring = px(r.x() + m + 1, cy)
    inner = None
    if sum(ring) > sum(px(r.x() + m + 3, cy)) + 24 and dist(ring, px(r.x() + r.width() // 2, r.y() + m + 1)) <= 6:
        inner = ring
    return rad, inner


hrad, hinner = thumb_corner(hthumb, htb)
V['sthumb-radius'] = '%dpx' % hrad
V['sthumb-inner'] = 'none'
if hinner is not None:
    V['sthumb-inner'] = 'inset 0px 0px 0px 1px %s' % hexc(hinner)
    hfill = hfill[1:-1]
    V['sthumb-bg'] = gradient([px(x, sthumb.y() + tb['margin'] + tb['radius'] + 3)
                               for x in range(sthumb.x() + tb['margin'] + 2, sthumb.right() - tb['margin'] - 1)],
                              vertical=False)
if args.debug:
    sys.stderr.write('hthumb %s radius %d inner %s\n' % (hthumb, hrad, hinner))
V['sthumb-bg-h'] = gradient(hfill)
grip(hthumb, htb, True, 'scroll-grip-h.png', 'sthumb-grip-h')


def capture(rect, fname, var):
    """Everything drawn inside rect that isn't the window behind it."""
    im = Image.new('RGBA', (rect.width(), rect.height()), (0, 0, 0, 0))
    for y in range(rect.y(), rect.y() + rect.height()):
        for x in range(rect.x(), rect.x() + rect.width()):
            if dist(px(x, y), bgc(x, y)) > 4:
                im.putpixel((x - rect.x(), y - rect.y()), rgba(px(x, y)))
    image_var(var, fname, im)
    V[var + '-size'] = '%dpx %dpx' % im.size


# Arrow buttons: copied whole (arrow and any frame QtCurve gives them).
hget = lambda sc: style.subControlRect(QStyle.CC_ScrollBar, hopt, sc, hw).translated(hw.mapTo(root, hw.rect().topLeft()))
# Qt's sub-line rect can span the whole bar (the kde layout puts a second
# back button next to the forward one), so take one button's length at each
# end, the length of the last (add-line) button.
vbar, hbar = wrect('scroll-v'), wrect('scroll-h')
hadd = hget(QStyle.SC_ScrollBarAddLine)
vl, hl = max(1, sadd.height()), max(1, hadd.width())
for rect, fname, var in ((QRect(vbar.x(), vbar.y(), vbar.width(), vl), 'scroll-up.png', 'scroll-up'),
                         (QRect(vbar.x(), vbar.bottom() - vl + 1, vbar.width(), vl), 'scroll-down.png', 'scroll-down'),
                         (QRect(hbar.x(), hbar.y(), hl, hbar.height()), 'scroll-left.png', 'scroll-left'),
                         (QRect(hbar.right() - hl + 1, hbar.y(), hl, hbar.height()), 'scroll-right.png', 'scroll-right')):
    if sbtype == 'none' or rect.width() <= 0 or rect.height() <= 0:
        V[var] = 'none'
        V[var + '-size'] = '1px 1px'
    else:
        # Qt's last add-line button sits at the very end of the bar
        capture(rect, fname, var)
# The groove across the bar, as drawn between the buttons.
gim = Image.new('RGBA', (sw.width(), 1), (0, 0, 0, 0))
for i in idx:
    gim.putpixel((i, 0), rgba(row[i]))
image_var('scroll-groove-v', 'scroll-groove-v.png', gim)
image_var('scroll-groove-h', 'scroll-groove-h.png', gim.transpose(Image.TRANSPOSE))

tw = probes['tabs']
bar = tw.tabBar()
off = bar.mapTo(root, bar.rect().topLeft())
ta = box(bar.tabRect(0).translated(off), 0)
ti = box(bar.tabRect(1).translated(off), 0)
tab_extent = {}
for name, b, r in (('tab-active', ta, bar.tabRect(0)), ('tab', ti, bar.tabRect(1))):
    m = 0
    while m < r.height() // 2 and dist(px(r.translated(off).center().x(), r.translated(off).y() + m),
                                        bgc(r.translated(off).center().x(), r.translated(off).y() + m)) <= 3:
        m += 1
    rr = r.translated(off)
    b = box(rr, m)
    # The tab's drawn frame along a middle row clear of the label: from the
    # centre outwards to the first clearly darker pixel on each side.
    ym = rr.y() + m + 3
    fill_c = px(rr.center().x(), ym)
    xl = rr.center().x()
    while xl > rr.x() - 3 and sum(px(xl, ym)) > sum(fill_c) - 60:
        xl -= 1
    xr = rr.center().x()
    while xr < rr.right() + 3 and sum(px(xr, ym)) > sum(fill_c) - 60:
        xr += 1
    side = px(xl, ym)
    top_y = rr.y() + m
    rs = 0
    while rs < rr.height() // 2 and dist(px(xl, top_y + rs), side) > 24:
        rs += 1
    rad = max(b['radius'], rs)
    b = box(rr, m, column_x=xl + rad + 2)
    b['side'] = side
    side_x = xl
    xs = [xl, xr]
    V[name + '-top'] = '%dpx' % m
    V[name + '-height'] = '%dpx' % (bar.height() - m)
    V[name + '-border'] = hexc(b['top'])
    V[name + '-bs'] = hexc(b['side'])
    fill = b['fill']
    # a lighter line just inside the frame (QtCurve's inner highlight)
    V[name + '-inner'] = 'none'
    if len(fill) > 4 and sum(fill[0]) > sum(fill[2]) + 30 and sum(px(side_x + 1, ym + 4)) > sum(px(side_x + 3, ym + 4)) + 30:
        V[name + '-inner'] = 'inset 0px 0px 0px 1px %s' % hexc(fill[0])
        fill = fill[1:]
    V[name + '-bg'] = gradient(fill)
    V[name + '-radius'] = '%dpx' % rad
    if xs:
        text_w = bar.fontMetrics().horizontalAdvance(bar.tabText(0 if name == 'tab-active' else 1))
        V[name + '-padding'] = '%dpx' % max(0, (xs[-1] - xs[0] + 1 - 2 - text_w) // 2)
        tab_extent[name] = (xs[0], xs[-1])
V['tab-bar-height'] = '%dpx' % bar.height()
if 'tab' in tab_extent and 'tab-active' in tab_extent:
    V['tab-gap'] = '%dpx' % max(0, tab_extent['tab'][0] - tab_extent['tab-active'][1] - 1)
pane_top = off.y() + bar.height()
pr2 = QRect(tw.mapTo(root, tw.rect().topLeft()).x(), pane_top - 2, tw.width(), tw.height() - bar.height() + 2)
col = [px(pr2.x() + pr2.width() // 2, yy) for yy in range(pane_top - 2, pane_top + 6)]
V['pane-border'] = hexc(min(col, key=sum))
# The pane's top line can run inside the tab bar's height: the active tab
# then covers it (flush) and the others stop on it.
line_at = pane_top - 2 + min(range(len(col)), key=lambda i: sum(col[i]))
overlap = max(0, pane_top - line_at)
V['pane-shift'] = '%dpx' % -overlap
V['tab-bottom'] = '%dpx' % overlap
V['tab-height'] = '%dpx' % (bar.height() - int(V['tab-top'][:-2]) - overlap)
V['tab-bar-x'] = '%dpx' % (tab_extent['tab-active'][0] - tw.mapTo(root, tw.rect().topLeft()).x()) if 'tab-active' in tab_extent else '0px'
V['pane-bg'] = hexc(px(pr2.x() + pr2.width() // 2, pane_top + 20))

gw = probes['group']
gopt = QStyleOptionGroupBox()
gopt.initFrom(gw)
gopt.text = gw.title()
gopt.subControls = QStyle.SC_GroupBoxFrame | QStyle.SC_GroupBoxLabel
gframe = style.subControlRect(QStyle.CC_GroupBox, gopt, QStyle.SC_GroupBoxFrame, gw).translated(gw.mapTo(root, gw.rect().topLeft()))
gb = box(gframe)
V['frame-border'] = hexc(gb['top'])
V['frame-bg'] = gradient(gb['fill'])
V['frame-radius'] = '%dpx' % gb['radius']
glabel = style.subControlRect(QStyle.CC_GroupBox, gopt, QStyle.SC_GroupBoxLabel, gw).translated(gw.mapTo(root, gw.rect().topLeft()))


def dark_pixels(r):
    return sum(1 for y in range(r.y(), r.bottom() + 1) for x in range(r.x(), r.right() + 1) if sum(px(x, y)) < 300)


V['group-title-weight'] = 'bold' if dark_pixels(glabel) > dark_pixels(wrect('txt-label')) * 1.15 else 'normal'

# Where the frame's top line runs relative to the title: through its middle
# (QtCurve's default for gbLabel) or below it. The panel layout is a title
# of fixed height followed by the frame, pulled up by a negative gap.
gtop = gw.mapTo(root, gw.rect().topLeft()).y()
fx = gframe.x() + gframe.width() // 2
line_y = next((y for y in range(gframe.y(), gframe.bottom()) if dist(px(fx, y), bgc(fx, y)) > 20), gframe.y())
title_h = glabel.height()
V['group-title-height'] = '%dpx' % title_h
V['group-title-x'] = '%dpx' % (glabel.x() - gframe.x())
V['group-title-gap'] = '%dpx' % (line_y - (glabel.y() + title_h))
on_line = glabel.y() < line_y < glabel.bottom()
V['group-title-bg'] = V['window'] if on_line else 'none'
V['group-title-image'] = 'window' if on_line else 'none'  # filled in once the window tile is known
V['frame-pad-top'] = '%dpx' % max(0, -(line_y - (glabel.y() + title_h))) if on_line else '0px'
# A frame that fades out downwards (groupBox=faded): sides and fill become
# see-through instead of solid colours.
side_x = next((x for x in range(gframe.x(), gframe.x() + 6) if dist(px(x, line_y + 3), bgc(x, line_y + 3)) > 6), gframe.x())
frame_h = gframe.bottom() - line_y
border_c = px(side_x, line_y + 2)


def alpha_over_bg(c, bgcol, ink):
    # c = bg * (1 - a) + ink * a, per channel, averaged
    vals = [(bgcol[k] - c[k]) / (bgcol[k] - ink[k]) for k in range(3) if abs(bgcol[k] - ink[k]) > 8]
    return max(0.0, min(1.0, sum(vals) / len(vals))) if vals else 0.0


late = px(side_x, line_y + int(frame_h * 0.85))
V['frame-side-width'] = '1px'
V['frame-side-image'] = 'none'
V['frame-side-size'] = '1px 1px'
V['frame-side-top'] = '0px'
if dist(late, bgc(side_x, line_y + int(frame_h * 0.85))) < dist(border_c, bgc(side_x, line_y + 2)) // 3:
    ink = border_c
    # from the line's own row, so the rounded corner pixel comes along
    col = Image.new('RGBA', (1, frame_h))
    for i in range(frame_h):
        y = line_y + i
        a = alpha_over_bg(px(side_x, y), bgc(side_x, y), ink)
        col.putpixel((0, i), rgba(ink, round(a * 255)))
    image_var('frame-side-image', 'group-side.png', col)
    V['frame-side-width'] = '0px'
    V['frame-side-size'] = '1px %dpx' % frame_h
    V['frame-side-top'] = '%dpx' % (title_h + (line_y - (glabel.y() + title_h)))
    # the fill: darkening of whatever is behind, fading out
    stops = []
    for i in range(frame_h - 1):
        y = line_y + 1 + i
        c, b = px(fx, y), bgc(fx, y)
        stops.append(max(0.0, min(1.0, 1 - sum(c) / max(1, sum(b)))))
    keep = [0, len(stops) - 1]
    for i in range(1, len(stops) - 1, max(1, len(stops) // 8)):
        keep.append(i)
    keep = sorted(set(keep))
    parts = ['from( rgba( 0, 0, 0, %.3f ) )' % stops[0]]
    for i in keep[1:-1]:
        parts.append('color-stop( %.3f, rgba( 0, 0, 0, %.3f ) )' % ((i + 0.5) / len(stops), stops[i]))
    parts.append('to( rgba( 0, 0, 0, %.3f ) )' % stops[-1])
    V['frame-bg'] = 'gradient( linear, 0%% 0%%, 0%% 100%%, %s )' % ', '.join(parts)
    V['frame-border'] = hexc(px(fx, line_y))

# Window background: a repeating pattern (bgndAppearance=striped and the
# like) becomes a small tile of the measured colours.
def period(seq):
    for p in range(1, 17):
        if all(seq[i] == seq[i + p] for i in range(len(seq) - p)):
            return p
    return 0


cx = root.width() - 6
colv = [px(cx, y) for y in range(2, 66)]
rowv = [px(x, root.height() - 6) for x in range(root.width() - 70, root.width() - 6)]
pv, ph = period(colv), period(rowv)
V['window-image'] = 'none'
V['window-image-size'] = '1px 1px'
if pv and ph and (pv > 1 or ph > 1):
    # several periods per tile, so a window isn't thousands of tiny quads
    tw, th = ph * -(-32 // ph), pv * -(-32 // pv)
    tile = Image.new('RGBA', (tw, th))
    for yy in range(th):
        for xx in range(tw):
            tile.putpixel((xx, yy), rgba(px(root.width() - 70 + xx % ph, yy % pv)))
    image_var('window-image', 'window.png', tile)
    V['window-image-size'] = '%dpx %dpx' % (tw, th)

if V['group-title-image'] == 'window':
    V['group-title-image'] = V['window-image']
V['progress-text-weight'] = 'bold' if setting('boldProgress', 'true') == 'true' else 'normal'

# ---- output -----------------------------------------------------------------

selector = '.QtCurve' + ('.' + args.name if args.name else '')
lines = ['/* Generated by tools/qtref/qtcurve2panorama.py from %s. */' % (args.stylerc or 'QtCurve defaults'),
         selector, '{']
for k, v in V.items():
    if k == 'scroll-groove':
        continue
    lines.append('\t--%s: %s;' % (k, v))
lines.append('}')
text = '\n'.join(lines) + '\n'
BEGIN, END = '/* BEGIN qtcurve2panorama */', '/* END qtcurve2panorama */'
if (args.update or args.output) and images:
    os.makedirs(img_dir, exist_ok=True)
    for fname, im in images.items():
        im.save(os.path.join(img_dir, fname))
    sys.stderr.write('images: %s\n' % img_dir)
if args.update:
    css = open(args.update).read()
    a, b = css.index(BEGIN) + len(BEGIN), css.index(END)
    open(args.update, 'w').write(css[:a] + '\n' + text + css[b:])
elif args.output:
    open(args.output, 'w').write(text)
else:
    sys.stdout.write(text)
sys.stderr.write('scroll groove across the bar: %s\n' % V['scroll-groove'])
shutil.rmtree(cfg_home, ignore_errors=True)
