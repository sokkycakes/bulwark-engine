#!/usr/bin/env python3
"""Print the details behind compare.py's percentages, for tuning gdifont.

usage: diagnose.py <windows_dir> <candidate_dir>

Sections:
  1. Does Windows' own 1-bit ExtTextOut equal its GGO_BITMAP?
  2. ExtTextOut q0 mismatches (candidate vs Windows), by kind and offset
  3. Size choice: ppem (tmHeight - tmInternalLeading) per config where the
     sides disagree, with the VDMX entries around it
  4. tm metrics other than height: which differ, with examples
  5. ABC mismatches by kind
  6. Greyscale: Windows ExtTextOut levels, how they relate to Windows'
     GGO_GRAY8, and candidate vs Windows GGO_GRAY8 value pairs
  7. 1-bit misses by size
  8. The space glyph as each side reports it
"""
import base64, json, os, struct, sys
from collections import Counter, defaultdict

def load(d):
    cfg, gl = {}, {}
    for line in open(os.path.join(d, "configs.jsonl"), encoding="utf-8"):
        c = json.loads(line)
        cfg[(c["face"], c["tall"], c["weight"], c["aa"])] = c
    for line in open(os.path.join(d, "glyphs.jsonl"), encoding="utf-8"):
        g = json.loads(line)
        gl[(g["face"], g["tall"], g["weight"], g["aa"], g["cp"])] = g
    return cfg, gl

def mono_px(m):
    """{(x, y)} of set pixels in a GGO_BITMAP record"""
    if "data" not in m:
        return set()
    d = base64.b64decode(m["data"])
    pitch = ((m["bbx"] + 31) // 32) * 4
    return {(x, y) for y in range(m["bby"]) for x in range(m["bbx"]) if d[y * pitch + (x >> 3)] & (0x80 >> (x & 7))}

def gray_px(m):
    if "data" not in m:
        return {}
    d = base64.b64decode(m["data"])
    pitch = (m["bbx"] + 3) & ~3
    return {(x, y): d[y * pitch + x] for y in range(m["bby"]) for x in range(m["bbx"]) if d[y * pitch + x]}

def eto_px(e):
    if "rgb" not in e:
        return {}
    d = base64.b64decode(e["rgb"])
    return {(e["x"] + x, e["y"] + y): d[(y * e["w"] + x) * 3] for y in range(e["h"]) for x in range(e["w"]) if d[(y * e["w"] + x) * 3]}

def vdmx_entries(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    n = struct.unpack(">H", data[4:6])[0]
    tabs = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        tabs[tag] = data[off:off + ln]
    v = tabs.get(b"VDMX")
    if not v:
        return None
    _, _, nrat = struct.unpack(">HHH", v[:6])
    ratios = [struct.unpack(">BBBB", v[6 + 4 * i:10 + 4 * i]) for i in range(nrat)]
    for i, (cs, x, y0, y1) in enumerate(ratios):
        if cs == 1 and x == 1 and y0 <= 1 <= y1:
            off = struct.unpack(">H", v[6 + 4 * nrat + 2 * i:8 + 4 * nrat + 2 * i])[0]
            recs = struct.unpack(">H", v[off:off + 2])[0]
            return {struct.unpack(">H", v[off + 4 + 6 * j:off + 6 + 6 * j])[0]:
                    struct.unpack(">hh", v[off + 6 + 6 * j:off + 10 + 6 * j]) for j in range(recs)}
    return None

def main():
    wd, cd = sys.argv[1], sys.argv[2]
    wcfg, wgl = load(wd)
    ccfg, cgl = load(cd)
    P = print

    P("== 1. Windows ExtTextOut q0 vs Windows GGO_BITMAP ==")
    same = Counter(); tot = Counter()
    for k, g in wgl.items():
        if k[3] != 0:
            continue
        c = wcfg[k[:4]]
        m = mono_px(g["mono"])
        placed = {(x + g["mono"]["ox"] - g["a"], y + c["tmAscent"] - g["mono"]["oy"]) for x, y in m}
        e = set(eto_px(g["eto"]))
        tot[k[0]] += 1
        same[k[0]] += placed == e
    for f in sorted(tot):
        P(f"  {f}: {same[f]}/{tot[f]} identical")

    P("== 2. ExtTextOut q0 mismatches, candidate vs Windows ==")
    kinds = defaultdict(Counter); offs = defaultdict(Counter)
    for k, w in wgl.items():
        c = cgl.get(k)
        if not c or k[3] != 0:
            continue
        we, ce = w["eto"], c["eto"]
        if all(we.get(f) == ce.get(f) for f in ("x", "y", "w", "h", "rgb")):
            kinds[k[0]]["equal"] += 1
        elif we.get("rgb") == ce.get("rgb") and (we.get("w"), we.get("h")) == (ce.get("w"), ce.get("h")):
            kinds[k[0]]["same ink, moved"] += 1
            offs[k[0]][(ce.get("x", 0) - we.get("x", 0), ce.get("y", 0) - we.get("y", 0))] += 1
        else:
            gm = (w["mono"].get("data"), w["mono"]["ox"], w["mono"]["oy"]) == (c["mono"].get("data"), c["mono"]["ox"], c["mono"]["oy"])
            kinds[k[0]]["other, GGO_BITMAP equal" if gm else "other, GGO_BITMAP differs too"] += 1
    for f in sorted(kinds):
        P(f"  {f}: {dict(kinds[f])}  moved by (dx,dy): {dict(offs[f].most_common(6))}")

    P("== 3. Size choice where the sides disagree (ppem = tmHeight - tmInternalLeading) ==")
    for k in sorted(wcfg):
        if k[3] != 0 or k not in ccfg:
            continue
        w, c = wcfg[k], ccfg[k]
        wp, cp = w["tmHeight"] - w["tmInternalLeading"], c["tmHeight"] - c["tmInternalLeading"]
        if (wp, w["tmHeight"], w["tmAscent"]) != (cp, c["tmHeight"], c["tmAscent"]):
            v = vdmx_entries(os.path.join(wd, "fonts", f"{k[0]}_{k[2]}.ttf"))
            near = {p: v[p] for p in range(min(wp, cp) - 2, max(wp, cp) + 3) if v and p in v}
            P(f"  {k[:3]}: windows ppem {wp} h {w['tmHeight']} asc {w['tmAscent']} | candidate ppem {cp} h {c['tmHeight']} asc {c['tmAscent']} | VDMX {near}")

    P("== 4. Other tm metrics ==")
    for m in ("tmMaxCharWidth", "tmAveCharWidth", "tmExternalLeading", "tmOverhang", "tmDescent"):
        bad = defaultdict(list); n = Counter()
        for k in sorted(wcfg):
            if k[3] != 0 or k not in ccfg:
                continue
            n[k[0]] += 1
            if wcfg[k][m] != ccfg[k].get(m):
                bad[k[0]].append(f"{k[1]}/{k[2]}:{wcfg[k][m]}vs{ccfg[k].get(m)}")
        P(f"  {m}: " + "; ".join(f"{f} {n[f]-len(bad[f])}/{n[f]} ok {' '.join(bad[f][:6])}" for f in sorted(n)))

    P("== 5. ABC mismatches ==")
    ak = defaultdict(Counter); ex = defaultdict(list)
    for k, w in wgl.items():
        c = cgl.get(k)
        if not c or k[3] != 0:
            continue
        d = tuple(int(c[f] != w[f]) for f in "abc")
        if any(d):
            ak[k[0]]["".join(f for f, x in zip("ABC", d) if x)] += 1
            if len(ex[k[0]]) < 5:
                ex[k[0]].append(f"{k[1]}/{k[2]} U+{k[4]:04X} win {w['a']},{w['b']},{w['c']} cand {c['a']},{c['b']},{c['c']}")
    for f in sorted(ak):
        P(f"  {f}: {dict(ak[f])}  e.g. {'; '.join(ex[f])}")

    P("== 6. Greyscale ==")
    lv = Counter(); w2e = defaultdict(Counter); pairs = defaultdict(Counter); ggeo = Counter()
    for k, w in wgl.items():
        if k[3] != 1:
            continue
        e = eto_px(w["eto"])
        vals = set(e.values())
        if len(vals - {255}) == 0:
            continue
        lv.update(e.values())
        c = wcfg[k[:4]]
        g = gray_px(w["gray8"])
        for (x, y), v in g.items():
            p = (x + w["gray8"]["ox"] - w["a"], y + c["tmAscent"] - w["gray8"]["oy"])
            w2e[v][e.get(p, 0)] += 1
    P("  Windows grey ExtTextOut levels (value: count): " + str(sorted(lv.items())))
    P("  Windows GGO_GRAY8 value -> Windows grey ExtTextOut value at the same pixel (top 3):")
    P("   " + "; ".join(f"{v}->" + ",".join(f"{a}x{n}" for a, n in w2e[v].most_common(3)) for v in sorted(w2e)))
    for k, w in wgl.items():
        c = cgl.get(k)
        if not c or k[3] != 1:
            continue
        wg, cg = w["gray8"], c["gray8"]
        geo = (wg["bbx"], wg["bby"], wg["ox"], wg["oy"]) == (cg["bbx"], cg["bby"], cg["ox"], cg["oy"])
        ggeo[k[0]] += geo
        if geo:
            a, b = gray_px(wg), gray_px(cg)
            for p in set(a) | set(b):
                pairs[k[0]][(b.get(p, 0), a.get(p, 0))] += 1
    P("  GGO_GRAY8 glyphs with the same box and origin: " + str(dict(ggeo)))
    for f in sorted(pairs):
        tot = sum(pairs[f].values()); eq = sum(n for (a, b), n in pairs[f].items() if a == b)
        top = [x for x in pairs[f].most_common(60) if x[0][0] != x[0][1]][:12]
        P(f"  {f}: pixels equal {eq}/{tot}; candidate->windows (top unequal): " +
          ", ".join(f"{a}->{b}x{n}" for (a, b), n in top))

    P("== 7. 1-bit (GGO_BITMAP) equal per size, all faces ==")
    sz = defaultdict(lambda: [0, 0])
    for k, w in wgl.items():
        c = cgl.get(k)
        if not c or k[3] != 0:
            continue
        s = sz[(k[0], k[2], k[1])]
        s[0] += 1
        s[1] += (w["mono"].get("data"), w["mono"]["ox"], w["mono"]["oy"]) == (c["mono"].get("data"), c["mono"]["ox"], c["mono"]["oy"])
    for f in sorted({k[:2] for k in sz}):
        P(f"  {f[0]} {f[1]}: " + " ".join(f"{t}:{sz[(f[0], f[1], t)][1]}" for t in range(8, 25) if (f[0], f[1], t) in sz))

    P("== 8. Space (U+0020) ==")
    for k in [("Tahoma", 12, 400, 0, 32), ("Arial", 16, 400, 1, 32)]:
        for name, gl in (("windows", wgl), ("candidate", cgl)):
            g = gl.get(k)
            if g:
                P(f"  {name} {k}: abc {g['a']},{g['b']},{g['c']} mono {g['mono']} gray8 {g['gray8']} eto {g['eto']}")

if __name__ == "__main__":
    main()
