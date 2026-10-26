# Local test server with the headers SharedArrayBuffer needs (COOP/COEP).
import http.server, sys, os
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()
    def log_message(self, *a): pass
H.extensions_map['.wasm'] = 'application/wasm'
H.extensions_map['.js'] = 'text/javascript'
os.chdir(sys.argv[1])
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), H).serve_forever()
