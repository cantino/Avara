#!/usr/bin/env python3
# Static server with the cross-origin isolation headers the transport needs.
import sys, http.server, functools

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()
    def log_message(self, *a): pass

port = int(sys.argv[1]); root = sys.argv[2]
http.server.ThreadingHTTPServer(("127.0.0.1", port),
    functools.partial(H, directory=root)).serve_forever()
