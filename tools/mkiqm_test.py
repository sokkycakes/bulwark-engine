#!/usr/bin/env python3
"""Writes the two-joint test model tests/data/skeltest.iqm, laid out by the
public IQM v2 specification. A strip runs along +X: the first two vertices
follow the root joint, the last two the "tip" joint at x=32. Raw frames turn
the tip about +Z by 0, 90 and 180 degrees; animation "still" is frame 0 and
"bend" is frames 0-2 at 10 fps. ISC licence, see LICENSE."""
import math
import struct
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "tests/data/skeltest.iqm"

text = b"\0"
def name(s):
    global text
    o = len(text)
    text += s.encode() + b"\0"
    return o

n_mesh, n_mat = name("strip"), name("white")
n_root, n_tip = name("root"), name("tip")
n_still, n_bend = name("still"), name("bend")
while len(text) % 4:
    text += b"\0"

pos = [(0, 0, 0), (32, 0, 0), (64, 0, 0), (64, 0, 8)]
tc = [(0, 0), (0.5, 0), (1, 0), (1, 1)]
nrm = [(0, -1, 0)] * 4
bidx = [(0, 0, 0, 0), (0, 0, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
bw = [(255, 0, 0, 0)] * 4
tris = [(1, 2, 3), (0, 1, 3)]

angles = [0, 90, 180]
quats = [(0, 0, math.sin(math.radians(a) / 2), math.cos(math.radians(a) / 2)) for a in angles]

def pack_floats(rows):
    return b"".join(struct.pack("<%df" % len(r), *r) for r in rows)

blobs = []  # (name, bytes) in file order after the header
blobs.append(("text", text))
mesh = struct.pack("<6I", n_mesh, n_mat, 0, 4, 0, 2)
blobs.append(("meshes", mesh))
va_data = [
    (0, 7, 3, pack_floats(pos)),
    (1, 7, 2, pack_floats(tc)),
    (2, 7, 3, pack_floats(nrm)),
    (4, 1, 4, bytes(sum(bidx, ()))),
    (5, 1, 4, bytes(sum(bw, ()))),
]
blobs.append(("vertexarrays", None))
for i, v in enumerate(va_data):
    blobs.append(("va%d" % i, v[3]))
blobs.append(("triangles", b"".join(struct.pack("<3I", *t) for t in tris)))
joints = struct.pack("<Ii10f", n_root, -1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1)
joints += struct.pack("<Ii10f", n_tip, 0, 32, 0, 0, 0, 0, 0, 1, 1, 1, 1)
blobs.append(("joints", joints))
qscale = 2 / 65535.0
poses = struct.pack("<iI10f10f", -1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, *([0] * 10))
poses += struct.pack("<iI10f10f", 0, 0x78, 32, 0, 0, -1, -1, -1, -1, 1, 1, 1,
                     0, 0, 0, qscale, qscale, qscale, qscale, 0, 0, 0)
blobs.append(("poses", poses))
anims = struct.pack("<3IfI", n_still, 0, 1, 10, 0) + struct.pack("<3IfI", n_bend, 0, 3, 10, 0)
blobs.append(("anims", anims))
frames = b""
for q in quats:
    frames += struct.pack("<4H", *[int(round((c + 1) / qscale)) for c in q])
blobs.append(("frames", frames))

ofs = {}
cur = 124
for nm, b in blobs:
    if nm == "vertexarrays":
        b = b"\0" * (20 * len(va_data))
    ofs[nm] = cur
    cur += len(b)
    while cur % 4:
        cur += 1
va_table = b"".join(struct.pack("<5I", t, 0, f, c, ofs["va%d" % i]) for i, (t, f, c, _) in enumerate(va_data))

header = b"INTERQUAKEMODEL\0" + struct.pack(
    "<27I", 2, cur, 0,
    len(text), ofs["text"],
    1, ofs["meshes"],
    len(va_data), 4, ofs["vertexarrays"],
    len(tris), ofs["triangles"], 0,
    2, ofs["joints"],
    2, ofs["poses"],
    2, ofs["anims"],
    len(quats), 4, ofs["frames"], 0,
    0, 0,
    0, 0)
assert len(header) == 124
data = bytearray(header)
for nm, b in blobs:
    if nm == "vertexarrays":
        b = va_table
    assert len(data) == ofs[nm], nm
    data += b
    while len(data) % 4:
        data += b"\0"
open(out, "wb").write(data)
