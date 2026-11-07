#!/usr/bin/env python3
"""Bulwark engine - tests for tools/nds/sndpack.py. ISC licence, see LICENSE.

    python3 tools/nds/test_sndpack.py [-v]

Everything is made here from generated tones: no game data is read or written
outside a temporary directory.
"""
import io
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sndpack as sp  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sndpack.py")


def wav_bytes(samples, rate, bits=16, channels=1, cue=None, cue_after_data=True, extra=b""):
    """a RIFF/WAVE file; samples: ints in the format's range (interleaved)"""
    if bits == 8:
        data = bytes((int(v) + 128) & 255 for v in samples)
    else:
        data = struct.pack("<%dh" % len(samples), *[int(v) for v in samples])
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    chunks = [b"fmt " + struct.pack("<I", len(fmt)) + fmt]
    datachunk = b"data" + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")
    cuechunk = b""
    if cue is not None:
        body = struct.pack("<I", 1) + struct.pack("<IIIIII", 1, cue, 0x61746164, 0, 0, cue)
        cuechunk = b"cue " + struct.pack("<I", len(body)) + body
    if cue_after_data:
        chunks += [datachunk, extra, cuechunk]
    else:
        chunks += [cuechunk, extra, datachunk]
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def readf(path):
    with open(path, "rb") as f:
        return f.read()


def tone(freq, rate, n, amp=12000.0):
    t = np.arange(n) / rate
    return np.rint(amp * np.sin(2 * math.pi * freq * t)).astype(np.int64)


def dominant_freq(x, rate):
    w = np.hanning(len(x))
    f = np.abs(np.fft.rfft(x * w))
    k = int(np.argmax(f))
    # parabolic interpolation on the log magnitude
    a, b, c = np.log(f[k - 1:k + 2] + 1e-12)
    p = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + p) * rate / len(x)


class Opt:
    maxrate = 22050
    maxrate_loop = 11025
    pcm8 = None
    adpcm_min_snr = 18.0
    stream_kb = 96
    report = False
    thorough = True


def literal_adpcm(val, idx, code):
    """SPU.cpp NextSample_ADPCM's arithmetic, line by line (melonDS 1.1)"""
    v = sp.ADPCM_TABLE[idx]
    diff = v >> 3
    if code & 1:
        diff += v >> 2
    if code & 2:
        diff += v >> 1
    if code & 4:
        diff += v
    diff &= 0xFFFF  # u16 in SPU.cpp
    if code & 8:
        val -= diff
        if val < -0x7FFF:
            val = -0x7FFF
    else:
        val += diff
        if val > 0x7FFF:
            val = 0x7FFF
    idx += sp.ADPCM_INDEX[code & 7]
    idx = 0 if idx < 0 else 88 if idx > 88 else idx
    return val, idx


class Names(unittest.TestCase):
    def test_vectors(self):
        # docs/spec/link.md / SPEC §2.1: T, A and S share these
        for name, norm, key in [("misc/talk.wav", "misc/talk", 0x7c72dd3c),
                                ("common/null.wav", "common/null", 0x7e3eafd0),
                                ("weapons/glock1.wav", "weapons/glock1", 0xf61c39de),
                                ("Sound\\Weapons\\Glock1.WAV", "weapons/glock1", 0xf61c39de),
                                ("vox/hello", "vox/hello", 0x98432201),
                                ("music/track02", "music/track02", 0xc3df92fe)]:
            self.assertEqual(sp.norm_name(name), norm)
            self.assertEqual(sp.snd_key(name), key, name)

    def test_norm_rules(self):
        self.assertEqual(sp.norm_name("./sound/a/b.ogg"), "a/b")
        self.assertEqual(sp.norm_name("sound/sound/x.wav"), "sound/x")  # one "sound/" only
        self.assertEqual(sp.norm_name(".wav"), ".wav")                 # len > 4 rule (sv_link.c snd_norm)
        self.assertEqual(sp.norm_name("x.flac"), "x.flac")

    def test_music_names(self):
        self.assertEqual(sp.music_name("music/track02.ogg"), "music_track02.dsm")
        self.assertEqual(sp.music_name("sound/bgm/X.mp3"), "sound_bgm_x.dsm")


class Riff(unittest.TestCase):
    def test_cue_after_data(self):
        x = tone(440, 11025, 5000)
        p = sp.decode_wav(wav_bytes(x, 11025, cue=1234, cue_after_data=True, extra=b"LIST\x04\0\0\0abcd"))
        self.assertEqual((p.rate, p.bits, p.channels, len(p.data), p.loopstart), (11025, 16, 1, 5000, 1234))
        self.assertTrue(np.array_equal(p.data, x))

    def test_cue_before_data_and_bad_cue(self):
        x = tone(440, 11025, 100)
        self.assertEqual(sp.decode_wav(wav_bytes(x, 11025, cue=50, cue_after_data=False)).loopstart, 50)
        self.assertEqual(sp.decode_wav(wav_bytes(x, 11025, cue=100)).loopstart, -1)  # past the end: no loop

    def test_8bit_stereo_and_odd_chunk(self):
        raw = [10, -10, 20, -20, 30, -30, 40]  # 7 bytes: odd, padded; 3 stereo frames
        p = sp.decode_wav(wav_bytes(raw, 8000, bits=8, channels=2))
        self.assertEqual((p.channels, len(p.data)), (2, 3))
        self.assertEqual(p.data[1].tolist(), [20 * 256, -20 * 256])
        self.assertEqual(sp.to_mono(p).tolist(), [0.0, 0.0, 0.0])

    def test_truncated_chunk_clamped(self):
        # a data chunk claiming more than the file has (ffmpeg's piped WAVs do)
        w = bytearray(wav_bytes(tone(100, 8000, 64), 8000))
        i = w.index(b"data")
        w[i + 4:i + 8] = struct.pack("<I", 0xFFFFFFFF)
        self.assertEqual(len(sp.decode_wav(bytes(w)).data), 64)

    def test_not_pcm(self):
        w = bytearray(wav_bytes(tone(100, 8000, 64), 8000))
        w[20:22] = struct.pack("<H", 3)  # float
        self.assertIsNone(sp.decode_wav(bytes(w)))


class Resampler(unittest.TestCase):
    def test_tone_44k_to_22k(self):
        x = tone(1000, 44100, 44100).astype(np.float64)
        y = sp.resample(x, 44100, 22050)
        self.assertEqual(len(y), 22050)
        mid = y[2000:-2000]
        f = dominant_freq(mid, 22050)
        self.assertLess(abs(f - 1000) / 1000, 0.005, f)
        rms_in = np.sqrt(np.mean(x[4000:-4000] ** 2))
        rms_out = np.sqrt(np.mean(mid ** 2))
        self.assertLess(abs(rms_out / rms_in - 1), 0.005, rms_out / rms_in)

    def test_alias_rejected(self):
        # 15 kHz at 44.1 kHz is above 22.05 kHz's Nyquist: it must go
        x = tone(15000, 44100, 44100).astype(np.float64)
        y = sp.resample(x, 44100, 22050)[2000:-2000]
        self.assertLess(np.sqrt(np.mean(y ** 2)) / np.sqrt(np.mean(x ** 2)), 10 ** (-50 / 20))

    def test_odd_ratio(self):
        x = tone(1000, 48000, 48000).astype(np.float64)
        y = sp.resample(x, 48000, 22050)
        self.assertLess(abs(dominant_freq(y[2000:-2000], 22050) - 1000), 5)


class Adpcm(unittest.TestCase):
    def test_step_matches_spu_cpp(self):
        rnd = random.Random(1)
        for idx in range(89):
            for code in range(16):
                for val in (-0x7FFF, -30000, -1, 0, 1, 12345, 0x7FFF, rnd.randint(-0x7FFF, 0x7FFF)):
                    self.assertEqual(sp.adpcm_step(val, idx, code), literal_adpcm(val, idx, code))

    def test_tone_round_trip_through_melon(self):
        x = tone(1000, 22050, 4000)
        data, dec = sp.enc_adpcm(x)
        self.assertEqual(len(data), 4 + 2000)
        snr = sp.snr_db(x, dec)
        print("\n  ADPCM 1 kHz tone at 22050: SNR %.1f dB" % snr)
        self.assertGreater(snr, 20.0)
        play = sp.melon_play(data, sp.FMT_ADPCM, -1, 4000, 4000 + 12)
        # melonDS: Start() sets Pos -3; Pos -2..7 (the header) keep CurSample 0
        self.assertEqual(play[:10], [0] * 10)
        self.assertEqual(play[10:4010], dec.tolist())
        self.assertEqual(play[4010:], [0, 0])  # a one-shot ends, CurSample 0

    def test_loop_replays_exactly(self):
        x = tone(300, 11025, 4096)
        data, dec = sp.enc_adpcm(x)
        ls = 1000 // 8 * 8 + 8
        play = sp.melon_play(data, sp.FMT_ADPCM, ls, 4096, 10 + 4096 + 3 * (4096 - ls))
        d = dec.tolist()
        self.assertEqual(play[10:10 + 4096], d)
        for k in range(3):
            o = 10 + 4096 + k * (4096 - ls)
            self.assertEqual(play[o:o + 4096 - ls], d[ls:], "pass %d" % (k + 2))

    def test_pcm8_through_melon(self):
        x = tone(300, 11025, 64, amp=20000)
        data, dec = sp.enc_pcm8(x)
        play = sp.melon_play(data, sp.FMT_PCM8, -1, 64, 70)
        self.assertEqual(play[:2], [0, 0])  # Pos -2, -1
        self.assertEqual(play[2:66], dec.tolist())
        loop = sp.melon_play(data, sp.FMT_PCM8, 16, 64, 2 + 64 + 48)
        self.assertEqual(loop[66:114], dec.tolist()[16:])

    def test_short_never_plays(self):
        # SPU.cpp Run<>: LoopPos+Length under 16 bytes keeps the channel silent
        ch = sp.MelonChannel(b"\x40" * 12, sp.FMT_PCM8, 0, 3, 2)
        self.assertEqual([ch.tick() for _ in range(10)], [0] * 10)

    def test_clamp_is_7fff(self):
        v, i = sp.adpcm_step(-0x7FF0, 88, 15)
        self.assertEqual(v, -0x7FFF)

    def test_stream_states(self):
        x = tone(440, 11025, 5000)
        data, dec = sp.enc_stream(x, block=2048)
        block, nb = struct.unpack_from("<II", data, 0)
        self.assertEqual((block, nb, len(data)), (2048, 3, 8 + 12 + 3 * 1024))
        out, bad = sp.stream_decode(data, 5000)
        self.assertEqual(bad, 0)
        self.assertEqual(out, dec.tolist())
        # each block decodes alone from its state (what lets the DS seek)
        st = struct.unpack_from("<hBB", data, 8 + 4)
        codes = sp.unpack_nibbles(data[8 + 12 + 1024:], 2048).tolist()
        v, i = st[0], st[1]
        for k in range(2048):
            v, i = sp.adpcm_step(v, i, codes[k])
            self.assertEqual(v, dec[2048 + k])


class Convert(unittest.TestCase):
    def pcm(self, x, rate, loop=-1, bits=16):
        return sp.Pcm(np.asarray(x, np.float64), rate, loop, bits, 1)

    def test_word_aligned_end_and_loop(self):
        x = tone(200, 11025, 3003)
        s = sp.convert("a/b", self.pcm(x, 11025, loop=1001), Opt())
        self.assertEqual(s.fmt, sp.FMT_ADPCM)
        self.assertEqual((s.frames, s.loopstart, s.loopshift, s.endtrim), (3000, 1008, 7, 3))
        self.assertEqual(len(s.data), 4 + 1500)
        self.assertEqual(s.flags, 1)

    def test_loop_rate_cap(self):
        x = tone(200, 44100, 44100)
        s = sp.convert("l", self.pcm(x, 44100, loop=4410), Opt())
        self.assertEqual((s.rate, s.loopstart % 8), (11025, 0))
        self.assertTrue(1102 <= s.loopstart <= 1110)
        s1 = sp.convert("o", self.pcm(x, 44100), Opt())
        self.assertEqual(s1.rate, 22050)

    def test_choice(self):
        o = Opt()
        # a clean tone: ADPCM
        self.assertEqual(sp.convert("t", self.pcm(tone(300, 11025, 4000), 11025), o).fmt, sp.FMT_ADPCM)
        # small: PCM8
        self.assertEqual(sp.convert("s", self.pcm(tone(300, 11025, 900), 11025), o).fmt, sp.FMT_PCM8)
        # white noise at full scale defeats IMA: PCM8 (better and above the floor)
        rnd = np.random.default_rng(3)
        n = np.clip(rnd.normal(0, 12000, 8000), -32767, 32767)
        s = sp.convert("n", self.pcm(n, 11025), o)
        self.assertLess(s.snr_adpcm, 18)
        self.assertEqual((s.fmt, s.why), (sp.FMT_PCM8, "snr"))
        # near silence (below 8 bits): ADPCM even under the floor
        q = rnd.integers(-7, 8, 8000)
        self.assertEqual(sp.convert("q", self.pcm(q, 11025), o).fmt, sp.FMT_ADPCM)
        # named by --pcm8
        o2 = Opt()
        o2.pcm8 = ["t*"]
        self.assertEqual(sp.convert("t", self.pcm(tone(300, 11025, 4000), 11025), o2).fmt, sp.FMT_PCM8)

    def test_pcm8_from_8bit_exact(self):
        x = np.arange(-128, 128).repeat(8) * 256
        s = sp.convert("e", self.pcm(x, 11025, bits=8), Opt())
        self.assertEqual(s.snr_pcm8, float("inf"))

    def test_stream_threshold(self):
        x = tone(200, 11025, 2 * 96 * 1024 + 100)
        s = sp.convert("long", self.pcm(x, 11025), Opt())
        self.assertEqual((s.fmt, s.flags), (sp.FMT_STREAM, 2))

    def test_min_bytes(self):
        s = sp.convert("tiny", self.pcm(tone(300, 11025, 5), 11025), Opt())
        self.assertGreaterEqual(len(s.data), sp.MIN_BYTES)


class PackFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sndpack-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_tool(self, *args, ok=True):
        r = subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True, cwd=self.tmp)
        if ok and r.returncode:
            self.fail("sndpack %s: %s%s" % (args, r.stdout, r.stderr))
        return r

    def make_game(self):
        g = os.path.join(self.tmp, "game")
        os.makedirs(os.path.join(g, "sound", "weapons"))
        os.makedirs(os.path.join(g, "sound", "ambience"))
        os.makedirs(os.path.join(g, "decls", "sound"))
        with open(os.path.join(g, "sound", "weapons", "Fire.WAV"), "wb") as f:
            f.write(wav_bytes(tone(800, 44100, 20000), 44100))
        with open(os.path.join(g, "sound", "ambience", "hum.wav"), "wb") as f:
            f.write(wav_bytes(tone(120, 11025, 9000), 11025, cue=301))
        with open(os.path.join(g, "sound", "weapons", "both.wav"), "wb") as f:
            f.write(wav_bytes(tone(500, 11025, 3000), 11025))
        with open(os.path.join(g, "decls", "sound", "w.sndshd"), "w") as f:
            f.write("weapon.fire\n{\n\tsample weapons/fire.wav\n\tsample weapons/gone.wav\n}\n")
        # a pk3 in the game dir: loose files beat it; it adds what is only in it
        with zipfile.ZipFile(os.path.join(g, "pak0.pk3"), "w") as z:
            z.writestr("sound/weapons/fire.wav", wav_bytes(tone(100, 11025, 3000), 11025))
            z.writestr("sound/misc/talk.wav", wav_bytes(tone(1500, 22050, 2000, amp=8000), 22050, bits=16))
            z.writestr("sound/weapons/both.ogg", b"OggS-not-really")
        # a later SRC: a pk3 that replaces hum
        p = os.path.join(self.tmp, "zz_test.pk3")
        with zipfile.ZipFile(p, "w") as z:
            z.writestr("sound/ambience/hum.wav", wav_bytes(tone(240, 11025, 6000), 11025, cue=16))
        return g, p

    def test_pack_layers_check(self):
        g, p = self.make_game()
        out = os.path.join(self.tmp, "t.dsp")
        r = self.run_tool(out, g, p, "--report", "--decls", g, "--jobs", "1")
        self.assertIn("PRIVATE", r.stdout)  # made from a zz_*.pk3
        self.assertIn("decls name 1 samples no SRC has", r.stdout)
        buf = readf(out)
        hd, ents = sp.read_pack(buf)
        names = {}
        for e in ents:
            h, data = sp.sample_at(buf, e)
            names[h["name"]] = (h, data)
        self.assertEqual(sorted(names), ["ambience/hum", "misc/talk", "weapons/both", "weapons/fire"])
        self.assertEqual(names["weapons/fire"][0]["rate"], 22050)            # the loose 44.1 kHz file, not the pk3's
        self.assertEqual(names["ambience/hum"][0]["frames"], 6000)          # the later SRC's
        self.assertEqual(names["ambience/hum"][0]["loopstart"], 16)
        self.assertEqual(names["weapons/both"][0]["frames"], 3000)          # .wav beats .ogg
        r = self.run_tool("--check", "--thorough", out, "--against", g, p)
        self.assertIn("ok", r.stdout)
        self.assertNotIn("ERROR", r.stdout)
        self.assertIn("SNR vs the sources", r.stdout)

    def test_many_pages(self):
        g = os.path.join(self.tmp, "many")
        os.makedirs(os.path.join(g, "sound", "x"))
        for i in range(50):
            with open(os.path.join(g, "sound", "x", "s%d.wav" % i), "wb") as f:
                f.write(wav_bytes(tone(200 + i * 10, 11025, 1500 + i), 11025))
        out = os.path.join(self.tmp, "m.dsp")
        self.run_tool(out, g)
        hd, ents = sp.read_pack(readf(out))
        self.assertEqual((hd["count"], hd["index_count"]), (50, 3))
        self.assertEqual(self.run_tool("--check", out).returncode, 0)

    def test_check_catches_damage(self):
        g, p = self.make_game()
        out = os.path.join(self.tmp, "d.dsp")
        self.run_tool(out, g, "--jobs", "1")
        buf = bytearray(readf(out))
        hd, ents = sp.read_pack(bytes(buf))
        o = hd["dir_off"] + 12  # the first entry's frames
        buf[o:o + 4] = struct.pack("<I", ents[0]["frames"] + 8)
        with open(out, "wb") as fh:
            fh.write(buf)
        r = self.run_tool("--check", out, ok=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("ERROR", r.stdout)

    def test_collision_refused(self):
        seen = {}
        i = 0
        while True:
            n = "c/%d" % i
            k = sp.fnv1a(n)
            if k in seen:
                a, b = seen[k], n
                break
            seen[k] = n
            i += 1
        g = os.path.join(self.tmp, "coll")
        os.makedirs(os.path.join(g, "sound", "c"))
        for n in (a, b):
            with open(os.path.join(g, "sound", n + ".wav"), "wb") as f:
                f.write(wav_bytes(tone(300, 11025, 2000), 11025))
        r = self.run_tool(os.path.join(self.tmp, "c.dsp"), g, ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("collision", r.stderr)

    def test_refuses_git_tree(self):
        g, p = self.make_game()
        subprocess.run(["git", "init", "-q", self.tmp], check=True)
        r = self.run_tool(os.path.join(self.tmp, "x.dsp"), g, ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("git work tree", r.stderr)

    def test_only_used(self):
        g, p = self.make_game()
        lst = os.path.join(self.tmp, "used.txt")
        with open(lst, "w") as f:
            f.write("LINKSNDUSED c1a0 ambience/hum.wav\n")
        out = os.path.join(self.tmp, "u.dsp")
        r = self.run_tool(out, g, "--only-used", "--used-list", lst, "--jobs", "1")
        hd, ents = sp.read_pack(readf(out))
        got = sorted(sp.sample_at(readf(out), e)[0]["name"] for e in ents)
        # fire: the decl; hum: the used list; talk: always kept; both: nothing names it
        self.assertEqual(got, ["ambience/hum", "misc/talk", "weapons/fire"])

    @unittest.skipUnless(shutil.which("ffmpeg"), "no ffmpeg")
    def test_music(self):
        g = os.path.join(self.tmp, "mg")
        os.makedirs(os.path.join(g, "music"))
        st = np.stack([tone(440, 44100, 30000), tone(660, 44100, 30000)], 1).ravel()
        with open(os.path.join(g, "music", "Track02.wav"), "wb") as f:
            f.write(wav_bytes(st, 44100, channels=2))
        md = os.path.join(self.tmp, "music")
        self.run_tool(os.path.join(self.tmp, "x.dsp"), g, "--music", md, "--music-stereo")
        f = os.path.join(md, "music_track02.dsm")
        buf = readf(f)
        magic, ver, ch, _, rate, frames, loop, block, nb = struct.unpack_from("<4sHBBIIiII", buf, 0)
        self.assertEqual((magic, ch, rate, frames, loop, block, nb), (b"BWMS", 2, 22050, 15000, -1, 2048, 8))
        r = self.run_tool("--check", f)
        self.assertIn("ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
