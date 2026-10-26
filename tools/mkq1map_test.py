#!/usr/bin/env python3
"""Writes tests/data/maps/q1test.bsp: a small Quake 1 (BSP29) map for
testing Q1 rendering, collision and visibility. With --hl it writes the same
map as GoldSrc BSP30 (tests/data/maps/hltest.bsp): four Half-Life sized
hulls, RGB lighting, a palette after each texture's mips, and the floor
texture kept outside the map in tests/data/hltest.wad (WAD3). The layout follows the
public descriptions of the format (the "Unofficial Quake Specs").

Two closed rooms, A at x 0..512 and B at x 1024..1536 (both y 0..512,
z 0..256). Room A has a 128x128x64 block on its floor. The visibility data
says the rooms cannot see each other. Lighting is a soft point light per
room, computed here.

With --bots it writes tests/data/maps/botmap.bsp (BSP30) and its waypoint
file tests/data/botmap.way instead: the bot framework's test map (see
docs/spec/bots.md), a 2048x2048 room with a dividing wall, a 48 unit ledge,
a flight of stairs and two platforms with a gap to jump. ISC licence, see LICENSE."""
import math
import struct
import sys

import os

args = [a for a in sys.argv[1:] if a not in ("--hl", "--bots")]
BOTS = "--bots" in sys.argv[1:]
HL = "--hl" in sys.argv[1:] or BOTS
out = args[0] if args else ("tests/data/maps/botmap.bsp" if BOTS else
                            "tests/data/maps/hltest.bsp" if HL else "tests/data/maps/q1test.bsp")

ROOMS = [((0, 0, 0), (512, 512, 256)), ((1024, 0, 0), (1536, 512, 256))]
BLOCKS = [((192, 192, 0), (320, 320, 64))]
if BOTS:
    ROOMS = [((-1024, -1024, 0), (1024, 1024, 384))]
    BLOCKS = [((-16, -400, 0), (16, 1024, 128)),       # dividing wall, open at the south end
              ((400, 200, 0), (800, 700, 48)),         # the ledge
              ((-800, -1024, 0), (-768, -854, 16)),    # stairs: three steps and a landing
              ((-768, -1024, 0), (-736, -854, 32)),
              ((-736, -1024, 0), (-704, -854, 48)),
              ((-704, -1024, 0), (-576, -854, 64)),
              ((300, -900, 0), (400, -800, 64)),       # two platforms with a gap between
              ((496, -900, 0), (596, -800, 64))]
BIG = 4096

def room_of(p):
    for i, (r0, r1) in enumerate(ROOMS):
        if all(r0[k] <= p[k] <= r1[k] for k in range(3)):
            return i
    return 0
HULLS = [((0, 0, 0), (0, 0, 0)), ((-16, -16, -24), (16, 16, 32)), ((-32, -32, -24), (32, 32, 64))]
if HL:  # standing, large, crouching
    HULLS = [((0, 0, 0), (0, 0, 0)), ((-16, -16, -36), (16, 16, 36)), ((-32, -32, -32), (32, 32, 32)),
             ((-16, -16, -18), (16, 16, 18))]

def empty(p, hull):
    mins, maxs = hull
    inroom = any(all(r0[k] - mins[k] <= p[k] <= r1[k] - maxs[k] for k in range(3)) for r0, r1 in ROOMS)
    inblock = any(all(b0[k] - maxs[k] < p[k] < b1[k] - mins[k] for k in range(3)) for b0, b1 in BLOCKS)
    return inroom and not inblock

def split_planes(hull):
    mins, maxs = hull
    ps = set()
    for r0, r1 in ROOMS:
        for k in range(3):
            ps.add((k, r0[k] - mins[k]))
            ps.add((k, r1[k] - maxs[k]))
    for b0, b1 in BLOCKS:
        for k in range(3):
            ps.add((k, b0[k] - maxs[k]))
            ps.add((k, b1[k] - mins[k]))
    return sorted(ps)

planes = []  # (axis, dist)
def plane_index(axis, dist):
    if (axis, dist) not in planes:
        planes.append((axis, dist))
    return planes.index((axis, dist))

# --- hull 0: nodes and leafs ---------------------------------------------------

nodes = []   # [planenum, front, back, mins, maxs]
leafs = [dict(contents=-2, mins=(-BIG,) * 3, maxs=(BIG,) * 3, faces=[], room=-1)]

def build(lo, hi, hull, cand, node_list, leaf_cb):
    for axis, dist in cand:
        if lo[axis] < dist < hi[axis]:
            pn = plane_index(axis, dist)
            idx = len(node_list)
            node_list.append(None)
            fhi, flo = list(hi), list(lo)
            flo[axis] = dist
            bhi = list(hi)
            bhi[axis] = dist
            front = build(flo, hi, hull, cand, node_list, leaf_cb)
            back = build(lo, bhi, hull, cand, node_list, leaf_cb)
            node_list[idx] = [pn, front, back, lo, hi]
            return idx
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    return leaf_cb(empty(centre, hull), lo, hi)

def hull0_leaf(is_empty, lo, hi):
    if not is_empty:
        return -1  # leaf 0, solid
    room = room_of([(lo[k] + hi[k]) / 2 for k in range(3)])
    leafs.append(dict(contents=-1, mins=lo, maxs=hi, faces=[], room=room))
    return -len(leafs)

root0 = build([-BIG] * 3, [BIG] * 3, HULLS[0], split_planes(HULLS[0]), nodes, hull0_leaf)

# --- hulls 1 and 2: clipnodes ----------------------------------------------------

clipnodes = []
def clip_leaf(is_empty, lo, hi):
    return -1 if is_empty else -2
clip_roots = []
for h in range(1, len(HULLS)):
    clip_roots.append(build([-BIG] * 3, [BIG] * 3, HULLS[h], split_planes(HULLS[h]), clipnodes, clip_leaf))

# --- faces -------------------------------------------------------------------------

def rect_faces():
    """(axis, dist, normal sign, lo, hi) rectangles facing into the empty space"""
    fs = []
    for r0, r1 in ROOMS:
        for k in range(3):
            lo, hi = list(r0), list(r1)
            fs.append((k, r0[k], +1, lo[:], hi[:]))
            fs.append((k, r1[k], -1, lo[:], hi[:]))
    for b0, b1 in BLOCKS:
        for k in range(3):
            if k == 2:
                fs.append((2, b1[2], +1, list(b0), list(b1)))  # top only; the bottom sits on the floor
            else:
                fs.append((k, b0[k], -1, list(b0), list(b1)))
                fs.append((k, b1[k], +1, list(b0), list(b1)))
    # keep faces small enough for ordinary lightmaps
    tiled = []
    for axis, dist, sign, lo, hi in fs:
        a1, a2 = [k for k in range(3) if k != axis]
        u = lo[a1]
        while u < hi[a1]:
            u2 = min(hi[a1], u + 256)
            v = lo[a2]
            while v < hi[a2]:
                v2 = min(hi[a2], v + 256)
                tlo, thi = list(lo), list(hi)
                tlo[a1], thi[a1], tlo[a2], thi[a2] = u, u2, v, v2
                tiled.append((axis, dist, sign, tlo, thi))
                v = v2
            u = u2
    return tiled if BOTS else fs

texinfos = []
def texinfo_for(axis, tex):
    if axis == 2:
        s, t = (1, 0, 0), (0, -1, 0)
    elif axis == 0:
        s, t = (0, 1, 0), (0, 0, -1)
    else:
        s, t = (1, 0, 0), (0, 0, -1)
    ti = (s + (0,), t + (0,), tex)
    if ti not in texinfos:
        texinfos.append(ti)
    return texinfos.index(ti)

verts, edges, surfedges, faces, lighting = [], [(0, 0)], [], [], bytearray()

def light_at(p, room):
    r0, r1 = ROOMS[room]
    c = [(r0[0] + r1[0]) / 2, (r0[1] + r1[1]) / 2, 200]
    d = math.dist(p, c)
    v = 24 + 120 * max(0.0, 1 - d / max(450, (r1[0] - r0[0]) * 0.75))
    return int(min(255, v))

for axis, dist, sign, lo, hi in rect_faces():
    a1, a2 = [k for k in range(3) if k != axis]
    corners = []
    for u, v in ((lo[a1], lo[a2]), (hi[a1], lo[a2]), (hi[a1], hi[a2]), (lo[a1], hi[a2])):
        p = [0, 0, 0]
        p[axis], p[a1], p[a2] = dist, u, v
        corners.append(tuple(p))
    # clockwise seen from the side the normal points to
    normal = [0, 0, 0]
    normal[axis] = sign
    e1 = [corners[1][k] - corners[0][k] for k in range(3)]
    e2 = [corners[2][k] - corners[0][k] for k in range(3)]
    cross = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
    if sum(cross[k] * normal[k] for k in range(3)) > 0:
        corners.reverse()
    pn = plane_index(axis, dist)
    side = 0 if sign > 0 else 1
    tex = 1 if axis == 2 and sign > 0 else 0
    ti = texinfo_for(axis, tex)
    first = len(surfedges)
    base = len(verts)
    verts.extend(corners)
    for i in range(4):
        edges.append((base + i, base + (i + 1) % 4))
        surfedges.append(len(edges) - 1)
    s_vec, t_vec, _ = texinfos[ti]
    ss = [sum(c[k] * s_vec[k] for k in range(3)) + s_vec[3] for c in corners]
    ts = [sum(c[k] * t_vec[k] for k in range(3)) + t_vec[3] for c in corners]
    smin, smax = math.floor(min(ss) / 16), math.ceil(max(ss) / 16)
    tmin, tmax = math.floor(min(ts) / 16), math.ceil(max(ts) / 16)
    w, h = smax - smin + 1, tmax - tmin + 1
    room = room_of([(lo[k] + hi[k]) / 2 for k in range(3)])
    lightofs = len(lighting)
    # luxel positions on the plane: solve p from (s, t)
    for j in range(h):
        for i in range(w):
            s, t = (smin + i) * 16, (tmin + j) * 16
            p = [0, 0, 0]
            p[axis] = dist
            for k in (a1, a2):
                p[k] = s * s_vec[k] + t * t_vec[k]
            v = light_at(p, room)
            if HL:  # 255 is full brightness; room A warm, room B cool
                v = min(255, v * 2)
                lighting.extend((v, v * 3 // 4, v // 2) if room == 0 else (v // 2, v * 3 // 4, v))
            else:
                lighting.append(v)
    faces.append(dict(plane=pn, side=side, firstedge=first, numedges=4, texinfo=ti, lightofs=lightofs,
                      axis=axis, dist=dist, sign=sign, lo=lo, hi=hi))

# each face belongs to the first node on its plane whose box holds the face's
# centre (light sampling walks these lists); faces are stored grouped by node
def face_node(f):
    a = f["axis"]
    c = [(f["lo"][k] + f["hi"][k]) / 2 for k in range(3)]
    c[a] = f["dist"]
    for ni, n in enumerate(nodes):
        if n[0] == f["plane"] and all(n[3][k] <= c[k] <= n[4][k] for k in range(3)):
            return ni
    return len(nodes)

for f in faces:
    f["node"] = face_node(f)
faces.sort(key=lambda f: f["node"])
node_faces = [[0, 0] for _ in nodes]
for fi, f in enumerate(faces):
    if f["node"] < len(nodes):
        nf = node_faces[f["node"]]
        if not nf[1]:
            nf[0] = fi
        nf[1] += 1

# faces touching each empty leaf (the leaf's boundary holds the face's plane)
for li, lf in enumerate(leafs):
    if lf["contents"] != -1:
        continue
    for fi, f in enumerate(faces):
        a = f["axis"]
        face_side = lf["mins"][a] if f["sign"] > 0 else lf["maxs"][a]
        if face_side != f["dist"]:
            continue
        if all(lf["mins"][k] < f["hi"][k] and lf["maxs"][k] > f["lo"][k] for k in range(3) if k != a):
            lf["faces"].append(fi)

marksurfaces = []
for lf in leafs:
    lf["firstmark"] = len(marksurfaces)
    marksurfaces.extend(lf["faces"])

# --- visibility: each room sees only itself ------------------------------------------

nvis = len(leafs) - 1
vis = bytearray()
for li, lf in enumerate(leafs):
    if li == 0:
        lf["visofs"] = -1
        continue
    row = bytearray((nvis + 7) // 8)
    for lj in range(1, len(leafs)):
        if leafs[lj]["room"] == lf["room"]:
            row[(lj - 1) >> 3] |= 1 << ((lj - 1) & 7)
    lf["visofs"] = len(vis)
    i = 0
    while i < len(row):
        if row[i]:
            vis.append(row[i])
            i += 1
        else:
            n = 0
            while i < len(row) and not row[i] and n < 255:
                n += 1
                i += 1
            vis += bytes((0, n))

# --- textures ------------------------------------------------------------------------------

def miptex(name, a, b, pixels=True):
    size = 64
    hdr = name.encode().ljust(16, b"\0") + struct.pack("<II", size, size)
    if not pixels:
        return hdr + bytes(16)
    data = b""
    offs = []
    for m in range(4):
        s = size >> m
        offs.append(40 + len(data))
        data += bytes(a if ((x * (1 << m) // 16) + (y * (1 << m) // 16)) % 2 else b
                      for y in range(s) for x in range(s))
    if HL:  # 256 colours: a ramp per channel so indexes give distinct colours
        data += struct.pack("<H", 256) + bytes(c for i in range(256) for c in (i, 255 - i, (i * 7) & 255)) + b"\0\0"
    return hdr + struct.pack("<4I", *offs) + data

if HL:
    texs = [miptex("bwall", 6, 9), miptex("bfloor", 21, 26, pixels=False)]
    floor = miptex("bfloor", 21, 26)
    wad = bytearray(b"WAD3" + struct.pack("<ii", 1, 12 + len(floor)))
    wad += floor
    wad += struct.pack("<iiiBBH", 12, len(floor), len(floor), 0x43, 0, 0) + b"BFLOOR".ljust(16, b"\0")
    open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(out))), "hltest.wad"), "wb").write(wad)
else:
    texs = [miptex("bwall", 6, 9), miptex("bfloor", 21, 26)]
texlump = struct.pack("<I", len(texs))
ofs = 4 + 4 * len(texs)
blob = b""
for t in texs:
    texlump += struct.pack("<i", ofs + len(blob))
    blob += t
texlump += blob

# --- entities and lumps ------------------------------------------------------------------

entities = (b'{\n"classname" "worldspawn"\n"message" "Bulwark Q1 test"\n"wad" "%s"\n}\n'
            b'{\n"classname" "info_player_start"\n"origin" "96 96 40"\n"angle" "45"\n}\n'
            b'{\n"classname" "info_player_deathmatch"\n"origin" "96 96 40"\n"angle" "45"\n}\n\0')
entities = entities.replace(b"%s", b"\\sierra\\half-life\\valve\\hltest.wad;other.wad" if HL else b"")
if BOTS:
    entities = (b'{\n"classname" "worldspawn"\n"message" "Bulwark bot test"\n"wad" "hltest.wad"\n}\n'
                b'{\n"classname" "info_player_start"\n"origin" "-900 900 40"\n"angle" "0"\n}\n\0')

def pack_planes():
    b = b""
    for axis, dist in planes:
        n = [0.0, 0.0, 0.0]
        n[axis] = 1.0
        b += struct.pack("<4fi", *n, dist, axis)
    return b

def bbox_short(lo, hi):
    return [max(-32768, min(32767, int(v))) for v in list(lo) + list(hi)]

def node_child(c):
    return c  # node index, or -(leaf + 1) as returned by build

lumps = [None] * 15
lumps[0] = entities
lumps[1] = pack_planes()
lumps[2] = texlump
lumps[3] = b"".join(struct.pack("<3f", *v) for v in verts)
lumps[4] = bytes(vis)
lumps[5] = b"".join(struct.pack("<i2h6h2H", n[0], node_child(n[1]), node_child(n[2]), *bbox_short(n[3], n[4]),
                                 *node_faces[i]) for i, n in enumerate(nodes))
lumps[6] = b"".join(struct.pack("<8f2i", *s, *t, tex, 0) for s, t, tex in texinfos)
lumps[7] = b"".join(struct.pack("<hhihh4Bi", f["plane"], f["side"], f["firstedge"], f["numedges"], f["texinfo"],
                                0, 255, 255, 255, f["lightofs"]) for f in faces)
lumps[8] = bytes(lighting)
lumps[9] = b"".join(struct.pack("<i2h", n[0], n[1], n[2]) for n in clipnodes)
lumps[10] = b"".join(struct.pack("<ii6h2H4B", lf["contents"], lf["visofs"], *bbox_short(lf["mins"], lf["maxs"]),
                                 lf["firstmark"], len(lf["faces"]), 0, 0, 0, 0) for lf in leafs)
lumps[11] = b"".join(struct.pack("<H", m) for m in marksurfaces)
lumps[12] = b"".join(struct.pack("<2H", *e) for e in edges)
lumps[13] = b"".join(struct.pack("<i", e) for e in surfedges)
wmin = [min(r[0][k] for r in ROOMS) for k in range(3)]
wmax = [max(r[1][k] for r in ROOMS) for k in range(3)]
lumps[14] = struct.pack("<9f4i3i", *wmin, *wmax, 0, 0, 0, root0, clip_roots[0], clip_roots[1],
                        clip_roots[2] if HL else 0,
                        len(leafs) - 1, 0, len(faces))

data = bytearray(struct.pack("<i", 30 if HL else 29) + b"\0" * (15 * 8))
for i, l in enumerate(lumps):
    while len(data) % 4:
        data.append(0)
    struct.pack_into("<ii", data, 4 + i * 8, len(data), len(l))
    data += l
open(out, "wb").write(data)
print(f"{out}: {len(nodes)} nodes, {len(leafs)} leafs, {len(clipnodes)} clipnodes, {len(faces)} faces")

# --- bot test waypoints -------------------------------------------------------------
# docs/spec/bots.md: node count, then per node "x y z radius linkcount" and a
# line "othernode cost flags" per link; positions 24 above the floor.
if BOTS:
    WP = [(-900, 900, 24), (-900, 300, 24), (-600, -300, 24), (-500, -700, 24), (-150, -600, 24),
          (0, -650, 24), (150, -600, 24), (500, -300, 24), (600, 100, 24), (600, 300, 72),
          (600, 600, 72), (600, 900, 24), (250, 450, 24), (900, 450, 24), (900, -800, 24),
          (-300, 400, 24), (-880, -940, 24), (-640, -940, 88), (-900, -600, 24), (300, 900, 24),
          (350, -850, 88), (546, -850, 88)]
    PAIRS = [(0, 1), (0, 15), (1, 15), (1, 2), (15, 2), (2, 3), (2, 18), (18, 16), (16, 17), (18, 3),
             (3, 4), (4, 5), (5, 6), (6, 7), (6, 14), (7, 8), (7, 14), (14, 13), (8, 9), (9, 10),
             (10, 11), (8, 12), (12, 19), (19, 11), (11, 13), (13, 8), (12, 9), (13, 10), (7, 12), (20, 21), (20, 6), (21, 14)]
    links = [[] for _ in WP]
    for a, b in PAIRS:
        cost = round(math.dist(WP[a], WP[b]))
        links[a].append((b, cost, 0))
        links[b].append((a, cost, 0))
    lines = [str(len(WP))]
    for i, p in enumerate(WP):
        lines.append("%g %g %g 32 %d" % (*p, len(links[i])))
        lines += ["%d %d %d" % l for l in links[i]]
    wayout = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(out))), "botmap.way")
    open(wayout, "w").write("\n".join(lines) + "\n")
    print(f"{wayout}: {len(WP)} waypoints, {len(PAIRS) * 2} links")
