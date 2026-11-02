#!/usr/bin/env python3
# Turns a GTK 2 theme (a gtkrc, usually one using the Murrine engine) into
# Panorama CSS for data/panorama/styles/gtk/murrine.css.
#
# Black-box, like tools/qtref/qtcurve2panorama.py: it draws the widget
# gallery with real GTK 2 and the given gtkrc (tools/gtkref/gallery.c), once
# with labels and once without, measures the pixels (border colours, fill
# gradients, corner radius, relief lines) and writes CSS custom properties.
# murrine.css's rules only read those properties. Whatever a flat gradient
# can't carry (Murrine's curved or glassy glaze, its inner light border, the
# progress stripes) becomes a small overlay image: white or black at the
# alpha that turns the measured gradient into the measured pixels. Fixed-size
# glyphs (check and radio indicators, scrollbar steppers, the combo arrow,
# the slider thumb) are copied from the render.
# No engine code is read or copied.
#
#   python3 gtkrc2panorama.py [gtkrc] [-o out.css] [--name Variant] [--update murrine.css]
#
# Without a gtkrc it uses data/panorama/styles/gtk/murrine-defaults.gtkrc.
# Needs GTK 2 development files, Murrine (gtk2-engines-murrine), xvfb-run
# and Pillow; see docs/spec/ui-gtk-themes.md.
import argparse, json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

ap = argparse.ArgumentParser()
ap.add_argument('gtkrc', nargs='?', default=os.path.join(REPO, 'data/panorama/styles/gtk/murrine-defaults.gtkrc'))
ap.add_argument('-o', '--output')
ap.add_argument('--name', help='variant class: writes `.Murrine.<name>` instead of `.Murrine`')
ap.add_argument('--update', help='replace the generated block of this stylesheet (murrine.css) in place')
ap.add_argument('--images', help='where to write the images (default data/panorama/images/gtk/<name lower-cased>, '
                'or murrine-gen without --name)')
ap.add_argument('--debug', help='save the renders as <this>-gallery.png and <this>-blank.png')
ap.add_argument('--gallery-bin', help='a built tools/gtkref/gallery.c (default: build it into a temp dir)')
args = ap.parse_args()

from PIL import Image

work = tempfile.mkdtemp(prefix='gtkref-')


def build_gallery():
    if args.gallery_bin:
        return args.gallery_bin
    exe = os.path.join(work, 'gtk2gallery')
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gtk+-2.0']).decode().split()
    subprocess.check_call(['cc', '-O2', '-w', os.path.join(HERE, 'gallery.c'), '-o', exe] + flags)
    return exe


def render(exe, blank):
    """Runs the gallery under Xvfb; returns (whole-window image, geometry)."""
    font = os.path.join(work, 'font.gtkrc')
    with open(font, 'w') as f:
        # 12px DejaVu Sans, as the Qt references use; no selection on focus
        f.write('gtk-font-name = "DejaVu Sans 9"\ngtk-xft-dpi = 98304\ngtk-xft-antialias = 1\n'
                'gtk-xft-hinting = 1\ngtk-xft-hintstyle = "hintslight"\ngtk-xft-rgba = "none"\n'
                'gtk-entry-select-on-focus = 0\n')
    env = dict(os.environ, GTK2_RC_FILES=os.path.abspath(args.gtkrc) + ':' + font)
    env.pop('GTKREF_BLANK', None)
    if blank:
        env['GTKREF_BLANK'] = '1'
    top, whole = os.path.join(work, 'top.png'), os.path.join(work, 'whole.png')
    out = subprocess.check_output(['xvfb-run', '-a', '-s', '-screen 0 1024x768x24', exe, top, whole], env=env)
    return Image.open(whole).convert('RGB'), json.loads(out)


exe = build_gallery()
GAL, G = render(exe, False)
IMG, _ = render(exe, True)
if args.debug:
    GAL.save(args.debug + '-gallery.png')
    IMG.save(args.debug + '-blank.png')
STYLE = G['style']


def R(name):
    return tuple(G[name])


def px(x, y, im=None):
    return (im or IMG).getpixel((x, y))


def dist(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def hexc(c):
    return '#%02x%02x%02x' % tuple(c[:3])


def unhex(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


WIN = unhex(STYLE['normal']['bg'])


def bgc(x, y):
    """What the window draws at (x, y): its bg colour (GTK 2 windows are flat
    unless a pixmap is set; a pixmap window background is not handled)."""
    return WIN


V = {}
images = {}
img_sub = (args.name or 'murrine-gen').lower()
img_dir = args.images or os.path.join(REPO, 'data', 'panorama', 'images', 'gtk', img_sub)


def image_var(var, fname, im):
    images[fname] = im
    V[var] = 'url( "file://{images}/gtk/%s/%s" )' % (os.path.basename(img_dir.rstrip('/')), fname)
    V[var + '-size'] = '%dpx %dpx' % im.size


def no_image(var):
    V[var] = 'none'
    V[var + '-size'] = '1px 1px'


def gradient(colors, vertical=True):
    """A Panorama gradient through `colors` (start to end), keeping only the
    stops needed to stay within 2 levels of every sample."""
    if not colors:
        return 'none'
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
        parts.append('color-stop( %.3f, %s )' % ((i + 0.5) / n, hexc(colors[i])))
    parts.append('to( %s )' % hexc(colors[idx[-1]]))
    return 'gradient( linear, 0%% 0%%, %s, %s )' % (end, ', '.join(parts))


def median(cs):
    return tuple(sorted(c[k] for c in cs)[len(cs) // 2] for k in range(3))


def box(rect, margin=None, im=None, skip_x=None, vertical=True):
    """Measures a framed box drawn inside rect = (x, y, w, h): margin (rows of
    window before the frame), border colours per side, corner radius, relief
    lines outside the frame, and the face inside the frame as rows (or
    columns, vertical=False) of pixels. skip_x = (x0, x1) leaves out columns
    (a combo arrow) when taking each row's typical colour."""
    im = im or IMG
    x0, y0, w, h = rect
    xm, ym = x0 + w // 2, y0 + h // 2
    if margin is None:
        margin = 0
        while margin < min(w, h) // 2 and dist(px(xm, y0 + margin, im), bgc(xm, y0 + margin)) <= 20:
            margin += 1
    m = margin
    top, bottom = px(xm, y0 + m, im), px(xm, y0 + h - 1 - m, im)
    left, right = px(x0 + m, ym, im), px(x0 + w - 1 - m, ym, im)
    r = 0
    while r < min(w, h) // 2 and dist(px(x0 + m + r, y0 + m, im), top) > 24:
        r += 1
    fine = r
    while fine < min(w, h) // 2 and dist(px(x0 + m + fine, y0 + m, im), top) > 8:
        fine += 1
    fx0, fy0, fx1, fy1 = x0 + m + 1, y0 + m + 1, x0 + w - 1 - m, y0 + h - 1 - m  # face, exclusive ends
    face = [[px(x, y, im) for x in range(fx0, fx1)] for y in range(fy0, fy1)]
    above = [px(xm, y, im) for y in range(y0, y0 + m) if dist(px(xm, y, im), bgc(xm, y)) > 3]
    below = [px(xm, y, im) for y in range(y0 + h - m, y0 + h) if dist(px(xm, y, im), bgc(xm, y)) > 3]
    return dict(rect=rect, margin=m, top=top, bottom=bottom, left=left, right=right, radius=r, fine_radius=fine,
                face=face, face_rect=(fx0, fy0, fx1 - fx0, fy1 - fy0), above=above, below=below, skip_x=skip_x,
                vertical=vertical)


def pick(cs, q):
    """The colour at fraction q of cs sorted by brightness (0.5: median)."""
    cs = sorted(cs, key=sum)
    return cs[min(len(cs) - 1, int(len(cs) * q))]


def base_fill(b, q=None):
    """The face's gradient: each row's (or column's) most common colour, so a
    highlight that covers part of a row doesn't count. With q, the colour at
    that fraction by brightness instead (stripes that cover half a row)."""
    if q is not None:
        rows = b['face'] if b['vertical'] else list(zip(*b['face']))
        r = max(b['fine_radius'], 1)
        return [pick(row[r:len(row) - r] or row, q) for row in rows]
    face = b['face']
    fx0 = b['face_rect'][0]
    keep = lambda x: not (b['skip_x'] and b['skip_x'][0] <= fx0 + x < b['skip_x'][1])
    if b['vertical']:
        # leave out the corner columns
        r = max(b['fine_radius'], 1)
        rows = [[c for x, c in enumerate(row[r:len(row) - r], r) if keep(x)] or row for row in face]
        return [median(row) for row in rows]
    cols = list(zip(*face))
    r = max(b['fine_radius'], 1)
    return [median(col[r:len(col) - r] or col) for col in cols]


def overlay(b, fill, threshold=3):
    """The pixels of the face that the gradient `fill` doesn't explain, as
    white (lighter) or black (darker) at the alpha that gives the measured
    colour over the gradient. None when the gradient explains every pixel."""
    face = b['face']
    h, w = len(face), len(face[0]) if face else 0
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    hits = 0
    for y in range(h):
        for x in range(w):
            if b['skip_x'] and b['skip_x'][0] <= b['face_rect'][0] + x < b['skip_x'][1]:
                continue
            base = fill[y] if b['vertical'] else fill[x]
            c = face[y][x]
            d = sum(c) - sum(base)
            if abs(d) <= threshold * 3 or dist(c, base) <= threshold:
                continue
            hits += 1
            if d > 0:
                a = sum((c[k] - base[k]) / max(1, 255 - base[k]) for k in range(3)) / 3
                out.putpixel((x, y), (255, 255, 255, max(0, min(255, round(a * 255)))))
            else:
                a = sum((base[k] - c[k]) / max(1, base[k]) for k in range(3)) / 3
                out.putpixel((x, y), (0, 0, 0, max(0, min(255, round(a * 255)))))
    if b['skip_x']:
        # columns under the arrow: copy the nearest kept column
        sx0, sx1 = b['skip_x'][0] - b['face_rect'][0], b['skip_x'][1] - b['face_rect'][0]
        src = max(0, sx0 - 1)
        for x in range(max(0, sx0), min(w, sx1)):
            for y in range(h):
                out.putpixel((x, y), out.getpixel((src, y)))
    return out if hits > max(2, w * h // 200) else None


def shadow(b):
    if b['above'] and b['below']:
        a, c = b['above'][-1], b['below'][0]
        if dist(a, c) <= 12:
            return '0px 0px 0px 1px %s' % hexc(a)
        if dist(a, WIN) >= dist(c, WIN):
            return '0px -1px 0px 0px %s' % hexc(a)
        return '0px 1px 0px 0px %s' % hexc(c)
    if b['below']:
        return '0px 1px 0px 0px %s' % hexc(b['below'][0])
    if b['above']:
        return '0px -1px 0px 0px %s' % hexc(b['above'][-1])
    return 'none'


def put_box(prefix, b, with_radius=True, with_margin=False):
    V[prefix + '-bt'] = hexc(b['top'])
    V[prefix + '-bl'] = hexc(b['left'])
    V[prefix + '-br'] = hexc(b['right'])
    V[prefix + '-bb'] = hexc(b['bottom'])
    V[prefix + '-shadow'] = shadow(b)
    fill = base_fill(b)
    V[prefix + '-bg'] = gradient(fill, b['vertical'])
    ov = overlay(b, fill)
    if ov is not None:
        image_var(prefix + '-image', prefix + '.png', ov)
    else:
        no_image(prefix + '-image')
    if with_radius:
        V[prefix + '-radius'] = '%dpx' % b['fine_radius']
    if with_margin:
        V[prefix + '-margin'] = '%dpx' % b['margin']


def capture(rect, fname, var, bg=None, im=None, trim=True, solid=False):
    """Everything drawn inside rect that isn't the background behind it, as
    an image; trimmed to what was drawn unless trim=False. Returns the
    trimmed rectangle."""
    im = im or IMG
    x0, y0, w, h = rect
    pts = [(x, y) for y in range(y0, y0 + h) for x in range(x0, x0 + w)
           if dist(px(x, y, im), bg or bgc(x, y)) > 4]
    if not pts:
        no_image(var)
        return None
    if trim:
        x0, y0 = min(p[0] for p in pts), min(p[1] for p in pts)
        w, h = max(p[0] for p in pts) - x0 + 1, max(p[1] for p in pts) - y0 + 1
    if solid:
        # every pixel between a row's first and last drawn one (a thumb's
        # face can match the window colour in places)
        span = {}
        for x, y in pts:
            a, b = span.get(y, (x, x))
            span[y] = (min(a, x), max(b, x))
        pts = [(x, y) for y, (a, b) in span.items() for x in range(a, b + 1)]
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for x, y in pts:
        if x0 <= x < x0 + w and y0 <= y < y0 + h:
            out.putpixel((x - x0, y - y0), px(x, y, im) + (255,))
    image_var(var, fname, out)
    return (x0, y0, w, h)


# ---- palette ----------------------------------------------------------------
V['window'] = STYLE['normal']['bg']
V['window-text'] = STYLE['normal']['fg']
V['base'] = STYLE['normal']['base']
V['text'] = STYLE['normal']['text']
V['highlight'] = STYLE['selected']['bg']
V['highlighted-text'] = STYLE['selected']['fg']
V['disabled-text'] = STYLE['insensitive']['fg']


def ink(rect, im=GAL):
    """The text colour in rect: the labelled render's pixel furthest from
    the blank render's (light text on a dark theme as well)."""
    x0, y0, w, h = rect
    pts = [(x, y) for y in range(y0, y0 + h) for x in range(x0, x0 + w)]
    x, y = max(pts, key=lambda p: dist(px(p[0], p[1], im), px(p[0], p[1])))
    return px(x, y, im)


def ink_count(rect, im=GAL):
    x0, y0, w, h = rect
    lim = sum(WIN) - 200
    return sum(1 for y in range(y0, y0 + h) for x in range(x0, x0 + w) if sum(px(x, y, im)) < lim)


V['button-text'] = hexc(ink(R('btn-normal.label')))

# ---- push buttons -------------------------------------------------------------
nb = box(R('btn-normal'))
put_box('btn', nb, with_margin=True)
for st in ('hover', 'pressed', 'default', 'disabled', 'checked'):
    put_box('btn-' + st, box(R('btn-' + st), nb['margin']), with_radius=False)
# GTK moves a pressed button's label by child-displacement
lp, ln = R('btn-pressed.label'), R('btn-normal.label')
V['btn-pressed-shift'] = '%dpx' % (lp[1] - ln[1])
V['btn-default-weight'] = 'normal'
if 'btn-default.label' in G:
    V['btn-default-weight'] = 'bold' if ink_count(R('btn-default.label')) > ink_count(R('btn-normal.label')) * 1.25 else 'normal'

# ---- check boxes and radio buttons: the indicator, copied whole ---------------
def widget_bg(name, im=IMG):
    """A check button's own background (GTK paints a prelight box behind a
    hovered one): the pixel at its far right, past the label."""
    x, y, w, h = R(name)
    return px(x + w - 2, y + h // 2, im)


for kind, states in (('chk', ('normal', 'hover', 'checked', 'disabled', 'disabled-off')),
                     ('rad', ('normal', 'hover', 'checked', 'disabled'))):
    prefix = 'check' if kind == 'chk' else 'radio'
    ind = R('%s-normal.indicator' % kind)
    for st in states:
        name = '%s-%s' % (kind, st)
        bg = widget_bg(name)
        r = R(name + '.indicator')
        # one pixel round for relief lines; same size in every state
        rect = (r[0] - 1, r[1] - 1, r[2] + 2, r[3] + 2)
        capture(rect, '%s-%s.png' % (prefix, st), '%s-%s' % (prefix, st), bg=bg, trim=False)
    V[prefix + '-size'] = '%dpx' % (ind[2] + 2)
    V[prefix + '-x'] = '%dpx' % (ind[0] - 1 - R('%s-normal' % kind)[0])
    lab = R('%s-normal.label' % kind) if ('%s-normal.label' % kind) in G else None
    V[prefix + '-label-gap'] = '%dpx' % ((lab[0] - (ind[0] + ind[2] + 1)) if lab else 4)
hb = widget_bg('chk-hover')
V['check-row-hover-bg'] = hexc(hb) if dist(hb, WIN) > 1 else 'none'

# ---- entries ----------------------------------------------------------------
en = box(R('entry-normal'))
put_box('entry', en, with_margin=True)
for st in ('focus', 'disabled'):
    put_box('entry-' + st, box(R('entry-' + st), en['margin']), with_radius=False)


def text_box(rect):
    """Where the labelled render's text is inside rect: what differs from
    the blank render."""
    x0, y0, w, h = rect
    pts = [(x, y) for y in range(y0 + 2, y0 + h - 2) for x in range(x0 + 2, x0 + w - 2)
           if dist(px(x, y, GAL), px(x, y)) > 40 and sum(px(x, y, GAL)) < sum(px(x, y)) - 300]
    if not pts:
        return None
    return min(p[0] for p in pts), min(p[1] for p in pts)


# Text insets. Panorama puts the left edge of 12px DejaVu Sans one pixel
# right of the content box and its cap height 4px below the content top
# of a text entry (2px for a label); GTK's positions are read off the render.
PANO_BEARING, PANO_CAP, PANO_LABEL_CAP = 1, 4, 2
ex, ey, ew, eh = R('entry-normal')
tb_ = text_box(R('entry-normal')) or (ex + 4, ey + 9)
V['entry-pad-x'] = '%dpx' % max(0, tb_[0] - ex - 1 - PANO_BEARING)
V['entry-pad-top'] = '%dpx' % max(0, tb_[1] - ey - 1 - PANO_CAP)

# ---- combo box: a button with an arrow on the right ----------------------------
cx, cy, cw, ch = R('combo-normal')


def arrow_columns(rect):
    """Columns at the right of a combo, inside the frame, that differ from
    the face's left part: the arrow (and any separator)."""
    x0, y0, w, h = rect
    b = box(rect)
    m = b['margin']
    ref_x = x0 + w // 2
    cols = []
    for x in range(x0 + w // 2, x0 + w - m - 2):
        if any(dist(px(x, y), px(ref_x, y)) > 12 for y in range(y0 + m + 3, y0 + h - m - 3)):
            cols.append(x)
    return (min(cols), max(cols) + 1) if cols else None


ac = arrow_columns(R('combo-normal'))
cbn = box(R('combo-normal'), skip_x=ac)
put_box('combo', cbn, with_margin=True)
ach = arrow_columns(R('combo-hover'))
put_box('combo-hover', box(R('combo-hover'), cbn['margin'], skip_x=ach), with_radius=False)
no_image('combo-arrow')
if ac:
    m = cbn['margin']
    fill = base_fill(cbn)
    x0, x1 = ac
    y0, y1 = cy + m + 1, cy + ch - m - 1
    arrow = Image.new('RGBA', (x1 - x0, y1 - y0), (0, 0, 0, 0))
    for y in range(y0, y1):
        base = fill[y - y0]
        for x in range(x0, x1):
            c = px(x, y)
            if dist(c, base) > 6:
                arrow.putpixel((x - x0, y - y0), c + (255,))
    bb = arrow.getbbox()
    if bb:
        arrow = arrow.crop((0, bb[1], arrow.width, bb[3]))
        # transparent columns to the face's right edge, so the image sits at
        # the label's right end (the label fills the face)
        pad = cx + cw - m - 1 - x1
        padded = Image.new('RGBA', (arrow.width + pad, arrow.height), (0, 0, 0, 0))
        padded.paste(arrow, (0, 0))
        image_var('combo-arrow', 'combo-arrow.png', padded)
tb_ = text_box((cx, cy, cw // 2, ch)) or (cx + 4, cy + 9)
V['combo-pad-x'] = '%dpx' % max(0, tb_[0] - cx - int(V['combo-margin'][:-2]) - 1 - PANO_BEARING)
V['combo-pad-top'] = '%dpx' % max(0, tb_[1] - cy - int(V['combo-margin'][:-2]) - 1 - PANO_LABEL_CAP)

# ---- slider -------------------------------------------------------------------
sx, sy, sw, sh = R('slider.slider')


def groove_run(x, y0, y1):
    col = [px(x, y) for y in range(y0, y1)]
    idx = [i for i, c in enumerate(col) if dist(c, bgc(x, y0 + i)) > 3]
    return (y0 + idx[0], col[idx[0]:idx[-1] + 1]) if idx else (y0, [])


def put_groove(prefix, x, rect):
    y_at, run = groove_run(x, rect[1], rect[1] + rect[3])
    if len(run) < 3:
        return y_at
    lum = [sum(c) for c in run]
    a = min(range(0, min(2, len(run))), key=lambda i: lum[i])
    b = max(range(len(run) - min(2, len(run)), len(run)), key=lambda i: -lum[i])
    V[prefix + '-bt'] = hexc(run[a])
    V[prefix + '-bb'] = hexc(run[b])
    V[prefix + '-bg'] = gradient(run[a + 1:b])
    V[prefix + '-height'] = '%dpx' % (b - a + 1)
    V[prefix + '-shadow'] = ('0px 1px 0px 0px %s' % hexc(run[b + 1])) if b + 1 < len(run) else 'none'
    return y_at + a


rr = R('slider.range')
gy = put_groove('groove', sx + sw + 20, rr)
put_groove('slider-fill', sx - 20, rr)
# the groove's side colour and radius: along its middle row from the right end
gx_end = rr[0] + rr[2] - 1
gmid = gy + int(V['groove-height'][:-2]) // 2
# past any relief line: the border is the first clearly dark pixel
while gx_end > rr[0] and dist(px(gx_end, gmid), WIN) <= 20:
    gx_end -= 1
V['groove-bs'] = hexc(px(gx_end, gmid))
gr = 0
while gr < 6 and dist(px(gx_end - gr, gy), unhex(V['groove-bt'])) > 24:
    gr += 1
V['groove-radius'] = '%dpx' % gr
V['groove-inset'] = '%dpx' % (rr[0] + rr[2] - 1 - gx_end)
# the groove's top border row, from the widget's top (whole pixels; centring
# an odd height can land between pixels)
V['groove-top'] = '%dpx' % (gy - R('slider')[1])
tr = capture((sx - 1, rr[1], sw + 2, rr[3]), 'slider-thumb.png', 'thumb', solid=True)
capture((R('slider-hover.slider')[0] - 1, R('slider-hover.range')[1], sw + 2, rr[3]), 'slider-thumb-hover.png', 'thumb-hover', solid=True)
if tr:
    V['thumb-width'] = '%dpx' % tr[2]
    V['thumb-height'] = '%dpx' % tr[3]

# ---- progress bars --------------------------------------------------------------
pe = box(R('progress-empty'))
put_box('trough', pe, with_margin=True)
pr, pe_r = R('progress-full'), R('progress-empty')


def diff_bbox(rect, dx, dy, limit=4):
    """The bounding box of pixels in rect that differ from the same spot
    shifted by (dx, dy) (a reference widget without the part)."""
    x0, y0, w, h = rect
    pts = [(x, y) for y in range(y0, y0 + h) for x in range(x0, x0 + w)
           if dist(px(x, y), px(x + dx, y + dy)) > limit]
    if not pts:
        return None
    xa, ya = min(p[0] for p in pts), min(p[1] for p in pts)
    return (xa, ya, max(p[0] for p in pts) - xa + 1, max(p[1] for p in pts) - ya + 1)


fr_ = diff_bbox(pr, pe_r[0] - pr[0], pe_r[1] - pr[1])
pf = box(fr_, 0)
# the fill sits this far inside the trough's border box (Panorama: padding)
V['pfill-inset'] = '%dpx' % max(0, fr_[1] - pr[1] - pe['margin'] - 1)
V['pfill-bt'], V['pfill-bl'] = hexc(pf['top']), hexc(pf['left'])
V['pfill-br'], V['pfill-bb'] = hexc(pf['right']), hexc(pf['bottom'])
V['pfill-radius'] = '%dpx' % pf['fine_radius']
# stripes are lighter or darker than the fill and cover about half of each
# row: take the side that's closer to the trough's ends (the plain fill)
pfill = base_fill(pf)
lo, hi = base_fill(pf, 0.15), base_fill(pf, 0.85)
if sum(dist(a, b) for a, b in zip(lo, hi)) / max(1, len(lo)) > 12:
    edge = [pf['face'][y][1] for y in range(len(pf['face']))]  # just inside the left border
    pfill = lo if sum(dist(a, e) for a, e in zip(lo, edge)) <= sum(dist(a, e) for a, e in zip(hi, edge)) else hi
V['pfill-bg'] = gradient(pfill)
# Stripes (Murrine's progressbarstyle) repeat along the bar: one period of
# the overlay becomes a tile.
pov = overlay(pf, pfill)
no_image('pfill-image')
V['pfill-repeat'] = 'no-repeat'
if pov is not None:
    a = pov.split()[3]
    w = pov.width

    def mismatch(p):
        d = [abs(a.getpixel((x, y)) - a.getpixel((x + p, y))) for y in range(a.height) for x in range(4, w - p - 4)]
        return sum(d) / max(1, len(d))

    best = min(range(4, min(48, w // 2)), key=mismatch)
    if mismatch(best) < 6:
        x0 = (w // 2 // best) * best  # a whole number of periods in, so the phase matches
        image_var('pfill-image', 'progress-stripes.png', pov.crop((x0, 0, x0 + best, pov.height)))
        V['pfill-image-size'] = '%dpx 100%%' % best
        V['pfill-repeat'] = 'repeat-x'
    else:
        image_var('pfill-image', 'progress-fill.png', pov)
        V['pfill-image-size'] = '100% 100%'
# label colour: the text sits over the fill at 70%
tx, ty, tw, th = R('progress-text')
mid = [(x, y) for y in range(ty + 4, ty + th - 4) for x in range(tx + tw // 2 - 20, tx + tw // 2 + 20)
       if dist(px(x, y, GAL), px(x, y)) > 40]
V['progress-text'] = hexc(min((px(x, y, GAL) for x, y in mid), key=lambda c: -dist(c, px(*mid[0])))) if mid else V['highlighted-text']

# ---- scroll bars ---------------------------------------------------------------
vx, vy, vw, vh = R('scroll-v')
vs = R('scroll-v.slider')
V['scroll-size'] = '%dpx' % vw
# steppers: from the bar's end to where the slider can start
step = vs[1] - vy
V['scroll-button'] = '%dpx' % step
# the trough: across the bar, between the thumb and the down stepper
ty_mid = (vs[1] + vs[3] + vy + vh - step) // 2
tb = box((vx, ty_mid - 10, vw, 20), vertical=False)
# measure the side borders along the row (a horizontal cross-section)
row = [px(x, ty_mid) for x in range(vx, vx + vw)]
idx = [i for i, c in enumerate(row) if dist(c, WIN) > 0]
V['strough-margin'] = '%dpx' % (idx[0] if idx else 0)
# no trough drawn (or none told apart from the window)
V['strough-bs'] = 'rgba( 0, 0, 0, 0 )'
V['strough-bg-v'] = V['strough-bg-h'] = 'none'
V['strough-radius'] = '0px'
if idx:
    i0, i1 = idx[0], idx[-1]
    V['strough-bs'] = hexc(row[i0])
    V['strough-bg-v'] = gradient(row[i0 + 1:i1], vertical=False)
    # the stepper buttons cover the trough's ends
    V['strough-radius'] = '0px'
hx, hy, hw, hh = R('scroll-h')
row = [px(hx + hw // 2 + 40, y) for y in range(hy, hy + hh)]
idx = [i for i, c in enumerate(row) if dist(c, WIN) > 0]
if idx:
    V['strough-bg-h'] = gradient(row[idx[0] + 1:idx[-1]])
# arrow buttons, copied whole (their frames included)
capture((vx, vy, vw, step), 'scroll-up.png', 'scroll-up', trim=False)
capture((vx, vy + vh - step, vw, step), 'scroll-down.png', 'scroll-down', trim=False)
hs = R('scroll-h.slider')
hstep = hs[0] - hx
capture((hx, hy, hstep, hh), 'scroll-left.png', 'scroll-left', trim=False)
capture((hx + hw - hstep, hy, hstep, hh), 'scroll-right.png', 'scroll-right', trim=False)
# The thumb: a framed box, its fill varying across the bar.
# where the thumb is drawn: what differs from the bare trough further down
vtr = diff_bbox(vs, 0, ty_mid - (vs[1] + vs[3] // 2))
vt = box(vtr, 0, vertical=False)
V['sthumb-inset'] = '%dpx' % (vtr[0] - vx - int(V['strough-margin'][:-2]) - 1)

V['sthumb-width'] = '%dpx' % vtr[2]
V['sthumb-length'] = '%dpx' % vtr[3]
# The near end of the thumb can hide under the stepper; its far end shows
# the border and the corner.
tb_y = vtr[1] + vtr[3] - 1
tb_c = px(vtr[0] + vtr[2] // 2, tb_y)
V['sthumb-border'] = hexc(tb_c)
trad = 0
while trad < vtr[2] // 2 and dist(px(vtr[0] + trad, tb_y), tb_c) > 8:
    trad += 1
V['sthumb-radius'] = '%dpx' % trad
if dist(px(vtr[0] + vtr[2] // 2, vtr[1]), tb_c) > 24:
    # no border drawn at the near end: it starts one pixel under the stepper
    V['sthumb-length'] = '%dpx' % (vtr[3] + 1)
    vtr = (vtr[0], vtr[1] - 1, vtr[2], vtr[3] + 1)
    vt = box(vtr, 0, vertical=False)
vfill = base_fill(vt)
V['sthumb-bg'] = gradient(vfill, vertical=False)
ov = overlay(vt, vfill)
if ov is not None:
    image_var('sthumb-image-v', 'scroll-thumb-v.png', ov)
else:
    no_image('sthumb-image-v')
htr = diff_bbox(hs, (hx + hw - hstep - 30) - (hs[0] + hs[2] // 2), 0)
ht = box(htr, 0)
hfill = base_fill(ht)
V['sthumb-bg-h'] = gradient(hfill)
ov = overlay(ht, hfill)
if ov is not None:
    image_var('sthumb-image-h', 'scroll-thumb-h.png', ov)
else:
    no_image('sthumb-image-h')

# ---- group box (GtkFrame) ---------------------------------------------------------
gx, gy0, gw, gh = R('group')
lab = R('group.label')
fx = gx + gw // 2
line_y = next(y for y in range(gy0, gy0 + gh) if dist(px(fx, y), WIN) > 6)
V['frame-bt'] = hexc(px(fx, line_y))
# an etched frame: a second, lighter line inside the first
second = px(fx, line_y + 1)
V['frame-inner'] = ('inset 1px 1px 0px 0px %s' % hexc(second)) if dist(second, WIN) > 6 else 'none'
side_x = next(x for x in range(gx, gx + 8) if dist(px(x, gy0 + gh // 2), WIN) > 6)
V['frame-bl'] = hexc(px(side_x, gy0 + gh // 2))
bot_y = max(y for y in range(gy0, gy0 + gh) if dist(px(fx, y), WIN) > 6)
right_x = max(x for x in range(gx, gx + gw) if dist(px(x, gy0 + gh // 2), WIN) > 6)
V['frame-bb'] = hexc(px(fx, bot_y))
V['frame-br'] = hexc(px(right_x, gy0 + gh // 2))
V['frame-shadow'] = 'none'
V['frame-shrink'] = '0px'
inner_r, inner_b = px(right_x - 1, gy0 + gh // 2), px(fx, bot_y - 1)
if sum(unhex(V['frame-br'])) > sum(WIN) and sum(inner_r) < sum(WIN) - 30:
    # etched (GTK's SHADOW_ETCHED_IN): a dark frame and a light one moved
    # one pixel right and down. Panorama: the dark frame, one pixel smaller,
    # with the light one as an offset shadow.
    V['frame-shadow'] = '1px 1px 0px 0px %s' % V['frame-br']
    V['frame-br'], V['frame-bb'] = hexc(inner_r), hexc(inner_b)
    V['frame-shrink'] = '1px'
    V['frame-inner'] = 'none'
fr = 0
while fr < 8 and dist(px(side_x + fr, line_y), unhex(V['frame-bt'])) > 24:
    fr += 1
V['frame-radius'] = '%dpx' % fr
V['frame-bg'] = hexc(px(fx, (line_y + bot_y) // 2))
V['group-title-x'] = '%dpx' % (lab[0] - side_x)
V['group-title-height'] = '%dpx' % lab[3]
V['group-title-gap'] = '%dpx' % (line_y - (lab[1] + lab[3]))
V['group-title-weight'] = 'bold' if ink_count(lab) > ink_count(R('label')) * 1.25 else 'normal'
V['group-x'] = '%dpx' % (side_x - gx)
if V['frame-shadow'] == 'none':
    V['frame-shadow'] = V['frame-inner']
del V['frame-inner']
# where the frame puts its child, from the frame's top-left line
gc = R('group-child')
V['frame-pad-top'] = '%dpx' % max(0, gc[1] - line_y - 1)
V['frame-pad-left'] = '%dpx' % max(0, gc[0] - side_x - 1)

# ---- notebook ---------------------------------------------------------------------
nx, ny, nw, nh = R('tabs')
page = R('tabs.page')
labels = [R('tabs.label%d' % i) for i in range(3)]


def tab_extent(lab):
    """A tab's drawn box around its label: left and right edges along a row
    just under the tab's top, and the top edge."""
    yrow = lab[1] + lab[3] // 2
    xl = lab[0]
    while xl > nx and dist(px(xl, yrow), px(lab[0] - 1, yrow)) <= 24:
        xl -= 1
    xr = lab[0] + lab[2] - 1
    while xr < nx + nw - 1 and dist(px(xr, yrow), px(lab[0] + lab[2], yrow)) <= 24:
        xr += 1
    xm = lab[0] + lab[2] // 2
    yt = ny
    while yt < lab[1] and dist(px(xm, yt), WIN) <= 6:
        yt += 1
    return xl, xr, yt


pane_line = None
xm = page[0] + page[2] - 20
for y in range(ny, page[1] + 1):
    if dist(px(xm, y), WIN) > 6:
        pane_line = y
        break
V['pane-border'] = hexc(px(xm, pane_line))
V['pane-bg'] = hexc(px(page[0] + page[2] // 2, page[1] + page[3] // 2))
for name, lab in (('tab-active', labels[0]), ('tab', labels[1])):
    xl, xr, yt = tab_extent(lab)
    bottom = pane_line if name == 'tab' else pane_line + 1
    b = box((xl, yt, xr - xl + 1, bottom - yt), 0)
    V[name + '-top'] = '%dpx' % (yt - ny)
    V[name + '-height'] = '%dpx' % (pane_line - yt + (1 if name == 'tab-active' else 0))
    V[name + '-border'] = hexc(b['top'])
    V[name + '-bs'] = hexc(b['left'])
    rad = 0
    while rad < 6 and dist(px(xl + rad, yt), b['top']) > 24:
        rad += 1
    V[name + '-radius'] = '%dpx' % rad
    # fill: down a column clear of the label and the corner
    col = [px(xl + rad + 1, y) for y in range(yt + 1, bottom)]
    V[name + '-bg'] = gradient(col)
    V[name + '-padding'] = '%dpx' % max(0, lab[0] - xl - 1)
    V[name + '-padding-r'] = '%dpx' % max(0, xr - (lab[0] + lab[2]))
    if name == 'tab-active':
        V['tab-bar-x'] = '%dpx' % (xl - nx)
        first_r = xr
    else:
        V['tab-gap'] = '%dpx' % max(-1, xl - first_r - 1)
V['tab-bar-height'] = '%dpx' % (pane_line - ny)

# ---- output -------------------------------------------------------------------------
selector = '.Murrine' + ('.' + args.name if args.name else '')
src = os.path.relpath(args.gtkrc, REPO) if os.path.abspath(args.gtkrc).startswith(REPO) else os.path.basename(args.gtkrc)
lines = ['/* Generated by tools/gtkref/gtkrc2panorama.py from %s. */' % src, selector, '{']
for k, v in V.items():
    lines.append('\t--%s: %s;' % (k, v))
lines.append('}')
text = '\n'.join(lines) + '\n'
BEGIN, END = '/* BEGIN gtkrc2panorama */', '/* END gtkrc2panorama */'
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
