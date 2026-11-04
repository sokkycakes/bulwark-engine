#!/usr/bin/env python3
# Bulwark engine - turns a Half-Life map (BSP version 30) into what the DS
# map viewer loads (docs/platforms/nds.md, "Half-Life maps"). ISC licence,
# see LICENSE.
#
#   hlmap.py <map.bsp> <out.bsp> [-wad dir]... [-texkb n] [-maxdim n]
#
# The DS keeps one 256-colour palette for the world and has 4 MB of RAM, so
# the map is rewritten on the PC:
#
# - every texture is taken from the map or from the WADs its worldspawn
#   names (looked for in each -wad folder), shrunk to a power of two so all
#   of them fit -texkb of texture memory, and drawn with one palette made
#   for this map (index 255 stays clear for "{" textures);
# - the coloured lightmaps become one brightness a sample in the map, and
#   the colours go to a Quake-style .lit file beside it (QLIT, version 1);
# - clip hulls, which only the server needs, are left out;
# - tool textures (triggers, clip, origin, null) are renamed to the names
#   the DS already skips, and "!" water to Quake's "*".
#
# The output keeps Quake's layout (version 29), so face, model and texture
# numbers stay as they are in the original: the server's "*3" is the DS's
# model 3. Next to it come <out>.pal (the palette, 768 bytes) and <out>.lit.
#
# The result is the game's data cut down: keep it out of git and packages.
import os
import struct
import sys

import numpy as np
from PIL import Image

NUM_LUMPS = 15
L_ENT, L_PLANES, L_TEX, L_VERTS, L_VIS, L_NODES, L_TEXINFO, L_FACES, L_LIGHT, L_CLIP, L_LEAFS, L_MARKS, L_EDGES, L_SURFEDGES, L_MODELS = range(15)

SKIP_NAMES = {"aaatrigger": "trigger", "clip": "clip", "origin": "origin", "null": "skip", "skip": "skip", "hint": "hint", "bevel": "skip",
              "noclip": "clip", "clipbevel": "clip", "solidhint": "hint"}


def read_miptex(data, ofs):
    """name, w, h, mip-0 pixels (bytes) or None, palette (768 bytes) or None"""
    name = data[ofs:ofs + 16].split(b"\0")[0].decode("latin-1")
    w, h = struct.unpack_from("<II", data, ofs + 16)
    offs = struct.unpack_from("<4I", data, ofs + 24)
    if not offs[0] or w == 0 or h == 0 or w > 4096 or h > 4096:
        return name, w, h, None, None
    pix = data[ofs + offs[0]:ofs + offs[0] + w * h]
    # the palette follows the smallest mip: a 16-bit count, then RGB triples
    end = ofs + offs[3] + (w >> 3) * (h >> 3)
    count = struct.unpack_from("<H", data, end)[0] if end + 2 <= len(data) else 0
    pal = data[end + 2:end + 2 + 768] if count == 256 and end + 2 + 768 <= len(data) else None
    return name, w, h, pix, pal


def load_wads(names, dirs):
    """texture name (lower case) -> (data, offset) over every WAD found"""
    found = {}
    for n in names:
        base = os.path.basename(n.replace("\\", "/"))
        for d in dirs:
            p = os.path.join(d, base)
            if not os.path.isfile(p):
                continue
            data = open(p, "rb").read()
            if data[:4] not in (b"WAD3", b"WAD2"):
                continue
            num, tab = struct.unpack_from("<ii", data, 4)
            for i in range(num):
                filepos, disksize, size, typ, comp = struct.unpack_from("<iiibb", data, tab + i * 32)
                lname = data[tab + i * 32 + 16:tab + i * 32 + 32].split(b"\0")[0].decode("latin-1").lower()
                if typ == 0x43 and not comp and lname not in found:
                    found[lname] = (data, filepos)
            break
    return found


def worldspawn_wads(ents):
    for line in ents.split("\n"):
        line = line.strip()
        if line.startswith('"wad"'):
            v = line.split('"')[3]
            return [x for x in v.split(";") if x]
        if line == "}":
            break
    return []


def face_areas(lump, count):
    """the world area (square units) drawn with each texture"""
    verts = np.frombuffer(lump[L_VERTS], np.float32).reshape(-1, 3)
    edges = np.frombuffer(lump[L_EDGES], np.uint16).reshape(-1, 2)
    se = np.frombuffer(lump[L_SURFEDGES], np.int32)
    ti = lump[L_TEXINFO]
    area = [0.0] * count
    fl = lump[L_FACES]
    for f in range(len(fl) // 20):
        first, num, tinfo = struct.unpack_from("<iHH", fl, f * 20 + 4)
        miptex = struct.unpack_from("<i", ti, tinfo * 40 + 32)[0]
        if not 0 <= miptex < count or num < 3:
            continue
        e = se[first:first + num]
        pts = verts[np.where(e >= 0, edges[np.abs(e), 0], edges[np.abs(e), 1])]
        a = np.cross(pts[1:-1] - pts[0], pts[2:] - pts[0])
        area[miptex] += float(np.linalg.norm(a, axis=1).sum()) / 2
    return area


def pot_floor(v):
    p = 1
    while p * 2 <= v:
        p *= 2
    return p


def main():
    args = sys.argv[1:]
    src, out = args[0], args[1]
    wad_dirs, texkb, maxdim = [], 180, 128
    i = 2
    while i + 1 < len(args):
        if args[i] == "-wad":
            wad_dirs.append(args[i + 1])
        elif args[i] == "-texkb":
            texkb = int(args[i + 1])
        elif args[i] == "-maxdim":
            maxdim = int(args[i + 1])
        i += 2
    wad_dirs.append(os.path.dirname(os.path.abspath(src)))
    wad_dirs.append(os.path.join(os.path.dirname(os.path.abspath(src)), ".."))
    d = open(src, "rb").read()
    ver = struct.unpack_from("<i", d)[0]
    if ver != 30:
        sys.exit("%s: BSP version %d, not 30" % (src, ver))
    lumps = [struct.unpack_from("<ii", d, 4 + i * 8) for i in range(NUM_LUMPS)]
    lump = [d[o:o + l] for o, l in lumps]
    ents = lump[L_ENT].split(b"\0")[0].decode("latin-1")
    wads = load_wads(worldspawn_wads(ents), wad_dirs)

    # textures: pixels as RGB, from the map or a WAD
    tl = lump[L_TEX]
    count = struct.unpack_from("<i", tl)[0]
    offs = struct.unpack_from("<%di" % count, tl, 4)
    texs, missing = [], []
    for k in range(count):
        if offs[k] < 0:
            texs.append(None)
            continue
        name, w, h, pix, pal = read_miptex(tl, offs[k])
        if pix is None and name.lower() in wads:
            wd, wo = wads[name.lower()]
            _, w, h, pix, pal = read_miptex(wd, wo)
        low = name.lower()
        if low in SKIP_NAMES:
            texs.append({"name": SKIP_NAMES[low], "w": w, "h": h, "rgb": None})
            continue
        if name.startswith("!"):
            name = "*" + name[1:]
        if pix is None or pal is None:
            missing.append(name)
            texs.append({"name": name, "w": w, "h": h, "rgb": None})
            continue
        p = np.frombuffer(pal, np.uint8).reshape(256, 3)
        idx = np.frombuffer(pix, np.uint8).reshape(h, w)
        rgb = p[idx]
        clear = (idx == 255) if name.startswith("{") else None
        texs.append({"name": name, "w": w, "h": h, "rgb": rgb, "clear": clear})

    # sizes: each texture as big as it is (up to -maxdim), then the biggest
    # halved, one at a time, until they all fit the budget
    sz = []
    for t in texs:
        if not t or t["rgb"] is None:
            sz.append(None)
            continue
        w, h = t["w"], t["h"]
        if t["name"].lower().startswith("sky"):
            w *= 2  # the DS draws a sky's left half: give it two copies
        sz.append([max(8, min(pot_floor(w), maxdim)), max(8, min(pot_floor(h), maxdim))])
    # how much of the map each texture covers: textures on big surfaces keep more detail
    area = face_areas(lump, count)
    while sum(a * b for a, b in (x for x in sz if x)) > texkb * 1024:
        k = max((i for i in range(len(sz)) if sz[i] and max(sz[i]) > 8),
                key=lambda i: (sz[i][0] * sz[i][1]) / (area[i] + 1.0), default=None)
        if k is None:
            break
        a = 0 if sz[k][0] >= sz[k][1] else 1
        sz[k][a] //= 2
    sz = [tuple(x) if x else None for x in sz]
    cap = max((max(x) for x in sz if x), default=0)

    # one palette for the map: the shrunk pictures' colours, 255 of them
    shrunk = []
    for t, s in zip(texs, sz):
        if not s:
            shrunk.append(None)
            continue
        rgb = t["rgb"]
        if t["name"].lower().startswith("sky"):
            rgb = np.concatenate([rgb, rgb], axis=1)
        im = Image.fromarray(rgb, "RGB").resize(s, Image.BOX)
        clear = None
        if t.get("clear") is not None:
            clear = np.array(Image.fromarray((t["clear"] * 255).astype(np.uint8), "L").resize(s, Image.BOX)) > 127
        shrunk.append((im, clear))
    allpix = [np.array(im).reshape(-1, 3) for im, clear in (x for x in shrunk if x)]
    if allpix:
        stack = np.concatenate(allpix)
        side = int(np.ceil(np.sqrt(len(stack))))
        pad = np.zeros((side * side, 3), np.uint8)
        pad[:len(stack)] = stack
        pad[len(stack):] = stack[0]
        q = Image.fromarray(pad.reshape(side, side, 3), "RGB").quantize(255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        palette = (q.getpalette() + [0] * 768)[:765] + [0, 0, 255]
    else:
        palette = [i // 3 for i in range(768)]
    palimg = Image.new("P", (1, 1))
    palimg.putpalette(palette)

    # the texture lump, Quake style: name, size, four mips
    blobs, newoffs = [], []
    pos = 4 + 4 * count
    for k, t in enumerate(texs):
        if t is None:
            newoffs.append(-1)
            continue
        name = t["name"].encode("latin-1")[:15]
        if shrunk[k] is None:
            # a header only, at its original size, so texture coordinates stay right
            blob = struct.pack("<16sII4I", name, t["w"], t["h"], 0, 0, 0, 0)
        else:
            im, clear = shrunk[k]
            mips = []
            cur, cclear = im, clear
            for level in range(4):
                if level:
                    nw, nh = max(1, cur.width // 2), max(1, cur.height // 2)
                    cur = cur.resize((nw, nh), Image.BOX)
                    if cclear is not None:
                        cclear = cclear[::2, ::2][:nh, :nw]
                ind = np.array(cur.quantize(palette=palimg, dither=Image.Dither.NONE)).astype(np.uint8)
                ind[ind == 255] = 254
                if cclear is not None:
                    ind[cclear] = 255
                mips.append(ind.tobytes())
            # the header keeps the original size (texture coordinates and lightmaps
            # are worked out against it); a tag after it gives the size the mips
            # really have: "DSz\0", u16 width, u16 height, then the mips
            w0, h0 = im.width, im.height
            o = 48
            mofs = []
            for m in mips:
                mofs.append(o)
                o += len(m)
            blob = struct.pack("<16sII4I", name, t["w"], t["h"], *mofs) + b"DSz\0" + struct.pack("<HH", w0, h0) + b"".join(mips)
        while len(blob) % 4:
            blob += b"\0"
        newoffs.append(pos)
        blobs.append(blob)
        pos += len(blob)
    texlump = struct.pack("<i", count) + struct.pack("<%di" % count, *newoffs) + b"".join(blobs)

    # lighting: brightness in the map, colour beside it; HL's 255 is full, Quake's is about 140
    light = np.frombuffer(lump[L_LIGHT], np.uint8)
    n = len(light) // 3
    rgb = light[:n * 3].reshape(n, 3).astype(np.float32) * (140.0 / 255.0)
    mono = (rgb[:, 0] * 0.3 + rgb[:, 1] * 0.59 + rgb[:, 2] * 0.11).clip(0, 255).astype(np.uint8).tobytes()
    lit = b"QLIT" + struct.pack("<i", 1) + rgb.clip(0, 255).astype(np.uint8).tobytes()
    faces = bytearray(lump[L_FACES])
    for f in range(len(faces) // 20):
        lo = struct.unpack_from("<i", faces, f * 20 + 16)[0]
        if lo >= 0:
            struct.pack_into("<i", faces, f * 20 + 16, lo // 3)

    newl = list(lump)
    newl[L_TEX] = texlump
    newl[L_LIGHT] = mono
    newl[L_FACES] = bytes(faces)
    newl[L_CLIP] = b""
    body, dirs = b"", []
    pos = 4 + NUM_LUMPS * 8
    for b in newl:
        while len(b) % 4:
            b += b"\0"
        dirs.append((pos, len(b)))
        body += b
        pos += len(b)
    with open(out, "wb") as f:
        f.write(struct.pack("<i", 29))
        for o, l in dirs:
            f.write(struct.pack("<ii", o, l))
        f.write(body)
    base = os.path.splitext(out)[0]
    with open(base + ".pal", "wb") as f:
        f.write(bytes(palette))
    with open(base + ".lit", "wb") as f:
        f.write(lit)
    stored = sum(len(b) for b in blobs)
    print("%s: %d KB (was %d KB), %d textures at most %dpx (%d KB), %d missing%s" % (
        out, os.path.getsize(out) // 1024, len(d) // 1024, count, cap, stored // 1024, len(missing),
        (": " + " ".join(missing[:8])) if missing else ""))


main()
