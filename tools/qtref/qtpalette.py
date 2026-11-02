# Reads a Qt palette file (Role=#rrggbb lines, optionally Group:Role=...)
# into a QPalette. Used by gallery.py and qtcurve2panorama.py --palette.
from PyQt5.QtGui import QColor, QPalette

GROUPS = {'active': QPalette.Active, 'inactive': QPalette.Inactive, 'disabled': QPalette.Disabled}


def load_palette(path, base):
    pal = QPalette(base)
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, val = (t.strip() for t in line.split('=', 1))
        grp, _, role = key.rpartition(':')
        groups = [GROUPS[grp.lower()]] if grp else list(GROUPS.values())
        for g in groups:
            pal.setColor(g, getattr(QPalette, role), QColor(val))
    return pal
