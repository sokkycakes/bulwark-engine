#!/usr/bin/env python3
"""Bulwark engine - packs a game's sounds for the DS. ISC licence, see LICENSE.

    sndpack.py OUT.dsp SRC... [--maxrate 22050] [--maxrate-loop 11025]
               [--pcm8 GLOB] [--adpcm-min-snr 18] [--stream-kb 96]
               [--only-used] [--used-list FILE...] [--decls DIR...] [--keep GLOB]
               [--music OUTDIR [--music-stereo] [--music-rate 22050]]
               [--report] [--allow-repo] [--jobs N]
    sndpack.py --check FILE.dsp|FILE.dsm... [--against SRC...]

SRC is a game directory (its loose files beat its own .pk3/.pak files, which
go alphabetically, later winning, as src/common/fs.c searches them), a .pk3,
.pk3dir or .pak; later SRCs win, like game directories layered (base, valve,
coop). Every sound/**/*.wav|ogg|mp3 is packed under the key the server and the
DS name it by (docs/spec/link.md, "Sound"): FNV-1a 32 of the name in lower
case, '\\' as '/', without a leading "./", one leading "sound/" and a
.wav/.ogg/.mp3 ending. When one name has several files, a .wav beats an .ogg
beats an .mp3 (the PC asks for "x.wav" and lets "x.ogg" stand in,
src/sound/snd_mem.c find_and_decode), then the later SRC wins.

Formats on the DS (the hardware decodes all of them):
  0 PCM8, signed;
  2 IMA-ADPCM as the DS's sound hardware plays it: a u32 header {s16 value,
    u8 table index, u8 0}, then 4-bit codes, low nibble first; encoded against
    an exact model of melonDS 1.1's decoder (SPU.cpp NextSample_ADPCM, ported
    below as Adpcm), so what the tool measures is what melonDS plays;
  3 a stream: the same codes in blocks, which the ARM9 decodes into a ring
    (sound_nds.c): u32 blockframes, u32 nblocks, nblocks x {s16 value, u8 index,
    u8 0} (the decoder's state before the block's first frame), then the
    blocks, each blockframes/2 bytes (the last one padded).
Pack layout (.dsp, little-endian): see write_pack(). Music (.dsm): see
write_dsm().

The packs are the game's sounds: a pack made from Half-Life's sound/, hldata
or coop's zz_*.pk3 is private and never goes in git or a public package.
"""
import argparse
import fnmatch
import io
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

import numpy as np

# --- names -------------------------------------------------------------------------------------

EXTS = (".wav", ".ogg", ".mp3")
EXT_RANK = {".wav": 0, ".ogg": 1, ".mp3": 2}


def norm_name(name):
    """sv_link.c snd_norm: lower case, '\\' -> '/', no leading "./", one leading
    "sound/" dropped, a trailing .wav/.ogg/.mp3 dropped"""
    s = name.replace("\\", "/").lower()
    if s.startswith("./"):
        s = s[2:]
    if s.startswith("sound/"):
        s = s[6:]
    if len(s) > 4 and s[-4:] in EXTS:
        s = s[:-4]
    return s


def fnv1a(s):
    h = 2166136261
    for b in s.encode("latin1", "replace"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def snd_key(name):
    return fnv1a(norm_name(name))


def music_name(name):
    """the .dsm file a music name plays from: lower case, '\\' -> '/', no
    leading "./", no extension, '/' -> '_' ("music/track02" -> music_track02,
    "sound/bgm/x.mp3" -> sound_bgm_x)"""
    s = name.replace("\\", "/").lower()
    if s.startswith("./"):
        s = s[2:]
    if len(s) > 4 and s[-4:] in EXTS:
        s = s[:-4]
    return s.replace("/", "_") + ".dsm"


# --- sources -----------------------------------------------------------------------------------

class Layer:
    """one place files come from: a directory of loose files, a zip or a pak;
    files() maps a lower-case relative path to (display name, reader)"""

    def __init__(self, label, private):
        self.label, self.private, self._files = label, private, {}

    def files(self):
        return self._files


def readf(path):
    with open(path, "rb") as f:
        return f.read()


def dir_layer(root, label, private):
    L = Layer(label, private)
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            L._files[rel.lower()] = (rel, (lambda p=p: readf(p)))
    return L


def zip_layer(path, private):
    L = Layer(path, private)
    z = zipfile.ZipFile(path)
    # later entries with the same name win, as the archive's own order (fs.c)
    for info in z.infolist():
        if info.is_dir():
            continue
        L._files[info.filename.lower()] = (info.filename, (lambda i=info: z.read(i)))
    return L


def pak_layer(path, private):
    L = Layer(path, private)
    with open(path, "rb") as f:
        h = f.read(12)
        if h[:4] != b"PACK":
            raise SystemExit("%s: not a Quake .pak" % path)
        ofs, ln = struct.unpack_from("<II", h, 4)
        f.seek(ofs)
        d = f.read(ln)
    for i in range(ln // 64):
        name = d[i * 64:i * 64 + 56].split(b"\0")[0].decode("latin1")
        o, n = struct.unpack_from("<II", d, i * 64 + 56)

        def rd(o=o, n=n):
            with open(path, "rb") as f:
                f.seek(o)
                return f.read(n)
        L._files[name.lower()] = (name, rd)
    return L


PRIVATE_RE = re.compile(r"(^|/)(zz_[^/]*\.pk3|valve/sound|hldata)(/|$)", re.I)


def is_private(path):
    p = os.path.abspath(path).replace(os.sep, "/")
    return bool(PRIVATE_RE.search(p)) or bool(re.search(r"(^|/)valve(/|$)", p) and os.path.isdir(os.path.join(path, "sound")))


def src_layers(src):
    """the layers of one SRC, lowest first"""
    priv = is_private(src)
    low = src.lower()
    if os.path.isdir(src) and not low.endswith(".pk3dir"):
        out = []
        packs = sorted(f for f in os.listdir(src) if f.lower().endswith((".pk3", ".pak")))
        for f in packs:  # alphabetical, later wins; loose files beat them all
            p = os.path.join(src, f)
            out.append(pak_layer(p, priv or is_private(p)) if f.lower().endswith(".pak") else zip_layer(p, priv or is_private(p)))
        out.append(dir_layer(src, src, priv))
        for f in sorted(os.listdir(src)):
            if f.lower().endswith(".pk3dir") and os.path.isdir(os.path.join(src, f)):
                out.insert(len(out) - 1, dir_layer(os.path.join(src, f), os.path.join(src, f), priv))
        return out
    if os.path.isdir(src):
        return [dir_layer(src, src, priv)]
    if low.endswith(".pak"):
        return [pak_layer(src, priv)]
    if low.endswith(".pk3") or low.endswith(".zip"):
        return [zip_layer(src, priv)]
    raise SystemExit("%s: not a game directory, .pk3, .pk3dir or .pak" % src)


class Entry:
    def __init__(self, name, rel, ext, layer, reader, order):
        self.name, self.rel, self.ext, self.layer, self.reader, self.order = name, rel, ext, layer, reader, order


def collect_sounds(layers, prefixes=("sound/",)):
    """{norm name: Entry} of the files PC would load for each name"""
    best = {}
    for order, L in enumerate(layers):
        for low, (rel, rd) in L.files().items():
            if not low.startswith(prefixes) or low.endswith("/"):
                continue
            ext = low[-4:]
            if ext not in EXTS:
                continue
            n = norm_name(rel) if prefixes == ("sound/",) else low[:-4]
            e = Entry(n, rel, ext, L, rd, order)
            old = best.get(n)
            if old is None or (EXT_RANK[ext], -order) < (EXT_RANK[old.ext], -old.order):
                best[n] = e
    return best


# --- decoding ----------------------------------------------------------------------------------

class Pcm:
    def __init__(self, data, rate, loopstart, bits, channels):
        self.data = data          # float64 frames (mono or [n,2]) in s16 units
        self.rate = rate
        self.loopstart = loopstart
        self.bits = bits
        self.channels = channels


def decode_wav(buf):
    """snd_mem.c decode_wav: RIFF/WAVE fmt 1, 8/16 bit, 1-2 channels; the first
    point of a "cue " chunk (the last such chunk) is where it loops from"""
    if len(buf) < 12 or buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        return None
    p, end = 12, len(buf)
    fmt = ch = rate = bits = 0
    pcm, loop = None, -1
    while p + 8 <= end:
        cid = buf[p:p + 4]
        size = struct.unpack_from("<I", buf, p + 4)[0]
        body = p + 8
        if size > end - body:
            size = end - body
        if cid == b"fmt " and size >= 16:
            fmt, ch, rate = struct.unpack_from("<HHI", buf, body)
            bits = struct.unpack_from("<H", buf, body + 14)[0]
        elif cid == b"data":
            pcm = (body, size)
        elif cid == b"cue " and size >= 28 and struct.unpack_from("<I", buf, body)[0] > 0:
            loop = struct.unpack_from("<I", buf, body + 4 + 20)[0]
        p = body + size + (size & 1)
    if fmt != 1 or pcm is None or bits not in (8, 16) or not 1 <= ch <= 2 or rate <= 0:
        return None
    o, n = pcm
    bps = bits // 8
    frames = n // (bps * ch)
    if bits == 8:
        a = (np.frombuffer(buf, np.uint8, frames * ch, o).astype(np.float64) - 128.0) * 256.0
    else:
        a = np.frombuffer(buf, "<i2", frames * ch, o).astype(np.float64)
    if ch == 2:
        a = a.reshape(frames, 2)
    return Pcm(a, rate, loop if 0 <= loop < frames else -1, bits, ch)


def decode_ffmpeg(buf, what):
    """.ogg/.mp3: decoded by ffmpeg into a WAV, read by decode_wav"""
    ff = shutil.which("ffmpeg")
    if not ff:
        warn("%s: no ffmpeg on PATH, skipped" % what)
        return None
    r = subprocess.run([ff, "-v", "error", "-i", "pipe:0", "-f", "wav", "-acodec", "pcm_s16le", "pipe:1"],
                       input=buf, capture_output=True)
    if r.returncode or not r.stdout:
        warn("%s: ffmpeg failed: %s" % (what, r.stderr.decode(errors="replace").strip()[:200]))
        return None
    pcm = decode_wav(r.stdout)
    if pcm is not None:
        pcm.loopstart = -1
        if pcm.channels > 2:
            pcm.data = pcm.data[:, :2]
    return pcm


def decode_any(buf, what):
    """snd_mem.c decode_file: by content, not by name"""
    if buf[:4] == b"OggS":
        return decode_ffmpeg(buf, what)
    if buf[:4] == b"RIFF":
        return decode_wav(buf)
    if buf[:3] == b"ID3" or (len(buf) >= 2 and buf[0] == 0xFF and (buf[1] & 0xE0) == 0xE0):
        return decode_ffmpeg(buf, what)
    return None


# --- resampling --------------------------------------------------------------------------------

def resample(x, src, dst, taps=32, beta=8.0, loopstart=-1):
    """windowed-sinc (Kaiser) resampler: `taps` zero crossings of the output
    rate's sinc a side; cutoff 0.97 of the output's Nyquist. A looping
    sample's tail is filtered as if it ran on into its loop."""
    if src == dst:
        return x.copy()
    ratio = dst / src
    fc = min(1.0, ratio) * 0.97          # cutoff, in cycles per input sample x2
    half = int(math.ceil(taps / fc))      # input samples a side
    n_out = int(math.floor(len(x) * ratio))
    pre = np.zeros(half + 1)
    if loopstart >= 0 and len(x) - loopstart > 0:
        seg = x[loopstart:]
        post = np.tile(seg, half // len(seg) + 2)[:half + 2]
    else:
        post = np.zeros(half + 2)
    xp = np.concatenate([pre, x, post])
    out = np.empty(n_out)
    k = np.arange(-half + 1, half + 1)
    step = 4096
    for s in range(0, n_out, step):
        n = np.arange(s, min(n_out, s + step))
        t = n / ratio                     # position in input samples
        i0 = np.floor(t).astype(np.int64)
        idx = i0[:, None] + k[None, :]    # input indices
        d = idx - t[:, None]              # distance in input samples
        w = fc * np.sinc(fc * d) * kaiser(d / (half + 1), beta)
        out[s:s + len(n)] = np.sum(xp[idx + half + 1] * w, axis=1) / np.sum(w, axis=1)
    return out


def kaiser(u, beta):
    """Kaiser window at u in [-1, 1] (0 outside)"""
    u = np.clip(u, -1.0, 1.0)
    return np.i0(beta * np.sqrt(1.0 - u * u)) / np.i0(beta)


# --- the DS's ADPCM (melonDS 1.1 src/SPU.cpp) -------------------------------------------------

ADPCM_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)
ADPCM_TABLE = (
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133,
    0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462,
    0x7FFF)
# DIFF[idx][c]: SPU.cpp's shift-add diff for the magnitude bits c (0..7)
DIFF = tuple(tuple((v >> 3) + ((v >> 2) if c & 1 else 0) + ((v >> 1) if c & 2 else 0) + (v if c & 4 else 0)
                   for c in range(8)) for v in ADPCM_TABLE)
NEXT_IDX = tuple(tuple(min(88, max(0, i + ADPCM_INDEX[c])) for c in range(8)) for i in range(89))


def adpcm_step(val, idx, code):
    """one code through SPU.cpp NextSample_ADPCM: the new (val, idx)"""
    d = DIFF[idx][code & 7]
    if code & 8:
        val -= d
        if val < -0x7FFF:
            val = -0x7FFF
    else:
        val += d
        if val > 0x7FFF:
            val = 0x7FFF
    return val, NEXT_IDX[idx][code & 7]


def adpcm_encode(x, val, idx, block=0):
    """greedy encoder against the exact decoder: each code is the one of the
    16 whose decoded value is nearest the target. x: int sequence (s16).
    Returns (codes bytearray of one code a byte, decoded list, states) where
    states[b] is (val, idx) before frame b*block when block > 0."""
    n = len(x)
    codes = bytearray(n)
    dec = [0] * n
    states = []
    D, NX = DIFF, NEXT_IDX
    for i in range(n):
        if block and i % block == 0:
            states.append((val, idx))
        t = x[i]
        dt = D[idx]
        delta = t - val
        if delta >= 0:
            # the magnitude whose diff is nearest delta (diffs rise with c)
            best_c, best_e = 0, abs(delta - dt[0])
            for c in range(1, 8):
                e = abs(delta - dt[c])
                if e < best_e:
                    best_c, best_e = c, e
                elif dt[c] > delta:
                    break
            nv = val + dt[best_c]
            if nv > 0x7FFF:
                nv = 0x7FFF
            # a negative code can be nearer only when delta is under step/8
            alt = val - dt[0]
            if alt < -0x7FFF:
                alt = -0x7FFF
            if abs(t - alt) < abs(t - nv):
                code, nv = 8, alt
            else:
                code = best_c
        else:
            m = -delta
            best_c, best_e = 0, abs(m - dt[0])
            for c in range(1, 8):
                e = abs(m - dt[c])
                if e < best_e:
                    best_c, best_e = c, e
                elif dt[c] > m:
                    break
            nv = val - dt[best_c]
            if nv < -0x7FFF:
                nv = -0x7FFF
            alt = val + dt[0]
            if alt > 0x7FFF:
                alt = 0x7FFF
            if abs(t - alt) < abs(t - nv):
                code, nv = 0, alt
            else:
                code = 8 | best_c
        codes[i] = code
        dec[i] = nv
        val = nv
        idx = NX[idx][code & 7]
    return codes, dec, states, (val, idx)


def pick_start_index(x):
    """the table index to start from: the one that encodes the first 64
    frames best (searched over every 4th index)"""
    head = x[:64]
    if not head:
        return 0
    best = (None, 0)
    for i in range(0, 89, 4):
        _, dec, _, _ = adpcm_encode(head, head[0], i)
        e = sum((a - b) * (a - b) for a, b in zip(head, dec))
        if best[0] is None or e < best[0]:
            best = (e, i)
    return best[1]


def pack_nibbles(codes):
    """one code a byte -> two a byte, low nibble first (odd count: a 0 code pads)"""
    if len(codes) & 1:
        codes = codes + b"\0"
    a = np.frombuffer(bytes(codes), np.uint8)
    return ((a[0::2] & 15) | ((a[1::2] & 15) << 4)).astype(np.uint8).tobytes()


def unpack_nibbles(data, n):
    a = np.frombuffer(data, np.uint8)
    out = np.empty(len(a) * 2, np.uint8)
    out[0::2] = a & 15
    out[1::2] = a >> 4
    return out[:n]


def adpcm_decode(data, frames):
    """a fmt-2 sample (header + codes) decoded as SPU.cpp does, first pass"""
    val = struct.unpack_from("<h", data, 0)[0]
    idx = min(88, data[2] & 0x7F)
    out = [0] * frames
    for i, c in enumerate(unpack_nibbles(data[4:], frames).tolist()):
        val, idx = adpcm_step(val, idx, c)
        out[i] = val
    return out


class MelonChannel:
    """melonDS 1.1 SPUChannel for PCM8/ADPCM, sample by sample (Start,
    NextSample_PCM8, NextSample_ADPCM, the run gate of Run<>): the values
    CurSample takes, one a timer period. loop_words/len_words are the
    SOUNDxPNT/SOUNDxLEN words; repeat 1 loops, 2 one-shot."""

    def __init__(self, mem, fmt, loop_words, len_words, repeat):
        self.mem, self.fmt, self.repeat = mem, fmt, repeat
        self.LoopPos, self.Length = loop_words * 4, len_words * 4   # SPU.cpp keeps bytes
        self.on = (self.LoopPos + self.Length) >= 16                # Run<>: shorter never plays
        self.Pos = -3                                                # Start(): -3 but for PSG/noise
        self.Cur = 0
        self.rd = 0                                                  # FIFO read offset (bytes)
        self.val = self.idx = self.vloop = self.iloop = 0
        self.byte = 0

    def _read(self, n):
        # the FIFO streams LoopPos+Length bytes, then from LoopPos again when looping
        total = self.LoopPos + self.Length
        if self.rd >= total and self.repeat & 1:
            self.rd = self.LoopPos + (self.rd - total)
        b = self.mem[self.rd:self.rd + n]
        self.rd += n
        return int.from_bytes(b, "little")

    def tick(self):
        if not self.on:
            return 0
        self.Pos += 1
        if self.fmt == 0:
            if self.Pos < 0:
                return self.Cur
            if self.Pos >= self.LoopPos + self.Length:
                if self.repeat & 1:
                    self.Pos = self.LoopPos
                    self.rd = self.LoopPos
                elif self.repeat & 2:
                    self.Cur, self.on = 0, False
                    return 0
            v = self._read(1)
            self.Cur = (v - 256 if v > 127 else v) << 8
            return self.Cur
        # ADPCM
        if self.Pos < 8:
            if self.Pos == 0:
                h = self._read(4)
                self.val = h & 0xFFFF
                self.val = self.val - 0x10000 if self.val & 0x8000 else self.val
                self.idx = min(88, (h >> 16) & 0x7F)
                self.vloop, self.iloop = self.val, self.idx
            return self.Cur
        if (self.Pos >> 1) >= self.LoopPos + self.Length:
            if self.repeat & 1:
                self.Pos = self.LoopPos << 1
                self.val, self.idx = self.vloop, self.iloop
                self.rd = self.LoopPos
                self.byte = self._read(1)
            elif self.repeat & 2:
                self.Cur, self.on = 0, False
                return 0
        else:
            if not (self.Pos & 1):
                self.byte = self._read(1)
            else:
                self.byte >>= 4
            self.val, self.idx = adpcm_step(self.val, self.idx, self.byte & 15)
            if self.Pos == (self.LoopPos << 1):
                self.vloop, self.iloop = self.val, self.idx
        self.Cur = self.val
        return self.Cur


# --- conversion --------------------------------------------------------------------------------

def snr_db(ref, dec):
    ref = np.asarray(ref, np.float64)
    dec = np.asarray(dec, np.float64)
    s = float(np.sum(ref * ref))
    e = float(np.sum((ref - dec) ** 2))
    if s == 0.0:
        return float("inf") if e == 0.0 else float("-inf")
    if e == 0.0:
        return float("inf")
    return 10.0 * math.log10(s / e)


FMT_PCM8, FMT_PCM16, FMT_ADPCM, FMT_STREAM = 0, 1, 2, 3
FMT_NAMES = {0: "pcm8", 1: "pcm16", 2: "adpcm", 3: "stream"}
WORD_FRAMES = {FMT_PCM8: 4, FMT_ADPCM: 8}
MIN_BYTES = 16          # SPU.cpp Run<>: LoopPos+Length under 16 bytes never plays
BLOCK_FRAMES = 2048


class Sample:
    pass


def to_mono(pcm):
    if pcm.data.ndim == 2:
        return pcm.data.mean(axis=1)
    return pcm.data


def prepare(pcm, maxrate, maxrate_loop):
    """mono, at the rate the DS plays it: the reference the encoders aim at
    (float, s16 units), its rate and its loop start in its frames"""
    x = to_mono(pcm)
    loop = pcm.loopstart
    cap = maxrate_loop if loop >= 0 else maxrate
    rate = pcm.rate
    if rate > cap:
        x = resample(x, rate, cap, loopstart=loop)
        if loop >= 0:
            loop = int(round(loop * cap / rate))
        rate = cap
    return x, rate, loop


def align_frames(x, loop, q):
    """word-aligned: the loop start moved up to a multiple of q frames, the end
    cut to one; a one-shot shorter than MIN_BYTES is padded with silence, a
    loop by repeating its loop. Returns (x, loop, loopshift, endtrim)."""
    n = len(x)
    shift = 0
    if loop >= 0:
        nl = -(-loop // q) * q
        shift = nl - loop
        loop = nl
        if loop >= n:
            loop, shift = -1, 0
    end = n // q * q
    trim = n - end
    x = x[:end]
    if loop >= 0 and loop >= len(x):
        loop = -1
    return x, loop, shift, trim


def min_frames(fmt):
    return {FMT_PCM8: MIN_BYTES, FMT_ADPCM: (MIN_BYTES - 4) * 2}[fmt]


def pad_min(x, loop, fmt):
    need = min_frames(fmt)
    if len(x) >= need:
        return x
    if loop >= 0:
        seg = x[loop:]
        while len(x) < need:
            x = np.concatenate([x, seg])
        return x
    return np.concatenate([x, np.zeros(need - len(x))])


def quantize(x):
    return np.clip(np.rint(x), -32768, 32767).astype(np.int64)


def enc_pcm8(xq):
    """signed 8-bit: rounded, saturated"""
    q = np.clip(np.floor((xq + 128) / 256.0), -128, 127).astype(np.int64)
    return q.astype(np.int8).tobytes(), (q * 256)


def enc_adpcm(xq):
    xs = [int(v) for v in np.clip(xq, -0x7FFF, 0x7FFF)]
    idx = pick_start_index(xs)
    val = xs[0] if xs else 0
    codes, dec, _, _ = adpcm_encode(xs, val, idx)
    data = struct.pack("<hBB", val, idx, 0) + pack_nibbles(codes)
    return data, np.array(dec, np.int64)


def enc_stream(xq, block=BLOCK_FRAMES):
    """fmt 3: u32 blockframes, u32 nblocks, the states, the blocks"""
    xs = [int(v) for v in np.clip(xq, -0x7FFF, 0x7FFF)]
    idx = pick_start_index(xs)
    val = xs[0] if xs else 0
    codes, dec, states, _ = adpcm_encode(xs, val, idx, block)
    nb = len(states)
    out = bytearray(struct.pack("<II", block, nb))
    for v, i in states:
        out += struct.pack("<hBB", v, i, 0)
    body = pack_nibbles(codes)
    out += body + b"\0" * (nb * block // 2 - len(body))
    return bytes(out), np.array(dec, np.int64)


def convert(name, pcm, opt):
    """one sound -> Sample (fmt, rate, frames, loopstart, data, report fields)"""
    s = Sample()
    s.name, s.key = name, fnv1a(name)
    s.src_rate, s.src_bits, s.src_ch, s.src_frames = pcm.rate, pcm.bits, pcm.channels, len(pcm.data)
    s.src_loop = pcm.loopstart
    x, rate, loop = prepare(pcm, opt.maxrate, opt.maxrate_loop)
    s.rate = rate
    forced8 = any(fnmatch.fnmatch(name, g) or fnmatch.fnmatch(name + ".wav", g) for g in (opt.pcm8 or []))
    stream_bytes = 4 + len(x) // 2
    s.snr_adpcm = s.snr_pcm8 = None
    if not forced8 and stream_bytes > opt.stream_kb * 1024:
        # long: a stream, decoded by the ARM9 (any length, any loop point)
        xq = quantize(x)
        s.loopshift = s.endtrim = 0
        s.fmt, s.flags = FMT_STREAM, 2 | (1 if loop >= 0 else 0)
        s.data, dec = enc_stream(xq)
        s.frames, s.loopstart = len(xq), loop
        s.snr_adpcm = snr_db(xq, dec)
        s.ref = xq
        return s
    # PCM8 and hardware ADPCM: decide by ADPCM's SNR
    xa, la, sha, tra = align_frames(x, loop, WORD_FRAMES[FMT_ADPCM])
    xa = pad_min(xa, la, FMT_ADPCM)
    qa = quantize(xa)
    da, deca = enc_adpcm(qa)
    s.snr_adpcm = snr_db(qa, deca)
    x8, l8, sh8, tr8 = align_frames(x, loop, WORD_FRAMES[FMT_PCM8])
    x8 = pad_min(x8, l8, FMT_PCM8)
    q8 = quantize(x8)
    d8, dec8 = enc_pcm8(q8)
    s.snr_pcm8 = snr_db(q8, dec8)
    # PCM8 when named, when small, or when ADPCM is under the floor and PCM8 is
    # better (a source below 8 bits' resolution, such as base's near-silent
    # placeholders, rounds to 0 in PCM8: ADPCM keeps more of it)
    if forced8 or len(d8) < 1024 or (s.snr_adpcm < opt.adpcm_min_snr and s.snr_pcm8 > s.snr_adpcm):
        s.fmt, s.data, s.frames, s.loopstart, s.loopshift, s.endtrim, s.ref = FMT_PCM8, d8, len(q8), l8, sh8, tr8, q8
        s.why = "forced" if forced8 else ("small" if len(d8) < 1024 else "snr")
    else:
        s.fmt, s.data, s.frames, s.loopstart, s.loopshift, s.endtrim, s.ref = FMT_ADPCM, da, len(qa), la, sha, tra, qa
        s.why = ""
    s.flags = 1 if s.loopstart >= 0 else 0
    return s


# --- pack file ---------------------------------------------------------------------------------

DIR_ENT = 24
PAGE = 512
PER_PAGE = PAGE // DIR_ENT     # 21
HDR = 32
SHDR = 21                      # sample header before the name


def align(n, a):
    return (n + a - 1) // a * a


def write_pack(path, samples):
    """
    header (32 B): "BWSP", u16 version 1, u16 flags 0, u32 count, u32 dir_off,
      u32 index_off, u32 index_count, u32 data_off, u32 reserved 0
    dir (at dir_off, 512-aligned): count x {u32 key, u32 offset, u8 fmt, u8 flags,
      u16 rate, u32 frames, s32 loopstart, u32 bytes} sorted by key, 21 a
      512-byte page, each page padded with zeros
    index (at index_off): index_count x u32, the first key of each dir page
    samples (from data_off, each 4-aligned): u32 key, u8 fmt, u8 flags,
      u16 rate, u32 frames, s32 loopstart, u32 bytes, u8 namelen, name, zeros
      to 4, then `bytes` of data
    frames: frames the sample plays (fmt 2: after the header word); loopstart in
    frames (-1: none; fmt 0: a multiple of 4, fmt 2: of 8); bytes: data length
    (fmt 2 includes the header word).
    """
    ss = sorted(samples, key=lambda s: s.key)
    keys = [s.key for s in ss]
    if len(set(keys)) != len(keys):
        raise SystemExit("key collision in the pack (refused)")
    count = len(ss)
    npages = (count + PER_PAGE - 1) // PER_PAGE
    dir_off = PAGE
    index_off = dir_off + npages * PAGE
    data_off = align(index_off + npages * 4, PAGE)
    body = bytearray()
    offs = {}
    for s in sorted(ss, key=lambda s: s.name):
        o = data_off + len(body)
        offs[s.key] = o
        nm = s.name.encode("latin1")[:255]
        h = struct.pack("<IBBHIiIB", s.key, s.fmt, s.flags, s.rate, s.frames, s.loopstart, len(s.data), len(nm)) + nm
        h += b"\0" * (align(len(h), 4) - len(h))
        body += h + s.data
        body += b"\0" * (align(len(body), 4) - len(body))
    out = bytearray(struct.pack("<4sHHIIIIII", b"BWSP", 1, 0, count, dir_off, index_off, npages, data_off, 0))
    out += b"\0" * (dir_off - len(out))
    for p in range(npages):
        page = bytearray()
        for s in ss[p * PER_PAGE:(p + 1) * PER_PAGE]:
            page += struct.pack("<IIBBHIiI", s.key, offs[s.key], s.fmt, s.flags, s.rate, s.frames, s.loopstart, len(s.data))
        out += page + b"\0" * (PAGE - len(page))
    for p in range(npages):
        out += struct.pack("<I", ss[p * PER_PAGE].key)
    out += b"\0" * (data_off - len(out))
    out += body
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return len(out)


def read_pack(buf):
    """-> (header dict, [entry dicts]) or raises ValueError"""
    if len(buf) < HDR or buf[:4] != b"BWSP":
        raise ValueError("not a BWSP pack")
    _, ver, flags, count, dir_off, index_off, nidx, data_off, _ = struct.unpack_from("<4sHHIIIIII", buf, 0)
    hd = dict(version=ver, flags=flags, count=count, dir_off=dir_off, index_off=index_off, index_count=nidx, data_off=data_off)
    ents = []
    for i in range(count):
        o = dir_off + (i // PER_PAGE) * PAGE + (i % PER_PAGE) * DIR_ENT
        if o + DIR_ENT > len(buf):
            raise ValueError("dir entry %d past the end" % i)
        k, off, fmt, fl, rate, frames, loop, nbytes = struct.unpack_from("<IIBBHIiI", buf, o)
        ents.append(dict(key=k, offset=off, fmt=fmt, flags=fl, rate=rate, frames=frames, loopstart=loop, bytes=nbytes))
    return hd, ents


def sample_at(buf, e):
    """the sample header and data an entry points at"""
    o = e["offset"]
    k, fmt, fl, rate, frames, loop, nbytes, nl = struct.unpack_from("<IBBHIiIB", buf, o)
    name = buf[o + SHDR:o + SHDR + nl].decode("latin1")
    dp = o + align(SHDR + nl, 4)
    return dict(key=k, fmt=fmt, flags=fl, rate=rate, frames=frames, loopstart=loop, bytes=nbytes, name=name,
                data_pos=dp), buf[dp:dp + nbytes]


def stream_decode(data, frames):
    """fmt 3 / one .dsm channel's blocks: decoded continuously; also checks
    each block's state against the table. -> (decoded, bad states)"""
    block, nb = struct.unpack_from("<II", data, 0)
    states = [struct.unpack_from("<hBB", data, 8 + 4 * b) for b in range(nb)]
    codes = unpack_nibbles(data[8 + 4 * nb:], frames).tolist()
    return _stream_run(codes, states, block, frames)


def _stream_run(codes, states, block, frames):
    out = [0] * frames
    bad = 0
    val, idx = (states[0][0], states[0][1]) if states else (0, 0)
    for i in range(frames):
        if i % block == 0:
            b = i // block
            sv, si = states[b][0], states[b][1]
            if (sv, si) != (val, idx) and i:
                bad += 1
            val, idx = sv, si
        val, idx = adpcm_step(val, idx, codes[i])
        out[i] = val
    return out, bad


# --- music (.dsm) ------------------------------------------------------------------------------

def write_dsm(path, pcm, rate, stereo):
    """
    "BWMS", u16 version 1, u8 channels (1/2), u8 0, u32 rate, u32 frames,
    s32 loopstart (-1), u32 blockframes, u32 nblocks (28 B); then the states
    nblocks x channels x {s16 value, u8 index, u8 0} (block-major); then the
    blocks, each channel's blockframes/2 bytes in turn (the last padded).
    Returns (bytes written, SNR per channel).
    """
    x = pcm.data
    if x.ndim == 1:
        chans = [x]
    elif stereo:
        chans = [x[:, 0], x[:, 1]]
    else:
        chans = [x.mean(axis=1)]
    if pcm.rate != rate:
        chans = [resample(c, pcm.rate, rate) for c in chans]
    frames = min(len(c) for c in chans)
    nb = (frames + BLOCK_FRAMES - 1) // BLOCK_FRAMES
    enc, snrs = [], []
    for c in chans:
        xq = quantize(c[:frames])
        xs = [int(v) for v in np.clip(xq, -0x7FFF, 0x7FFF)]
        idx = pick_start_index(xs)
        codes, dec, states, _ = adpcm_encode(xs, xs[0] if xs else 0, idx, BLOCK_FRAMES)
        body = pack_nibbles(codes)
        body += b"\0" * (nb * BLOCK_FRAMES // 2 - len(body))
        enc.append((states, body))
        snrs.append(snr_db(xq, dec))
    out = bytearray(struct.pack("<4sHBBIIiII", b"BWMS", 1, len(chans), 0, rate, frames, -1, BLOCK_FRAMES, nb))
    for b in range(nb):
        for states, _ in enc:
            out += struct.pack("<hBB", states[b][0], states[b][1], 0)
    hb = BLOCK_FRAMES // 2
    for b in range(nb):
        for _, body in enc:
            out += body[b * hb:(b + 1) * hb]
    with open(path + ".tmp", "wb") as f:
        f.write(out)
    os.replace(path + ".tmp", path)
    return len(out), snrs


def check_dsm(path):
    buf = readf(path)
    errs = []
    if buf[:4] != b"BWMS":
        return ["not a BWMS file"], None
    _, ver, ch, _, rate, frames, loop, block, nb = struct.unpack_from("<4sHBBIIiII", buf, 0)
    if ver != 1 or ch not in (1, 2) or block % 8 or nb != (frames + block - 1) // block:
        errs.append("bad header ver=%d ch=%d block=%d nblocks=%d frames=%d" % (ver, ch, block, nb, frames))
        return errs, None
    st0 = 28
    data0 = st0 + nb * ch * 4
    hb = block // 2
    if len(buf) != data0 + nb * ch * hb:
        errs.append("size %d, expected %d" % (len(buf), data0 + nb * ch * hb))
        return errs, None
    for c in range(ch):
        states = [struct.unpack_from("<hBB", buf, st0 + (b * ch + c) * 4) for b in range(nb)]
        body = b"".join(buf[data0 + (b * ch + c) * hb:data0 + (b * ch + c + 1) * hb] for b in range(nb))
        _, bad = _stream_run(unpack_nibbles(body, frames).tolist(), states, block, frames)
        if bad:
            errs.append("channel %d: %d block states differ from the decoder's" % (c, bad))
    return errs, dict(channels=ch, rate=rate, frames=frames, seconds=frames / rate, bytes=len(buf))


# --- which sounds are used (--only-used) -------------------------------------------------------

KEEP_ALWAYS = ("misc/talk", "common/null", "common/wpn_hudon", "common/wpn_hudoff", "common/wpn_moveselect")
STR_RE = re.compile(rb"[\x20-\x7e]{4,}")
TOK_RE = re.compile(r"[^\s\"';{}()]+")


def strings_of(buf):
    for m in STR_RE.finditer(buf):
        for t in TOK_RE.findall(m.group().decode("latin1")):
            yield t


def parse_sndshd(text):
    """{shader name (lower): [sample names]}, the way SoundDef_Precache reads
    a decl: a name, then a { } body whose "sample" lines name the samples"""
    out = {}
    text = re.sub(r"//[^\n]*", "", text)
    for m in re.finditer(r"([^\s{}]+)\s*\{([^}]*)\}", text):
        out[m.group(1).lower()] = re.findall(r"(?m)^\s*sample\s+(\S+)", m.group(2))
    return out


def sentence_words(text):
    """sentences.txt: NAME word word...; ',' is vox/comma; a word without a
    path takes the last path seen in its sentence (Sentences_ProcessSample),
    "vox" at the start. Pitch/volume marks like "(p110)" are dropped [hyp: the
    client's tokenizer splits them off]."""
    out = set()
    for line in text.splitlines():
        line = line.split("//")[0].replace(",", " vox/comma.wav ")
        toks = line.split()
        if len(toks) < 2:
            continue
        path = "vox"
        for w in toks[1:]:
            w = re.sub(r"\([^)]*\)", "", w).rstrip(".")
            if not w:
                continue
            if "/" in w:
                path = w.split("/")[0]
                out.add(w)
            else:
                out.add(path + "/" + w)
    return out


SCAN_RE = re.compile(r"^(progs\.dat|csprogs\.dat|hud\.dat|progs/[^/]+\.dat|maps/.*\.(bsp|ent)|models/.*\.mdl|"
                     r"decls/.*\.(def|sndshd)|scripts/.*\.(txt|def)|sound/sentences\.txt)$")


def used_names(layers, used_lists, keep_globs, available):
    """the normalised names something names: decls (shaders and entityDefs),
    sentences, progs strings, BSP entities, model events, S's LINKSNDUSED
    dump, the always-kept list and --keep. Returns (set, {source: count},
    names the decls name that no SRC has)."""
    shaders = {}
    texts = []
    sentences = set()
    for L in layers:
        for low, (rel, rd) in L.files().items():
            if not SCAN_RE.match(low):
                continue
            buf = rd()
            if low.endswith(".sndshd"):
                shaders.update(parse_sndshd(buf.decode("latin1")))
            elif low.endswith("sentences.txt"):
                sentences |= sentence_words(buf.decode("latin1"))
            texts.append((low, buf))
    used, why = set(), {}

    def add(n, src):
        k = norm_name(n)
        if k not in used:
            used.add(k)
            if k in available:  # count what it adds to the pack
                why[src] = why.get(src, 0) + 1

    missing = set()
    for sh, samples in shaders.items():
        for smp in samples:
            if norm_name(smp) not in available:
                missing.add(norm_name(smp))
    referenced_shaders = set()
    for low, buf in texts:
        src = low.split("/")[0] if "/" in low else low
        for t in strings_of(buf):
            tl = t.lower()
            if norm_name(t) in available:
                add(t, src)
            if tl in shaders:
                referenced_shaders.add(tl)
            m = re.match(r"^(?:play2?|playvol)\s*(\S+)", tl)
            if m:
                add(m.group(1), src)
    # every shader's samples: the server sends what the decls name, and the
    # decls are loaded whole (SoundDef_Init reads every .sndshd)
    for sh, samples in shaders.items():
        for smp in samples:
            add(smp, "decls")
    for w in sentences:
        add(w, "sentences")
    for f in used_lists:
        with open(f, encoding="latin1") as fh:
            lines = fh.read().splitlines()
        for line in lines:
            p = line.split()
            if "LINKSNDUSED" in p:
                i = p.index("LINKSNDUSED")
                if len(p) > i + 2:
                    add(p[i + 2], "used-list")
            elif len(p) == 1:
                add(p[0], "used-list")
    for n in KEEP_ALWAYS:
        add(n, "always")
    for g in keep_globs:
        for n in available:
            if fnmatch.fnmatch(n, g) or fnmatch.fnmatch(n + ".wav", g):
                add(n, "keep")
    return used, why, missing


# --- main --------------------------------------------------------------------------------------

def warn(msg):
    print("sndpack: " + msg, file=sys.stderr)


def git_worktree_of(path):
    d = os.path.dirname(os.path.abspath(path)) or "."
    while not os.path.isdir(d):
        d = os.path.dirname(d)
    r = subprocess.run(["git", "-C", d, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def fmt_db(v):
    if v is None:
        return "-"
    if v == float("inf"):
        return "inf"
    if v == float("-inf"):
        return "-inf"
    return "%.1f" % v


def convert_one(args):
    name, rel, buf, opt = args
    pcm = decode_any(buf, rel)
    if pcm is None:
        return name, None
    return name, convert(name, pcm, opt)


def build(opt):
    for o in [opt.out] + ([opt.music] if opt.music else []):
        wt = git_worktree_of(o if o != opt.music else os.path.join(o, "x"))
        if wt and not opt.allow_repo:
            raise SystemExit("sndpack: %s is in the git work tree %s: packs are game data (refused; --allow-repo)" % (o, wt))
    layers = []
    for s in opt.src:
        layers += src_layers(s)
    if not layers:
        raise SystemExit("sndpack: no sources")
    found = collect_sounds(layers)
    private = sorted({e.layer.label for e in found.values() if e.layer.private})
    # collisions: different names, same key
    bykey = {}
    for n in found:
        bykey.setdefault(fnv1a(n), []).append(n)
    coll = [v for v in bykey.values() if len(v) > 1]
    if coll:
        for v in coll:
            warn("key collision: %s" % ", ".join(v))
        raise SystemExit(1)
    names = sorted(found)
    missing = set()
    if opt.only_used or opt.decls:
        scan = list(layers)
        for d in opt.decls or []:
            scan += src_layers(d)
        if opt.only_used and not opt.used_list and not scan:
            raise SystemExit("sndpack: --only-used needs --used-list or game dirs to scan")
        used, why, missing = used_names(scan, opt.used_list or [], opt.keep or [], set(found))
        if opt.only_used:
            names = [n for n in names if n in used]
            print("only-used: %d of %d sounds (%s)" % (len(names), len(found),
                  ", ".join("%s %d" % kv for kv in sorted(why.items()))))
    jobs = []
    for n in names:
        e = found[n]
        jobs.append((n, e.rel, e.reader(), opt))
    if opt.jobs > 1 and len(jobs) > 1:
        import multiprocessing as mp
        with mp.Pool(opt.jobs) as pool:
            results = pool.map(convert_one, jobs, chunksize=1)
    else:
        results = [convert_one(j) for j in jobs]
    samples = []
    for n, s in results:
        if s is None:
            warn("%s: not a PCM wav, ogg vorbis or mp3 file (skipped)" % found[n].rel)
            continue
        s.src = found[n].rel
        samples.append(s)
    size = write_pack(opt.out, samples)
    tot = {}
    for s in samples:
        t = tot.setdefault(s.fmt, [0, 0, 0.0])
        t[0] += 1
        t[1] += len(s.data)
        t[2] += s.frames / s.rate
    print("%s: %d sounds, %d bytes (%.1f KB)%s" % (opt.out, len(samples), size, size / 1024,
          " PRIVATE (from %s): never in git or a public package" % ", ".join(private) if private else ""))
    for f, (c, b, sec) in sorted(tot.items()):
        print("  %-6s %4d sounds %9d bytes %7.1f s" % (FMT_NAMES[f], c, b, sec))
    if missing:
        print("decls name %d samples no SRC has%s" % (len(missing), ": " + " ".join(sorted(missing)[:40]) +
              (" ..." if len(missing) > 40 else "") if opt.report else ""))
    if opt.report:
        report(samples)
    if opt.music:
        music(opt, layers)
    return 0


def report(samples):
    print("%-40s %6s %3s %2s  %-6s %5s %7s %6s %6s %4s %4s %s" % (
        "name", "srcHz", "bit", "ch", "fmt", "Hz", "bytes", "adpcm", "pcm8", "lsh", "trim", "why"))
    for s in sorted(samples, key=lambda s: s.name):
        print("%-40s %6d %3d %2d  %-6s %5d %7d %6s %6s %4s %4d %s" % (
            s.name[:40], s.src_rate, s.src_bits, s.src_ch, FMT_NAMES[s.fmt], s.rate, len(s.data),
            fmt_db(s.snr_adpcm), fmt_db(s.snr_pcm8), s.loopshift if s.loopstart >= 0 else "-", s.endtrim,
            getattr(s, "why", "")))
    a = [s.snr_adpcm for s in samples if s.fmt in (FMT_ADPCM, FMT_STREAM) and s.snr_adpcm not in (None, float("inf"))]
    if a:
        a.sort()
        print("ADPCM/stream SNR (dB, vs the resampled source): min %.1f  median %.1f  max %.1f  (n=%d)" % (
            a[0], a[len(a) // 2], a[-1], len(a)))


def music_one(args):
    rel, buf, out, rate, stereo = args
    pcm = decode_any(buf, rel)
    if pcm is None:
        return rel, out, None
    size, snrs = write_dsm(out, pcm, min(pcm.rate, rate), stereo)
    return rel, out, (size, len(pcm.data) / pcm.rate, snrs)


def music(opt, layers):
    os.makedirs(opt.music, exist_ok=True)
    found = collect_sounds(layers, prefixes=("music/", "sound/bgm/"))
    jobs = [(found[n].rel, found[n].reader(), os.path.join(opt.music, music_name(found[n].rel)), opt.music_rate,
             opt.music_stereo) for n in sorted(found)]
    if opt.jobs > 1 and len(jobs) > 1:
        import multiprocessing as mp
        with mp.Pool(opt.jobs) as pool:
            res = pool.map(music_one, jobs, chunksize=1)
    else:
        res = [music_one(j) for j in jobs]
    for rel, out, r in res:
        if r is None:
            warn("%s: not decodable (skipped)" % rel)
            continue
        size, sec, snrs = r
        print("%s: %d bytes, %.1f s, SNR %s dB" % (out, size, sec, "/".join(fmt_db(v) for v in snrs)))


def check(files, against, opt):
    """--check: every structural rule of the format, every sample decoded as
    melonDS plays it; with --against, SNR against the sources converted again"""
    nerr = 0
    ref = {}
    if against:
        layers = []
        for s in against:
            layers += src_layers(s)
        ref = collect_sounds(layers)
    for path in files:
        if path.lower().endswith(".dsm"):
            errs, info = check_dsm(path)
            for e in errs:
                print("%s: ERROR %s" % (path, e))
            nerr += len(errs)
            if info:
                print("%s: music, %d ch, %d Hz, %.1f s, %d bytes, %s" % (path, info["channels"], info["rate"],
                      info["seconds"], info["bytes"], "ok" if not errs else "%d errors" % len(errs)))
            continue
        buf = readf(path)
        errs = []
        try:
            hd, ents = read_pack(buf)
        except (ValueError, struct.error) as ex:
            print("%s: ERROR %s" % (path, ex))
            nerr += 1
            continue
        npages = (hd["count"] + PER_PAGE - 1) // PER_PAGE
        if hd["version"] != 1:
            errs.append("version %d" % hd["version"])
        if hd["index_count"] != npages:
            errs.append("index_count %d, %d pages" % (hd["index_count"], npages))
        if hd["dir_off"] % 4 or hd["data_off"] % 4:
            errs.append("unaligned dir/data offset")
        for p in range(min(npages, hd["index_count"])):
            k = struct.unpack_from("<I", buf, hd["index_off"] + 4 * p)[0]
            if k != ents[p * PER_PAGE]["key"]:
                errs.append("index page %d: %08x, the page starts with %08x" % (p, k, ents[p * PER_PAGE]["key"]))
            used = (min(hd["count"], (p + 1) * PER_PAGE) - p * PER_PAGE) * DIR_ENT
            pad = buf[hd["dir_off"] + p * PAGE + used:hd["dir_off"] + (p + 1) * PAGE]
            if any(pad):
                errs.append("dir page %d: padding not zero" % p)
        keys = [e["key"] for e in ents]
        if any(a >= b for a, b in zip(keys, keys[1:])):
            errs.append("dir not sorted by key (or a key twice)")
        spans = []
        stats = {}
        snrs = []
        for e in ents:
            tag = "%08x" % e["key"]
            try:
                h, data = sample_at(buf, e)
            except struct.error:
                errs.append("%s: sample header past the end" % tag)
                continue
            tag = "%s %s" % (tag, h["name"])
            if any(h[f] != e[f] for f in ("key", "fmt", "flags", "rate", "frames", "loopstart", "bytes")):
                errs.append("%s: sample header differs from the dir entry" % tag)
            if fnv1a(h["name"]) != e["key"] or norm_name(h["name"]) != h["name"]:
                errs.append("%s: the name doesn't hash to the key (or isn't normalised)" % tag)
            if e["offset"] % 4 or h["data_pos"] % 4 or e["offset"] < hd["data_off"]:
                errs.append("%s: unaligned or before data_off" % tag)
            if h["data_pos"] + e["bytes"] > len(buf):
                errs.append("%s: data past the end of the file" % tag)
                continue
            spans.append((e["offset"], h["data_pos"] + e["bytes"], tag))
            fmt, fr, ls, nb = e["fmt"], e["frames"], e["loopstart"], e["bytes"]
            if e["rate"] == 0 or fr == 0:
                errs.append("%s: rate or frames 0" % tag)
                continue
            if bool(e["flags"] & 1) != (ls >= 0) or ls >= fr or ls < -1:
                errs.append("%s: loop flag/start inconsistent (flags %d, loopstart %d, frames %d)" % (tag, e["flags"], ls, fr))
            dec = None
            if fmt == FMT_PCM8:
                if nb != fr or fr % 4 or nb < MIN_BYTES or (ls >= 0 and ls % 4):
                    errs.append("%s: pcm8 not word-aligned or under %d bytes (frames %d, bytes %d, loop %d)" % (tag, MIN_BYTES, fr, nb, ls))
                dec = np.frombuffer(data, np.int8).astype(np.int64) * 256
                play = melon_play(data, fmt, ls, fr, nticks=fr + 2)
                if play[2:2 + fr] != dec.tolist():
                    errs.append("%s: melonDS model plays something else" % tag)
            elif fmt == FMT_ADPCM:
                if nb != 4 + fr // 2 or fr % 8 or nb < MIN_BYTES or (ls >= 0 and ls % 8):
                    errs.append("%s: adpcm not word-aligned or under %d bytes (frames %d, bytes %d, loop %d)" % (tag, MIN_BYTES, fr, nb, ls))
                if data[2] > 88 or data[3] != 0:
                    errs.append("%s: adpcm header index %d / byte 3 %d" % (tag, data[2], data[3]))
                dec = adpcm_decode(data, fr)
                if opt.thorough:
                    play = melon_play(data, fmt, ls, fr, nticks=10 + fr + (fr - ls if ls >= 0 else 0))
                    if play[10:10 + fr] != dec:
                        errs.append("%s: melonDS model plays something else" % tag)
                    if ls >= 0 and play[10 + fr:10 + fr + (fr - ls)] != dec[ls:]:
                        errs.append("%s: the loop doesn't replay from its start" % tag)
            elif fmt == FMT_STREAM:
                if nb < 8:
                    errs.append("%s: stream too short" % tag)
                    continue
                block, nbl = struct.unpack_from("<II", data, 0)
                if block % 8 or nbl != (fr + block - 1) // block or nb != 8 + 4 * nbl + nbl * block // 2:
                    errs.append("%s: stream blocks inconsistent (block %d, nblocks %d, bytes %d)" % (tag, block, nbl, nb))
                    continue
                dec, bad = stream_decode(data, fr)
                if bad:
                    errs.append("%s: %d block states differ from the decoder's" % (tag, bad))
            else:
                errs.append("%s: format %d" % (tag, fmt))
                continue
            t = stats.setdefault(fmt, [0, 0, 0.0])
            t[0] += 1
            t[1] += nb
            t[2] += fr / e["rate"]
            if dec is not None and h["name"] in ref:
                r = ref[h["name"]]
                pcm = decode_any(r.reader(), r.rel)
                if pcm is not None:
                    s = convert(h["name"], pcm, opt)
                    n = min(len(s.ref), len(dec))
                    v = snr_db(s.ref[:n], np.asarray(dec[:n]))
                    snrs.append((v, h["name"]))
                    if opt.report:
                        print("  %-40s %-6s %5d Hz %7d B  SNR %s dB" % (h["name"][:40], FMT_NAMES[fmt], e["rate"], nb, fmt_db(v)))
        spans.sort()
        for (a0, a1, ta), (b0, b1, tb) in zip(spans, spans[1:]):
            if b0 < a1:
                errs.append("%s overlaps %s" % (ta, tb))
        for e in errs:
            print("%s: ERROR %s" % (path, e))
        nerr += len(errs)
        print("%s: %d sounds, %d bytes, %s" % (path, hd["count"], len(buf), "ok" if not errs else "%d errors" % len(errs)))
        for f, (c, b, sec) in sorted(stats.items()):
            print("  %-6s %4d sounds %9d bytes %7.1f s" % (FMT_NAMES.get(f, f), c, b, sec))
        if snrs:
            v = sorted(x for x, _ in snrs if x != float("inf"))
            if v:
                print("  SNR vs the sources (dB): min %.1f (%s)  median %.1f  max %.1f  n=%d" % (
                    v[0], min(snrs)[1], v[len(v) // 2], v[-1], len(snrs)))
    return 1 if nerr else 0


def melon_play(data, fmt, loopstart, frames, nticks):
    """the samples melonDS outputs for a pack sample, a value a timer period,
    from the start (sound_nds.c's programming: one-shot from 0, or a loop from
    loopstart: PNT = loopstart/4 words for PCM8, loopstart/8 + 1 for ADPCM)"""
    words = len(data) // 4
    if loopstart >= 0:
        pnt = loopstart // 4 if fmt == FMT_PCM8 else loopstart // 8 + 1
        ch = MelonChannel(data, fmt, pnt, words - pnt, 1)
    else:
        ch = MelonChannel(data, fmt, 0, words, 2)
    return [ch.tick() for _ in range(nticks)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], usage=__doc__.split("\n\n")[1])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--against", nargs="+", default=[])
    ap.add_argument("--thorough", action="store_true", help="--check: run every ADPCM sample through the melonDS model")
    ap.add_argument("--maxrate", type=int, default=22050)
    ap.add_argument("--maxrate-loop", type=int, default=11025)
    ap.add_argument("--pcm8", action="append")
    ap.add_argument("--adpcm-min-snr", type=float, default=18.0)
    ap.add_argument("--stream-kb", type=float, default=96)
    ap.add_argument("--only-used", action="store_true")
    ap.add_argument("--used-list", nargs="+")
    ap.add_argument("--decls", nargs="+")
    ap.add_argument("--keep", action="append")
    ap.add_argument("--music")
    ap.add_argument("--music-stereo", action="store_true")
    ap.add_argument("--music-rate", type=int, default=22050)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--allow-repo", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    opt = ap.parse_args(argv)
    if opt.check:
        if not opt.paths:
            ap.error("--check needs pack files")
        return check(opt.paths, opt.against, opt)
    if len(opt.paths) < 2:
        ap.error("OUT.dsp and at least one SRC")
    opt.out, opt.src = opt.paths[0], opt.paths[1:]
    return build(opt)


if __name__ == "__main__":
    sys.exit(main())
