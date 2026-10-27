#!/usr/bin/env python3
# Writes a small GoldSrc (IDST v10) model to test the renderer, laid out as
# docs/spec/studio-mdl.md describes: two boxes on
# two bones, a "bend" sequence that turns the upper bone, two skin families
# and a masked texture. ISC licence, see LICENSE.
# usage: mkstudio_test.py out.mdl [outT.mdl]  (the second puts the textures there)
import struct, sys
out = sys.argv[1]; texout = sys.argv[2] if len(sys.argv) > 2 else None
class W:
    def __init__(s): s.b = bytearray()
    def z(s, n):
        a = len(s.b); s.b += bytes(n); return a
    def i(s, at, v): struct.pack_into('<i', s.b, at, v)
    def f(s, at, v): struct.pack_into('<f', s.b, at, v)
    def h(s, at, v): struct.pack_into('<h', s.b, at, v)
    def name(s, at, t): s.b[at:at+len(t)] = t.encode()

def textures(w, hdr_has_skins=True):
    texs = []
    for k, (nm, flags) in enumerate([("checker.bmp", 0), ("grate.bmp", 0x40)]):
        t = w.z(80); w.name(t, nm); w.i(t+64, flags); w.i(t+68, 64); w.i(t+72, 64)
        texs.append(t)
    w.i(180, 2); w.i(184, texs[0])
    for k, t in enumerate(texs):
        px = w.z(64*64 + 768); w.i(t+76, px)
        for y in range(64):
            for x in range(64):
                if k == 0: c = 1 if ((x//8)+(y//8)) % 2 else 2
                else: c = 255 if (x % 16 < 8 and y % 16 < 8) else 3
                if x < 4 and y < 4: c = 4   # marks the top-left corner
                w.b[px + y*64 + x] = c
        pal = px + 64*64
        cols = {0:(0,0,0),1:(230,230,230),2:(200,40,40),3:(40,160,60),4:(30,60,220),255:(255,0,255)}
        for c, rgb in cols.items(): w.b[pal+c*3:pal+c*3+3] = bytes(rgb)
    w.i(188, len(w.b))
    sk = w.z(2*2*2)       # 2 families x 2 refs
    for n, v in enumerate([0, 1, 1, 0]): w.h(sk + n*2, v)
    w.i(192, 2); w.i(196, 2); w.i(200, sk)

w = W(); w.z(244); w.name(0, "IDST"); w.i(4, 10); w.name(8, "arm.mdl")

for k, v in enumerate([-16,-16,0]): w.f(88+k*4, v)
for k, v in enumerate([16,16,64]): w.f(100+k*4, v)
bones = w.z(2*112)
w.name(bones, "root"); w.i(bones+32, -1)
w.name(bones+112, "upper"); w.i(bones+112+32, 0)
for j in range(2):
    for k in range(6):
        w.i(bones+112*j+40+4*k, -1)
        w.f(bones+112*j+64+4*k, 32.0 if (j == 1 and k == 2) else 0.0)
        w.f(bones+112*j+88+4*k, 0.01 if k >= 3 else 1.0)
w.i(140, 2); w.i(144, bones)
hb = w.z(32); w.i(hb, 0); w.i(156, 1); w.i(160, hb)
sg = w.z(104); w.name(sg, "default"); w.i(172, 1); w.i(176, sg)
# sequences: idle (1 frame, no animated channels), bend (10 frames: upper bone y rotation 0..1.35)
anim_idle = w.z(24)
anim_bend = w.z(24)
rle = w.z(2 + 2*10)
w.b[rle] = 10; w.b[rle+1] = 10
for f in range(10): w.h(rle+2+2*f, f*15)
w.h(anim_bend + 12 + 2*4, rle - (anim_bend + 12))   # bone 1, channel 4
seqs = w.z(2*176)
for n, (lab, frames, anim, flags) in enumerate([("idle", 1, anim_idle, 1), ("bend", 10, anim_bend, 1)]):
    s = seqs + n*176; w.name(s, lab); w.f(s+32, 5.0); w.i(s+36, flags); w.i(s+56, frames); w.i(s+120, 1); w.i(s+124, anim)
w.i(164, 2); w.i(168, seqs)
if not texout: textures(w)
else: w.i(192, 2); w.i(196, 1); sk = w.z(4); w.h(sk, 0); w.h(sk+2, 1); w.i(200, sk)
# geometry: a box on each bone, bone-local, z 0..32; faces as fans
def box(bone, half):
    v = [(x, y, z) for z in (0, 32) for y in (-half, half) for x in (-half, half)]
    faces = [((0,2,3,1),(0,0,-1)), ((4,5,7,6),(0,0,1)), ((0,1,5,4),(0,-1,0)), ((2,6,7,3),(0,1,0)), ((0,4,6,2),(-1,0,0)), ((1,3,7,5),(1,0,0))]
    return v, faces
vinfo = w.z(16); norminfo = w.z(12)
for i in range(16): w.b[vinfo+i] = i // 8
for i in range(12): w.b[norminfo+i] = i // 6
verts = w.z(16*12); norms = w.z(12*12)
cmds = []
for b in range(2):
    v, faces = box(b, 12 if b == 0 else 8)
    for i, p in enumerate(v):
        for k in range(3): w.f(verts + (b*8+i)*12 + k*4, p[k])
    for fi, (quad, n) in enumerate(faces):
        for k in range(3): w.f(norms + (b*6+fi)*12 + k*4, n[k])
meshes = w.z(2*20)
for b in range(2):
    v, faces = box(b, 12 if b == 0 else 8)
    t = w.z(2 + len(faces)*(2+4*8))
    p = t
    for fi, (quad, n) in enumerate(faces):
        w.h(p, -4); p += 2
        for c, vi in enumerate(quad):
            s, tt = [(0,0),(63,0),(63,63),(0,63)][c]
            w.h(p, b*8+vi); w.h(p+2, b*6+fi); w.h(p+4, s); w.h(p+6, tt); p += 8
    w.h(p, 0)
    w.i(meshes + b*20, len(faces)*2); w.i(meshes + b*20 + 4, t); w.i(meshes + b*20 + 8, b)
sub = w.z(112); w.name(sub, "body")
w.i(sub+72, 2); w.i(sub+76, meshes); w.i(sub+80, 16); w.i(sub+84, vinfo); w.i(sub+88, verts)
w.i(sub+92, 12); w.i(sub+96, norminfo); w.i(sub+100, norms)
bp = w.z(76); w.name(bp, "studio"); w.i(bp+64, 1); w.i(bp+68, 1); w.i(bp+72, sub)
w.i(204, 1); w.i(208, bp)
att = w.z(88); w.i(att+36, 1); w.i(212, 1); w.i(216, att)
w.i(72, len(w.b))
open(out, 'wb').write(w.b)
if texout:
    t = W(); t.z(244); t.name(0, "IDST"); t.i(4, 10); textures(t); t.i(72, len(t.b)); open(texout, 'wb').write(t.b)
