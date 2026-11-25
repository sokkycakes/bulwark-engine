#!/usr/bin/env python3
"""Export a Bulwark game for the web: one folder any static web server can
host (itch.io, GitHub Pages, nginx, `python3 tools/web/serve.py`).

    tools/web/export.py --basedir ~/nuclide --game base --out out/web-base \\
        --title "Nuclide" -- +map ai

What goes in the folder:
  index.html, bulwark-shell.js/.css  the page (tools/web/shell)
  engine/bulwark-<hash>.js/.wasm     the engine (a web build, tools/web/build.sh)
  data/<name>-<hash>.bwpack          the game's files and Bulwark's data/,
                                     gzip'd, in packs of up to --pack-mb
  bulwark-web.json                   what the page loads, and the arguments

Names carry a content hash, so a host can cache them for good and a new
export never mixes with an old one in a player's browser.

Only the standard library is used. See docs/platforms/web.md.
"""
import argparse
import fnmatch
import gzip
import hashlib
import json
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# never worth shipping: sources, editor files, version control
DEFAULT_EXCLUDES = [
    ".git", ".git/*", "*/.git/*", ".svn/*", "*/.DS_Store", ".DS_Store", "Thumbs.db",
    "mapsrc/*", "modelsrc/*", "*.blend", "*.blend1", "*.psd", "*.xcf", "*.kra",
    "*.qc", "*.qh", "*.src", "*.pyc", "__pycache__/*", "*/__pycache__/*",
    "*.autosave.*", "*.bak", "*~", "*.py", "*.prt", "*.srf", "*.lno", "*/autosave/*", "autosave/*",
    # what a desktop run leaves behind
    "*.log", "screenshots/*", "condump*.txt", "qconsole.log",
]


def die(msg):
    print("export: " + msg, file=sys.stderr)
    sys.exit(1)


def sha(data):
    return hashlib.sha256(data).hexdigest()[:12]


def excluded(rel, patterns):
    rel_l = rel.lower()
    for p in patterns:
        if fnmatch.fnmatch(rel, p) or fnmatch.fnmatch(rel_l, p.lower()):
            return True
        # a bare directory name or pattern matches anything under it
        if p.endswith("/*") and (rel + "/").startswith(p[:-1]):
            return True
    return False


def collect(root, mount, patterns):
    """(path in the browser, path on disk, size) for every file under root"""
    out = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        rel_dir = os.path.relpath(dirpath, root)
        rel_dir = "" if rel_dir == "." else rel_dir.replace(os.sep, "/")
        dirnames[:] = sorted(d for d in dirnames
                             if not excluded((rel_dir + "/" if rel_dir else "") + d + "/x", patterns)
                             and not d.startswith("."))
        for f in sorted(filenames):
            rel = (rel_dir + "/" if rel_dir else "") + f
            if excluded(rel, patterns):
                continue
            disk = os.path.join(dirpath, f)
            if not os.path.isfile(disk):
                continue
            out.append((mount + "/" + rel, disk, os.path.getsize(disk)))
    return out


def make_packs(label, files, pack_bytes, datadir, level):
    """files into packs of about pack_bytes each; a file bigger than that
    (a .pk3) gets a pack of its own"""
    packs, cur, cur_size = [], [], 0

    def flush():
        nonlocal cur, cur_size
        if cur:
            packs.append(cur)
        cur, cur_size = [], 0

    for f in files:
        if f[2] >= pack_bytes:
            flush()
            packs.append([f])
            continue
        if cur_size + f[2] > pack_bytes:
            flush()
        cur.append(f)
        cur_size += f[2]
    flush()

    out = []
    for n, pack in enumerate(packs):
        blob = bytearray()
        index = []
        for path, disk, size in pack:
            with open(disk, "rb") as fh:
                data = fh.read()
            index.append([path, len(blob), len(data)])
            blob += data
        raw = bytes(blob)
        packed = gzip.compress(raw, compresslevel=level, mtime=0)
        # already compressed (pk3, ogg): store as is
        use_gzip = len(packed) < len(raw) * 0.97
        body = packed if use_gzip else raw
        name = "%s%s-%s.bwpack" % (label, "" if len(packs) == 1 else "-%d" % (n + 1), sha(body))
        with open(os.path.join(datadir, name), "wb") as fh:
            fh.write(body)
        out.append({"url": "data/" + name, "size": len(body), "rawSize": len(raw), "gzip": use_gzip,
                    "files": index})
        print("  data/%s: %d files, %.1f MB%s" % (name, len(index), len(body) / 1048576.0,
                                                  " (gzip from %.1f MB)" % (len(raw) / 1048576.0) if use_gzip else ""))
    return out


def find_engine(path):
    for d in [path, os.path.join(path, "bin")]:
        js, wasm = os.path.join(d, "bulwark.js"), os.path.join(d, "bulwark.wasm")
        if os.path.isfile(js) and os.path.isfile(wasm):
            return js, wasm
    die("no bulwark.js and bulwark.wasm in %s: build the web engine first (tools/web/build.sh)" % path)


def safe_label(s):
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in s) or "data"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--basedir", required=True, help="the folder holding the game folders (and any .fmf)")
    ap.add_argument("--game", action="append", default=[],
                    help="a game folder under basedir, or a manifest name; repeat for mods on top (base first)")
    ap.add_argument("--engine", default=os.path.join(REPO, "build-web"),
                    help="the web build's folder, holding bulwark.js and bulwark.wasm (default: build-web)")
    ap.add_argument("--enginedata", default=os.path.join(REPO, "data"), help="Bulwark's own data/ (default: the repo's)")
    ap.add_argument("--out", required=True, help="the folder to write (replaced)")
    ap.add_argument("--title", default=None, help="the page's title (default: the game's name)")
    ap.add_argument("--exclude", action="append", default=[], help="a glob of files to leave out, relative to a game folder")
    ap.add_argument("--include-sources", action="store_true", help="keep the default exclusions (sources, editor files) in")
    ap.add_argument("--pack-mb", type=float, default=32, help="pack size in MB (default 32)")
    ap.add_argument("--fullscreen", choices=["auto", "always", "never"], default="auto",
                    help="go fullscreen on Play: auto is on phones and handhelds only (default)")
    ap.add_argument("--max-pixel-ratio", type=float, default=0,
                    help="cap on device pixels per CSS pixel (0: about 1440p worth)")
    ap.add_argument("--no-confirm-leave", action="store_true", help="don't ask before closing the tab in a game")
    ap.add_argument("--zip", metavar="FILE", help="also write the folder as a .zip (for itch.io)")
    ap.add_argument("--level", type=int, default=6, help="gzip level (default 6)")
    ap.add_argument("args", nargs="*", help="engine arguments after --, e.g. -- +map ai")
    a = ap.parse_args()

    basedir = os.path.abspath(os.path.expanduser(a.basedir))
    if not os.path.isdir(basedir):
        die("no folder " + basedir)
    games = a.game or ["base"]
    patterns = list(a.exclude) + ([] if a.include_sources else DEFAULT_EXCLUDES)

    # a game named by its manifest (base.fmf, tftemplate.fmf): its gamedirs
    gamedirs, manifests = [], []
    for g in games:
        fmf = os.path.join(basedir, g + ".fmf")
        if os.path.isfile(fmf):
            manifests.append(fmf)
            dirs = []
            with open(fmf, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    t = line.strip().split()
                    if len(t) >= 2 and t[0].lower() in ("gamedir", "basegame"):
                        dirs.append(t[1].strip('"').lstrip("*"))
            for d in dirs or [g]:
                if os.path.isdir(os.path.join(basedir, d)) and d not in gamedirs:
                    gamedirs.append(d)
        elif os.path.isdir(os.path.join(basedir, g)):
            if g not in gamedirs:
                gamedirs.append(g)
        else:
            die("no game folder or manifest named %s in %s" % (g, basedir))

    js, wasm = find_engine(os.path.abspath(a.engine))
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        if not os.path.isfile(os.path.join(out, "bulwark-web.json")) and os.listdir(out):
            die("%s exists and isn't an earlier export: pick an empty or new folder" % out)
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "engine"))
    os.makedirs(os.path.join(out, "data"))

    print("export: %s -> %s" % (", ".join(gamedirs), out))
    pack_bytes = int(a.pack_mb * 1048576)
    packs = []
    # Bulwark's own data (UI layouts, fonts) at /bulwark/data
    if os.path.isdir(a.enginedata):
        packs += make_packs("bulwark-data", collect(a.enginedata, "/bulwark/data", DEFAULT_EXCLUDES),
                            pack_bytes, os.path.join(out, "data"), a.level)
    # manifests and loose files at the base directory's top
    top = []
    for f in sorted(os.listdir(basedir)):
        p = os.path.join(basedir, f)
        if os.path.isfile(p) and (f.endswith(".fmf") or f.lower() in ("default.fmf",)):
            top.append(("/game/" + f, p, os.path.getsize(p)))
    if top:
        packs += make_packs("manifests", top, pack_bytes, os.path.join(out, "data"), a.level)
    for d in gamedirs:
        files = collect(os.path.join(basedir, d), "/game/" + d, patterns)
        if not files:
            die("nothing to export in %s" % os.path.join(basedir, d))
        packs += make_packs(safe_label(d), files, pack_bytes, os.path.join(out, "data"), a.level)

    with open(js, "rb") as fh:
        js_data = fh.read()
    with open(wasm, "rb") as fh:
        wasm_data = fh.read()
    js_name = "engine/bulwark-%s.js" % sha(js_data + wasm_data)
    wasm_name = "engine/bulwark-%s.wasm" % sha(wasm_data)
    with open(os.path.join(out, js_name), "wb") as fh:
        fh.write(js_data)
    with open(os.path.join(out, wasm_name), "wb") as fh:
        fh.write(wasm_data)

    # -game for each folder, or the manifest's name when one was asked for
    args = ["-basedir", "/game", "-enginedata", "/bulwark/data", "-userdir", "/user"]
    for g in games:
        args += ["-game", g]
    args += a.args
    title = a.title or (games[-1] if games[-1] != "base" else "Bulwark")
    config = {
        "title": title,
        "engine": {"js": js_name, "wasm": wasm_name, "wasmSize": len(wasm_data)},
        "args": args,
        "packs": packs,
        "options": {"fullscreenOnPlay": a.fullscreen, "maxPixelRatio": a.max_pixel_ratio or None,
                    "confirmLeave": not a.no_confirm_leave},
    }
    with open(os.path.join(out, "bulwark-web.json"), "w") as fh:
        json.dump(config, fh, separators=(",", ":"))

    shell = os.path.join(HERE, "shell")
    esc = title.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    with open(os.path.join(shell, "index.html"), encoding="utf-8") as fh:
        html = fh.read().replace("{{TITLE}}", esc)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(html)
    for f in ("bulwark-shell.js", "bulwark-shell.css"):
        shutil.copy(os.path.join(shell, f), os.path.join(out, f))
    with open(os.path.join(out, "manifest.webmanifest"), "w") as fh:
        json.dump({"name": title, "short_name": title[:12], "display": "fullscreen", "orientation": "landscape",
                   "background_color": "#000000", "theme_color": "#000000", "start_url": "./"}, fh)

    total = sum(p["size"] for p in packs) + len(wasm_data) + len(js_data)
    print("export: %d packs, %.1f MB to download; serve it with: python3 %s %s"
          % (len(packs), total / 1048576.0, os.path.relpath(os.path.join(HERE, "serve.py")), os.path.relpath(out)))

    if a.zip:
        zpath = os.path.abspath(a.zip)
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_STORED) as z:
            for dirpath, _, filenames in os.walk(out):
                for f in sorted(filenames):
                    p = os.path.join(dirpath, f)
                    z.write(p, os.path.relpath(p, out))
        print("export: wrote %s" % zpath)


if __name__ == "__main__":
    main()
