#!/usr/bin/env python3
"""Copy Xelu's controller prompts into data/panorama/images/glyphs/<set>/<button>.png.

Which file of Xelu's pack shows which button of which set is read from the
table in src/client/steaminput.c, so the engine, the launcher and these
images agree.

    python3 tools/steaminput/import_xelu.py <unzipped Xelu pack> [out dir]

The pack: https://thoseawesomeguys.com/prompts/ (CC0).
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

def table():
    src = open(os.path.join(REPO, 'src/client/steaminput.c')).read()
    body = src[src.index('} xelu[SI_SET_COUNT] = {'):]
    body = body[:body.index('\n};')]
    sets = {}
    for m in re.finditer(r'\[SI_SET_(\w+)\] = \{"([^"]+)",\s*\{(.*?)\}\}', body, re.S):
        files = dict(re.findall(r'\[SI_BTN_(\w+)\] = "([^"]+)"', m.group(3)))
        sets[m.group(1).lower()] = (m.group(2), files)
    return sets

def main():
    pack = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, 'data/panorama/images/glyphs')
    n = 0
    for name, (prefix, files) in table().items():
        os.makedirs(os.path.join(out, name), exist_ok=True)
        for btn, f in files.items():
            src = os.path.join(pack, prefix + f + '.png')
            if not os.path.exists(src):
                sys.exit('missing in the pack: ' + src)
            shutil.copyfile(src, os.path.join(out, name, btn.lower() + '.png'))
            n += 1
    print('%d glyphs in %d sets into %s' % (n, len(table()), out))

if __name__ == '__main__':
    main()
