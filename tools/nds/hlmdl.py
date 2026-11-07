#!/usr/bin/env python3
# Bulwark engine - bakes a GoldSrc studio model for the DS: fewer triangles,
# a few frames of the clips that matter, one texture, written as a Quake
# model (IDPO 6) that models_nds.c draws, with a trailer that maps the
# model's sequences to the baked clips and carries the texture's palette.
# ISC licence, see LICENSE.
#
#   hlmdldump <model.mdl> <x.dump>                       (tools/nds/hlmdldump.c)
#   hlmdl.py <x.dump> <out.mdl> [-tris 200] [-frames 72] [-skin 128x64] [-view] [-mirror]
#
# -mirror swaps left and right, for view models a weapon def marks
# "model_viewMirror" (left-handed models, such as CSO's).
#
# -view is for a weapon's view model (v_*.mdl): every sequence gets a clip,
# since the game plays them by number, and the arms and hands are left out
# (textures named like ARMS below), as are the additive effect meshes
# ($0b... and the like), which the DS would draw solid.
#
# Decimation collapses edges onto one of their two vertices, so every kept
# vertex is an original one and every frame stays exact for what is left.
# The cost of a collapse is the quadric error summed over a spread of frames
# of every clip, so parts that bend (knees, elbows) keep their vertices.
#
# Trailer, at the end of the file: "DSHL", u16 numclips, numclips x {u16
# first frame, u8 frames, u8 fps, u8 flags (1 loops), u8 activity}, u16
# numseqs, numseqs x u8 clip, 768 bytes of palette, then u32 offset of
# "DSHL" from the start of the file and "DSHL" again.
#
# The output is the game's model cut down: keep it out of git and packages.
import re
import struct
import sys

import numpy as np
from PIL import Image

# the GoldSrc activities worth a clip, most wanted first: idle, walk, run,
# attacks, flinches, deaths (numbers as the game's sequences store them)
WANTED = [1, 3, 4, 28, 30, 29, 31, 26, 27, 36, 37, 38, 39, 17, 18, 5, 6, 8, 9]

# view model textures that are the player's arms and hands (Half-Life's
# gloves, sleeves and forearms), and effect meshes
ARMS = re.compile(r"glove|sleeve|forearm|cuff|knuckle|handpak|hand_", re.I)
EFFECTS = re.compile(r"^\$0b|spark|(^|_)ef(\d|_|\.)", re.I)


def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"HLD1", "not a hlmdldump file"
    nv, nt, ntex, nseq = struct.unpack_from("<4I", d, 4)
    o = 20
    texes = []
    for _ in range(ntex):
        name = d[o:o + 64].split(b"\0")[0].decode("latin1")
        w, h = struct.unpack_from("<II", d, o + 64)
        o += 72
        pix = np.frombuffer(d, np.uint8, w * h, o).reshape(h, w)
        o += w * h
        pal = np.frombuffer(d, np.uint8, 768, o).reshape(256, 3)
        o += 768
        texes.append((name, w, h, pix, pal))
    tris = []
    for _ in range(nt):
        c = struct.unpack_from("<IffIffIffI", d, o)
        o += 40
        tris.append(([c[0], c[3], c[6]], [(c[1], c[2]), (c[4], c[5]), (c[7], c[8])], c[9]))
    seqs = []
    for _ in range(nseq):
        label = d[o:o + 32].split(b"\0")[0].decode("latin1")
        fps, flags, act, nf = struct.unpack_from("<fIII", d, o + 32)
        o += 48
        fr = np.frombuffer(d, np.float32, nf * nv * 3, o).reshape(nf, nv, 3)
        o += nf * nv * 12
        seqs.append(dict(label=label, fps=fps, flags=flags, act=act, frames=fr))
    return nv, tris, texes, seqs


def pick_clips(seqs, budget, every=False):
    """one clip a wanted activity, its first sequence (or every sequence); frames resampled to fit"""
    chosen = list(range(len(seqs))) if every else []
    seen = set()
    for act in WANTED if not every else []:
        for i, s in enumerate(seqs):
            if s["act"] == act and act not in seen and len(s["frames"]):
                chosen.append(i)
                seen.add(act)
                break
    if not chosen:
        chosen = [0]
    elif 0 not in chosen and seqs[0]["act"] not in seen:
        chosen.insert(0, 0)  # the rest sequence, for whatever maps nowhere
    # frames: up to 10 a second and 10 a clip, cut evenly if over budget
    want = []
    for i in chosen:
        s = seqs[i]
        n = len(s["frames"])
        rate = min(10.0, max(1.0, s["fps"]))
        k = max(1, min(10, n, int(round(n / max(s["fps"], 1) * rate))))
        want.append(k)
    while sum(want) > budget and max(want) > 2:
        j = want.index(max(want))
        want[j] -= 1
    clips = []
    for i, k in zip(chosen, want):
        s = seqs[i]
        n = len(s["frames"])
        idx = [min(n - 1, round(f * n / k)) if s["flags"] & 1 else min(n - 1, round(f * (n - 1) / max(1, k - 1))) for f in range(k)]
        dur = n / max(s["fps"], 1)
        clips.append(dict(seq=i, idx=idx, fps=max(1, min(255, round(k / dur))), flags=s["flags"] & 1, act=s["act"]))
    return clips


def seq_map(seqs, clips):
    by_act = {}
    for c, cl in enumerate(clips):
        by_act.setdefault(cl["act"], c)
    out = []
    for i, s in enumerate(seqs):
        hit = [c for c, cl in enumerate(clips) if cl["seq"] == i]
        out.append(hit[0] if hit else by_act.get(s["act"], by_act.get(1, 0)))
    return out


def decimate(tris, poses, target):
    """edge collapses onto an end vertex, cheapest first, until target triangles"""
    tris = [list(t) for t in tris]  # [verts, uvs, tex]
    nv = poses.shape[1]
    # quadrics a vertex, summed over the sample frames
    Q = np.zeros((nv, 4, 4))
    for v, uv, tex in tris:
        for fr in poses:
            a, b, c = fr[v[0]], fr[v[1]], fr[v[2]]
            n = np.cross(b - a, c - a)
            ln = np.linalg.norm(n)
            if ln < 1e-9:
                continue
            n /= ln
            p = np.append(n, -n.dot(a))
            K = np.outer(p, p) * ln  # weighted by area
            for x in v:
                Q[x] += K
    # edges on one texture only, or on the model's open border, get a penalty
    def edges():
        es = {}
        for ti, (v, uv, tex) in enumerate(tris):
            for k in range(3):
                a, b = v[k], v[(k + 1) % 3]
                es.setdefault((min(a, b), max(a, b)), []).append(ti)
        return es

    alive = [True] * len(tris)
    count = len(tris)
    cs = poses[:: max(1, len(poses) // 6)]
    hom = np.concatenate([cs, np.ones(cs.shape[:2] + (1,))], 2)  # S x V x 4
    while count > target:
        es = edges()
        cand = []
        for (a, b), ts in es.items():
            ts = [t for t in ts if alive[t]]
            if ts:
                border = len(ts) == 1 or len({tris[t][2] for t in ts}) > 1
                cand.append((a, b, border))
                cand.append((b, a, border))
        if not cand:
            break
        keep = np.array([c[0] for c in cand])
        gone = np.array([c[1] for c in cand])
        q = Q[keep] + Q[gone]  # E x 4 x 4
        p = hom[:, keep, :]  # S x E x 4
        cost = np.einsum("sei,eij,sej->e", p, q, p)
        bd = np.array([c[2] for c in cand])
        cost = np.where(bd, cost * 4 + 1e3, cost)
        best = None
        order = np.argsort(cost)
        # the cheapest few usually include one that doesn't fold a face over;
        # when they all do, look further down the list
        for e in list(order[:64]) + list(order[64:2048]):
            if not flips(tris, alive, int(keep[e]), int(gone[e]), poses[0]):
                best = (cost[e], int(keep[e]), int(gone[e]))
                break
        if best is None:
            break
        _, keep, gone = best
        Q[keep] += Q[gone]
        # the uv keep has in a triangle on the edge, by texture
        keepuv = {}
        for ti, (v, uv, tex) in enumerate(tris):
            if alive[ti] and keep in v and gone in v:
                keepuv.setdefault(tex, uv[v.index(keep)])
        for ti, (v, uv, tex) in enumerate(tris):
            if not alive[ti] or gone not in v:
                continue
            if keep in v:
                alive[ti] = False
                count -= 1
                continue
            k = v.index(gone)
            v[k] = keep
            if tex in keepuv:
                uv[k] = keepuv[tex]
    return [t for t, a in zip(tris, alive) if a]


def flips(tris, alive, keep, gone, pose):
    for ti, (v, uv, tex) in enumerate(tris):
        if not alive[ti] or gone not in v or keep in v:
            continue
        a, b, c = (pose[x] for x in v)
        n0 = np.cross(b - a, c - a)
        w = [keep if x == gone else x for x in v]
        a, b, c = (pose[x] for x in w)
        n1 = np.cross(b - a, c - a)
        if n0.dot(n1) <= 0:
            return True
    return False


def atlas(texes, used, size):
    """the used textures, shrunk by one factor, packed on shelves; returns image and placements"""
    W, H = size
    items = sorted(used, key=lambda t: -texes[t][2])
    f = 1.0
    while True:
        place, x, y, shelf = {}, 0, 0, 0
        ok = True
        for t in items:
            w = max(1, int(texes[t][1] * f))
            h = max(1, int(texes[t][2] * f))
            if x + w > W:
                x, y, shelf = 0, y + shelf, 0
            if w > W or y + h > H:
                ok = False
                break
            place[t] = (x, y, w, h)
            x += w
            shelf = max(shelf, h)
        if ok:
            break
        f *= 0.9
    img = Image.new("RGB", (W, H), (0, 0, 0))
    for t, (x, y, w, h) in place.items():
        name, tw, th, pix, pal = texes[t]
        src = Image.fromarray(pal[pix].astype(np.uint8), "RGB").resize((w, h), Image.BILINEAR)
        img.paste(src, (x, y))
    q = img.quantize(255, method=Image.Quantize.MEDIANCUT)
    pal = np.array(q.getpalette()[:768] + [0] * (768 - len(q.getpalette()[:768])), np.uint8)
    return np.array(q, np.uint8), pal, place


def main():
    a = sys.argv[1:]
    opt = {"-tris": "200", "-frames": "72", "-skin": "128x64"}
    pos = []
    view = mirror = False
    i = 0
    while i < len(a):
        if a[i] in ("-view", "-mirror"):
            view, mirror = (True, mirror) if a[i] == "-view" else (view, True)
            i += 1
        elif a[i] in opt:
            opt[a[i]] = a[i + 1]
            i += 2
        else:
            pos.append(a[i])
            i += 1
    nv, tris, texes, seqs = load(pos[0])
    if view:
        tris = [t for t in tris if t[2] >= len(texes) or not (ARMS.search(texes[t[2]][0]) or EFFECTS.search(texes[t[2]][0]))]
    clips = pick_clips(seqs, int(opt["-frames"]), view)
    smap = seq_map(seqs, clips)
    frames = np.stack([seqs[c["seq"]]["frames"][k] for c in clips for k in c["idx"]])
    if mirror:
        frames = frames * np.array([1, -1, 1], np.float32)  # y is left
        tris = [(v[::-1], uv[::-1], tex) for v, uv, tex in tris]
    sample = frames[:: max(1, len(frames) // 12)]
    small = decimate([(t[0], list(t[1]), t[2]) for t in tris], sample, int(opt["-tris"]))

    W, H = (int(v) for v in opt["-skin"].split("x"))
    used = sorted({t[2] for t in small if t[2] < len(texes)})
    if used:
        skin, pal, place = atlas(texes, used, (W, H))
    else:
        skin, pal, place = np.zeros((H, W), np.uint8), np.zeros(768, np.uint8), {}

    # one DS vertex per (vertex, s, t): Quake models keep one uv a vertex
    verts, stv, out_tris = {}, [], []
    for v, uv, tex in small:
        idx = []
        for x, (s, t) in zip(v, uv):
            if tex in place:
                px, py, pw, ph = place[tex]
                tw, th = texes[tex][1], texes[tex][2]
                s = px + min(max(s, 0), tw - 1) * pw / tw
                t = py + min(max(t, 0), th - 1) * ph / th
            key = (x, int(round(s)), int(round(t)))
            if key not in verts:
                verts[key] = len(stv)
                stv.append(key)
            idx.append(verts[key])
        out_tris.append(idx)
    src = [k[0] for k in stv]
    fr = frames[:, src, :]
    lo = fr.reshape(-1, 3).min(0)
    hi = fr.reshape(-1, 3).max(0)
    scale = np.maximum(hi - lo, 1e-3) / 255.0
    radius = float(np.linalg.norm(np.maximum(np.abs(lo), np.abs(hi))))

    o = bytearray()
    o += struct.pack("<4si3f3ff3f", b"IDPO", 6, *scale, *lo, radius, 0, 0, 0)
    o += struct.pack("<8if", 1, W, H, len(stv), len(out_tris), len(fr), 0, 0, 1.0)
    o += struct.pack("<i", 0) + skin.tobytes()
    for x, s, t in stv:
        o += struct.pack("<3i", 0, s, t)
    for t in out_tris:
        # Quake's winding is the other way round from GoldSrc's
        o += struct.pack("<4i", 1, t[0], t[2], t[1])
    names = [(seqs[c["seq"]]["label"], k) for c in clips for k in range(len(c["idx"]))]
    for f, (label, k) in zip(fr, names):
        q = np.clip(np.round((f - lo) / scale), 0, 255).astype(np.uint8)
        o += struct.pack("<i", 0) + bytes([0, 0, 0, 0, 255, 255, 255, 0])
        o += ("%.12s%d" % (label, k))[:15].encode().ljust(16, b"\0")
        o += np.concatenate([q, np.zeros((len(q), 1), np.uint8)], 1).tobytes()
    at = len(o)
    o += b"DSHL" + struct.pack("<H", len(clips))
    first = 0
    for c in clips:
        o += struct.pack("<HBBBB", first, len(c["idx"]), c["fps"], c["flags"], c["act"] & 255)
        first += len(c["idx"])
    o += struct.pack("<H", len(smap)) + bytes(smap) + pal.tobytes()
    o += struct.pack("<I", at) + b"DSHL"
    open(pos[1], "wb").write(o)
    print("%s: %d -> %d triangles, %d verts, %d clips %d frames (%s), skin %dx%d from %d textures, %d KB" % (
        pos[1], len(tris), len(out_tris), len(stv), len(clips), len(fr),
        ",".join("%s:%d" % (seqs[c["seq"]]["label"], len(c["idx"])) for c in clips), W, H, len(used), len(o) // 1024))


main()
