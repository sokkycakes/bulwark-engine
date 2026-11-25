#!/usr/bin/env python3
"""Serve an exported web game on this machine, as a web host would.

    python3 tools/web/serve.py out/web-base            # http://localhost:8080/
    python3 tools/web/serve.py out/web-base --port 9000 --lan

Browsers keep the pointer lock, fullscreen, controllers and saved files to
secure pages: http://localhost counts as one, an address on the LAN doesn't
(use https there, or Chrome's "Insecure origins treated as secure" flag).
Only the standard library is used.
"""
import argparse
import functools
import http.server
import os
import socketserver
import sys

TYPES = {
    ".wasm": "application/wasm",
    ".js": "text/javascript",
    ".json": "application/json",
    ".bwpack": "application/octet-stream",
    ".webmanifest": "application/manifest+json",
    ".html": "text/html; charset=utf-8",
    ".css": "text/css",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    isolate = False

    def guess_type(self, path):
        ext = os.path.splitext(path)[1].lower()
        return TYPES.get(ext) or super().guess_type(path)

    def end_headers(self):
        path = self.path.split("?", 1)[0]
        # hashed names never change; the page and its list always may
        if "/engine/" in path or "/data/" in path:
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        else:
            self.send_header("Cache-Control", "no-cache")
        if self.isolate:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            super().log_message(fmt, *args)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    quiet = False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="an export (tools/web/export.py --out)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--lan", action="store_true", help="listen on every address, not only this machine")
    ap.add_argument("--isolate", action="store_true", help="send COOP/COEP headers (cross-origin isolation)")
    ap.add_argument("--quiet", action="store_true", help="don't log each request")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.folder, "index.html")):
        sys.exit("serve: no index.html in %s" % a.folder)
    Handler.isolate = a.isolate
    handler = functools.partial(Handler, directory=os.path.abspath(a.folder))
    srv = Server(("0.0.0.0" if a.lan else "127.0.0.1", a.port), handler)
    srv.quiet = a.quiet
    print("serving %s at http://localhost:%d/  (Ctrl+C stops)" % (a.folder, a.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
