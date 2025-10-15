#!/usr/bin/env python3
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

class BackendHandler(BaseHTTPRequestHandler):
    backend_id = "?"
    def do_GET(self):
        body = f"Hello from BACKEND {self.backend_id}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Backend", self.backend_id)
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, fmt, *args):
        # suppress normal HTTP logs for clarity
        print(f"[{self.backend_id}] {self.address_string()} {self.command} {self.path}")

def run_backend(port, backend_id):
    handler = type(f"Handler_{backend_id}", (BackendHandler,), {"backend_id": backend_id})
    server = HTTPServer(("127.0.0.1", port), handler)
    print(f"✅ Backend {backend_id} listening on http://127.0.0.1:{port}")
    server.serve_forever()

if __name__ == "__main__":
    try:
        # Launch two backends on different threads
        t1 = threading.Thread(target=run_backend, args=(9000, "A"), daemon=True)
        t2 = threading.Thread(target=run_backend, args=(9001, "B"), daemon=True)
        t1.start()
        t2.start()
        print("\nBackends running! Try this in another terminal:")
        print("  curl -s http://127.0.0.1:8080/api\n")
        print("Expected alternating X-Backend headers: A, B, A, B ...")
        print("\nPress Ctrl+C to stop servers.\n")
        t1.join()  # keep running until interrupted
        t2.join()
    except KeyboardInterrupt:
        print("\n🛑 Stopped all backends.")
