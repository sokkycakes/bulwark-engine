#!/usr/bin/env python3
"""Measure WolfTeam's widget textures and print CSS custom properties for
styles/wt/wolfteam.css (the block between BEGIN/END measure-wt).

The textures are WolfTeam's own art (WT_Interface/*.dtx inside wolf.xfs,
converted to PNG) and never go in git; point this at the folder that holds
the converted wt_interface tree:

    python3 tools/wtui/measure-wt.py /path/to/wt_interface > stops.css

Every WolfTeam button, header and tab face is a plain vertical gradient with
the same value across its whole width, so a column of pixels is the whole
look; the script samples one column per state and emits a Panorama
gradient( linear ... color-stop ... ) with a stop every 2 pixels.
"""
import os
import sys

from PIL import Image

root = sys.argv[1]


def column(path, x, y0, y1, step=2):
    im = Image.open(os.path.join(root, path)).convert('RGBA')
    pts = []
    for y in range(y0, y1, step):
        pts.append(im.getpixel((x, y)))
    pts.append(im.getpixel((x, y1 - 1)))
    return pts


def gradient(pts):
    n = len(pts) - 1
    parts = ['gradient( linear, 0%% 0%%, 0%% 100%%, from( %s )' % hexa(pts[0])]
    for i in range(1, n):
        parts.append('color-stop( %.3f, %s )' % (i / n, hexa(pts[i])))
    parts.append('to( %s ) )' % hexa(pts[n]))
    return ', '.join(parts)


def hexa(p):
    if p[3] == 255:
        return '#%02x%02x%02x' % p[:3]
    return 'rgba( %d, %d, %d, %.3f )' % (p[0], p[1], p[2], p[3] / 255.0)


def prop(name, path, x, y0, y1, step=2):
    print('\t%s: %s;' % (name, gradient(column(path, x, y0, y1, step))))


# a_button_*.dtx: 90x28 faces stacked default / over / click / disable
prop('--wt-btn-bg', 'all/a_button_ok.png', 2, 0, 28)
prop('--wt-btn-hover-bg', 'all/a_button_ok.png', 2, 29, 57)
prop('--wt-btn-active-bg', 'all/a_button_ok.png', 2, 58, 86)
prop('--wt-btn-disabled-bg', 'all/a_button_ok.png', 2, 87, 115)
# lb_cate_*.dtx: list column headers, 22px, default / hover
prop('--wt-header-bg', 'lobby/lb_cate_roomname.png', 3, 0, 22)
prop('--wt-header-hover-bg', 'lobby/lb_cate_roomname.png', 3, 23, 45)
# a_scroll.dtx: the scroll thumb (13x22)
prop('--wt-scroll-thumb-bg', 'lobby/a_scroll.png', 6, 0, 22)
# mb_popup_bg.dtx: the title bar of a dialog (24px, then a #363636 line)
prop('--wt-title-bg', 'messagebox/mb_popup_bg.png', 40, 0, 24, 1)
# lb_ch_tab.dtx: glass tabs, hover (lit) and checked faces, 18px
prop('--wt-gtab-hover-bg', 'lobby/lb_ch_tab.png', 3, 19, 37, 1)
prop('--wt-gtab-checked-bg', 'lobby/lb_ch_tab.png', 3, 38, 56, 1)
# is_inven_weponbar.dtx: item card top shading (first 12px)
prop('--wt-card-top-bg', 'inven_shop/is_inven_weponbar.png', 100, 1, 13, 1)
# a_icon_close.dtx: 32px glass icon button, grey and red faces
prop('--wt-icon-bg', 'all/a_icon_close.png', 3, 1, 31)
prop('--wt-icon-hover-bg', 'all/a_icon_close.png', 35, 1, 31)
