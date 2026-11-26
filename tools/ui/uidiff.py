#!/usr/bin/env python3
# Bulwark - compare two UI screenshots region by region. ISC licence, see LICENSE.
#
# Made for the native UI (src/ui/uin_*.c), whose target is the PC Panorama
# build's drawing of the same page. For each named region it prints the mean
# absolute error (0-255, RGB averaged) and the bounding box of the "ink" in
# each image (pixels that differ from the background by more than a
# threshold), and whether the boxes agree within a tolerance. The exit code
# is 1 when any region fails, so a script can gate on it.
#
#   uidiff.py ref.png test.png                  HUD regions at the image size
#   uidiff.py ref.png test.png --safe 0.05      HUD regions inside a TV-safe inset
#   uidiff.py ref.png test.png --region name:x,y,w,h[,thresh] ...   your own (screen px)
#   uidiff.py ref.png test.png --diff out.png   also write |ref - test| x4
#
# The HUD regions are in Stiletto's 1440x1080 HUD canvas (data/web/hud/app.js
# HUD_LAYOUT and its drawers) and are mapped as the HUD maps them: centred
# clusters about the centre of the rectangle, the resource panel from its
# left edge, all scaled by min(w / 1440, h / 1080).
#
# HUD recipe (both builds at 640x480, ui_native_safe 0), with
# tools/ui/hud_fixture.cfg copied into the writable game dir:
#   bulwark -basedir B -game base -game stbuilt -width 640 -height 480
#     +exec hud_fixture.cfg +set r_drawworld 0 +set r_drawentities 0
#     +in 3 "ui_close all" +in 3.2 "ui_open layout/web/hud.xml"
#     +in 5 "screenshot NAME" +in 5.5 quit
# then uidiff.py stbuilt/screenshots/ref.png stbuilt/screenshots/native.png

import argparse
import sys

import numpy as np
from PIL import Image

# name: (x, y, w, h) in the 1440x1080 canvas, whether it hangs off the left
# edge (the resource panel) rather than the centre, and the ink threshold
# (None: --thresh). Regions leave a margin around their piece, so its ink
# box isn't cut by the region; a light threshold picks text off its plate.
HUD_REGIONS = [
	('speed', (610, 880, 220, 90), False, None),         # app.js:365-376
	('timeplate', (580, 5, 280, 190), False, None),      # app.js:416, holds the timer
	('timer', (640, 95, 160, 50), False, 160),           # app.js:419-426, white on the plate
	('score_l', (500, 55, 160, 120), False, None),       # app.js:428-437
	('score_r', (790, 55, 160, 120), False, None),
	('name_l', (482, 152, 160, 35), False, 220),         # app.js:438-458, white on the plate
	('name_r', (803, 152, 161, 35), False, 220),         # app.js:459-472
	('minipips', (790, 182, 90, 42), False, None),       # app.js:474-487
	('heart', (670, 570, 95, 95), False, None),          # app.js:493-525
	('clip', (725, 545, 120, 75), False, None),          # app.js:542-552, with the reserve
	('pips', (630, 555, 80, 75), False, None),           # app.js:553-575
	('explate', (55, 945, 115, 55), True, None),         # app.js:571-574
	('gauge', (50, 905, 510, 65), True, None),           # app.js:575-579, EX count and MAX on it
	('excount', (240, 860, 130, 150), True, 230),        # app.js:582-587, white over the yellow gauge
	('max', (410, 880, 120, 70), True, 230),             # app.js:588-591
	# whole clusters (HUD_LAYOUT, app.js:12-19), the ring left out
	('matchHeader', (342, 0, 756, 317), False, None),
	('centerStatus', (520, 360, 400, 360), False, None),
	('resourcePnl', (0, 820, 600, 200), True, None),
]


def hud_rects(w, h, safe):
	x0, y0 = w * safe, h * safe
	sw, sh = w - 2 * x0, h - 2 * y0
	s = min(sw / 1440.0, sh / 1080.0)
	cx, cy = x0 + sw / 2, y0 + sh / 2
	out = []
	for name, (x, y, rw, rh), left, thresh in HUD_REGIONS:
		px = x0 + x * s if left else cx + (x - 720) * s
		py = cy + (y - 540) * s
		out.append((name, int(px), int(py), int(round(rw * s + 0.5)), int(round(rh * s + 0.5)), thresh))
	return out


def background(img):
	# the most common colour: the clear colour behind a HUD
	flat = img.reshape(-1, 3)
	vals, counts = np.unique(flat, axis=0, return_counts=True)
	return vals[counts.argmax()].astype(np.int32)


def ink_box(img, bg, thresh):
	if thresh >= 128:
		# light ink on a plate: by brightness, not by distance from the background
		d = img.astype(np.int32).min(axis=2)
		ys, xs = np.nonzero(d > thresh)
		if len(xs) == 0:
			return None
		return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
	d = np.abs(img.astype(np.int32) - bg).max(axis=2)
	ys, xs = np.nonzero(d > thresh)
	if len(xs) == 0:
		return None
	return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def main():
	ap = argparse.ArgumentParser(description='Per-region mean error and ink boxes of two screenshots')
	ap.add_argument('ref')
	ap.add_argument('test')
	ap.add_argument('--safe', type=float, default=0.0, help='TV-safe inset the HUD was drawn in (ui_native_safe)')
	ap.add_argument('--region', action='append', default=[], help='name:x,y,w,h[,thresh] in screen pixels (replaces the HUD set)')
	ap.add_argument('--mae', type=float, default=4.0, help='largest mean absolute error that passes (0-255)')
	ap.add_argument('--box', type=int, default=1, help='largest ink-box edge difference that passes (px)')
	ap.add_argument('--thresh', type=int, default=40, help='difference from the background that counts as ink')
	ap.add_argument('--diff', help='write the difference, amplified 4x, to this PNG')
	a = ap.parse_args()

	ref = np.asarray(Image.open(a.ref).convert('RGB'))
	test = np.asarray(Image.open(a.test).convert('RGB'))
	if ref.shape != test.shape:
		sys.exit('size differs: %s vs %s' % (ref.shape[1::-1], test.shape[1::-1]))
	h, w = ref.shape[:2]
	if a.region:
		rects = []
		for r in a.region:
			name, nums = r.split(':', 1)
			v = [int(float(n)) for n in nums.split(',')]
			rects.append((name, v[0], v[1], v[2], v[3], v[4] if len(v) > 4 else None))
	else:
		rects = hud_rects(w, h, a.safe)
	bg_r, bg_t = background(ref), background(test)

	failed = 0
	print('%-13s %-17s %7s  %-19s %-19s %s' % ('region', 'rect', 'mae', 'ink ref', 'ink test', 'result'))
	for name, x, y, rw, rh, thresh in rects:
		thresh = a.thresh if thresh is None else thresh
		x, y = max(0, x), max(0, y)
		rw, rh = min(rw, w - x), min(rh, h - y)
		if rw <= 0 or rh <= 0:
			continue
		cr = ref[y:y + rh, x:x + rw]
		ct = test[y:y + rh, x:x + rw]
		mae = np.abs(cr.astype(np.int32) - ct.astype(np.int32)).mean()
		br, bt = ink_box(cr, bg_r, thresh), ink_box(ct, bg_t, thresh)
		if br and bt:
			edge = max(abs(p - q) for p, q in zip(br, bt))
			box_ok = edge <= a.box
		else:
			edge = None
			box_ok = br == bt
		ok = mae <= a.mae and box_ok
		failed += not ok

		def fmt(b):
			return '-' if not b else '%d,%d-%d,%d' % (b[0] + x, b[1] + y, b[2] + x, b[3] + y)

		why = []
		if mae > a.mae:
			why.append('mae')
		if not box_ok:
			why.append('box %s' % ('missing' if edge is None else '%dpx' % edge))
		print('%-13s %-17s %7.2f  %-19s %-19s %s' % (name, '%d,%d %dx%d' % (x, y, rw, rh), mae, fmt(br), fmt(bt),
		                                           'ok' if ok else 'FAIL ' + ' '.join(why)))
	whole = np.abs(ref.astype(np.int32) - test.astype(np.int32)).mean()
	print('whole image mae %.2f; %d of %d regions fail' % (whole, failed, len(rects)))
	if a.diff:
		d = np.clip(np.abs(ref.astype(np.int32) - test.astype(np.int32)) * 4, 0, 255).astype(np.uint8)
		Image.fromarray(d).save(a.diff)
	return 1 if failed else 0


if __name__ == '__main__':
	sys.exit(main())
