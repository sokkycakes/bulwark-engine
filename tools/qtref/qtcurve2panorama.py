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
        while margin < min(w, h) // 2 and dist(px(x0 + w // 2, y0 + margin), WIN) <= 20:
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
    above = [px(x0 + w // 2, y) for y in range(y0, y0 + m)]
    below = [px(x0 + w // 2, y) for y in range(y0 + h - m, y0 + h)]
    return dict(margin=m, top=top, side=side, bottom=bottom, fill=fill, radius=r,
                above=[c for c in above if dist(c, WIN) > 3], below=[c for c in below if dist(c, WIN) > 3])


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
    return sum(1 for y in range(r.y(), r.bottom()) for x in range(r.x() + 8, r.right() - 8)
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
        col = max((px(dr.x() + x, dr.y() + y) for x, y in diff), key=lambda c: -sum(c))
        w, h = x1 - x0 + 1, y1 - y0 + 1
        im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
        ImageDraw.Draw(im).polygon([(0, 0), (w * SS, 0), (0, h * SS)], fill=rgba(col))
        image_var('btn-default-image', 'default-corner.png', im.resize((w, h), Image.LANCZOS))
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
    idx = [i for i, c in enumerate(col) if dist(c, WIN) > 3]
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
put_groove('slider-fill', groove_at(handle.left() - 12, groove.top(), groove.bottom() + 1))
hb = box(handle)
V['thumb-width'] = '%dpx' % (handle.width() - 2 * hb['margin'])
V['thumb-height'] = '%dpx' % (handle.height() - 2 * hb['margin'])
put_box('thumb', hb)

pr = wrect('progress')
pt = box(pr)
pt = box(pr, pt['margin'], column_x=pr.right() - pt['margin'] - pt['radius'] - 3)
put_box('trough', pt, with_margin=True)
pf = box(QRect(pr.x(), pr.y(), pr.width() // 2, pr.height()), pt['margin'])
put_box('pfill', pf)
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
V['scroll-size'] = '%dpx' % sw.width()
# Where the arrow buttons go is a setting, not something to measure; Panorama
# has one button at each end, so every layout but "none" maps to that.
sbtype = setting('scrollbarType', 'kde')
V['scroll-button'] = '0px' if sbtype == 'none' else '%dpx' % (sw.width() - 2)
# groove: across the bar below the thumb
y = (sthumb.bottom() + sgroove.bottom()) // 2
row = [px(x, y) for x in range(sw.x(), sw.x() + sw.width())]
idx = [i for i, c in enumerate(row) if dist(c, WIN) > 3]
V['scroll-groove'] = ' '.join('%d:%s' % (i, hexc(row[i])) for i in idx)  # read back by the image writer
tb = box(QRect(sthumb.x(), sthumb.y(), sthumb.width(), sthumb.height()))
V['sthumb-width'] = '%dpx' % (sthumb.width() - 2 * tb['margin'])
V['sthumb-length'] = '%dpx' % (sthumb.height() - 2 * tb['margin'])
V['sthumb-bt'] = hexc(tb['side'])
V['sthumb-bg'] = gradient([px(x, sthumb.y() + tb['margin'] + tb['radius'] + 3)
                           for x in range(sthumb.x() + tb['margin'] + 1, sthumb.right() - tb['margin'])], vertical=False)
V['sthumb-radius'] = '%dpx' % tb['radius']

tw = probes['tabs']
bar = tw.tabBar()
off = bar.mapTo(root, bar.rect().topLeft())
ta = box(bar.tabRect(0).translated(off), 0)
ti = box(bar.tabRect(1).translated(off), 0)
for name, b, r in (('tab-active', ta, bar.tabRect(0)), ('tab', ti, bar.tabRect(1))):
    m = 0
    while m < r.height() // 2 and dist(px(r.translated(off).center().x(), r.translated(off).y() + m), WIN) <= 3:
        m += 1
    rr = r.translated(off)
    b = box(rr, m)
    b = box(rr, m, column_x=rr.x() + m + b['radius'] + 1)
    V[name + '-top'] = '%dpx' % m
    V[name + '-border'] = hexc(b['top'])
    V[name + '-bg'] = gradient(b['fill'])
    V[name + '-radius'] = '%dpx' % b['radius']
V['tab-bar-height'] = '%dpx' % bar.height()
pane_top = off.y() + bar.height()
pr2 = QRect(tw.mapTo(root, tw.rect().topLeft()).x(), pane_top - 2, tw.width(), tw.height() - bar.height() + 2)
col = [px(pr2.x() + pr2.width() // 2, yy) for yy in range(pane_top - 2, pane_top + 6)]
V['pane-border'] = hexc(min(col, key=sum))
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
