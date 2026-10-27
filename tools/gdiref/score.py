#!/usr/bin/env python3
"""Score several gdidump variants against one Windows capture, compactly.

usage: score.py <windows_dir> <label>=<dir> [...]

For each variant, prints one row per face and weight with the identical
fraction of: ABC (q0, q1), GGO_BITMAP (q0, q1), GGO_GRAY8, ExtTextOut
(q0, q1), plus the same over heights 8-10 only ("small"). Before that, it
shows how Windows' quality 1 output relates to its own quality 0 output,
split by whether gasp made the quality 1 text greyscale.
"""
import base64, json, os, sys
from collections import defaultdict

def load(d):
    gl = {}
    for line in open(os.path.join(d, "glyphs.jsonl"), encoding="utf-8"):
        g = json.loads(line)
        m, g8, e = g["mono"], g["gray8"], g["eto"]
        gl[(g["face"], g["weight"], g["tall"], g["aa"], g["cp"])] = (
            (g["a"], g["b"], g["c"]), (m.get("data"), m["ox"], m["oy"]), (g8.get("data"), g8["ox"], g8["oy"]),
            tuple(e.get(f) for f in ("x", "y", "w", "h", "rgb")))
    return gl

def grey_configs(d):
    """(face, weight, tall) whose quality 1 ExtTextOut has levels other than 0/255"""
    out = set()
    for line in open(os.path.join(d, "glyphs.jsonl"), encoding="utf-8"):
        g = json.loads(line)
        if g["aa"] == 1 and "rgb" in g["eto"] and set(base64.b64decode(g["eto"]["rgb"])[0::3]) - {0, 255}:
            out.add((g["face"], g["weight"], g["tall"]))
    return out

FIELDS = [("abc0", 0, 0), ("abc1", 0, 1), ("mono0", 1, 0), ("mono1", 1, 1), ("gray8", 2, 1), ("eto0", 3, 0), ("eto1", 3, 1)]

def main():
    wd = sys.argv[1]
    win = load(wd)
    grey = grey_configs(wd)

    print("== Windows quality 1 vs its own quality 0 (same glyph) ==")
    st = defaultdict(lambda: [0, 0, 0])
    for k, v in win.items():
        if k[3] != 1:
            continue
        z = win.get(k[:3] + (0, k[4]))
        if not z:
            continue
        s = st[(k[0], (k[0], k[1], k[2]) in grey)]
        s[0] += 1
        s[1] += v[0] == z[0]
        s[2] += v[1] == z[1]
    for (face, g), s in sorted(st.items()):
        print(f"  {face:16s} {'grey text' if g else '1-bit text':10s} n={s[0]:5d}  ABC same {100*s[1]/s[0]:5.1f}%  GGO_BITMAP same {100*s[2]/s[0]:5.1f}%")

    for arg in sys.argv[2:]:
        label, d = arg.split("=", 1)
        cand = load(d)
        agg = defaultdict(lambda: defaultdict(lambda: [0, 0]))
        for k, v in win.items():
            c = cand.get(k)
            if not c:
                continue
            face, weight, tall, aa, cp = k
            for name, idx, want in FIELDS:
                if aa != want:
                    continue
                eq = v[idx] == c[idx]
                for key in ((face, weight), ("ALL", 0)):
                    a = agg[key][name]; a[0] += 1; a[1] += eq
                    if tall <= 10:
                        a = agg[key]["s_" + name]; a[0] += 1; a[1] += eq
        print(f"== {label} ==")
        names = [n for n, _, _ in FIELDS]
        print("  " + f"{'face':22s}" + "".join(f"{n:>7s}" for n in names) + " | small:" + "".join(f"{n:>7s}" for n in names))
        for key in sorted(agg, key=lambda k: (k[0] == "ALL", k)):
            a = agg[key]
            pct = lambda n: f"{100*a[n][1]/a[n][0]:7.1f}" if a[n][0] else "      -"
            print(f"  {key[0] + ' ' + str(key[1]):22s}" + "".join(pct(n) for n in names) + " |       " + "".join(pct("s_" + n) for n in names))

if __name__ == "__main__":
    main()
