#!/usr/bin/env python3
"""Bench sink for TallyPad test builds.

Logs every POST /press with a timestamp and forwards nothing, so a test build
can be pressed freely without touching your real listener or its data.
Usage: python3 tools/bench-sink.py   (listens on :4189)
"""
import http.server, time
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0)).decode(errors='replace')
        print(f"{time.time():.3f} {self.client_address[0]} {self.path} {body}", flush=True)
        self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('0.0.0.0', 4189), H).serve_forever()
