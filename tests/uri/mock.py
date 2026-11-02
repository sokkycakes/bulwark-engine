# Mock web API for run.sh: answers with the method, path, Authorization,
# Content-Type and body it got as JSON; /err answers 400, /redir 302.
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def handle_one(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode()
        out = json.dumps({"method": self.command, "path": self.path, "auth": self.headers.get('Authorization'),
                          "ctype": self.headers.get('Content-Type'), "body": body}).encode()
        code = 400 if self.path.startswith('/err') else (302 if self.path.startswith('/redir') else 200)
        self.send_response(code)
        if code == 302: self.send_header('Location', 'http://127.0.0.1:%d/other' % self.server.server_port)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(out)))
        self.end_headers(); self.wfile.write(out)
    do_GET = do_POST = do_PUT = handle_one
    def log_message(self, *a): sys.stderr.write("mock: %s\n" % (a[0] % a[1:]))
HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
