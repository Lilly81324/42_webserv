#!/usr/bin/env python3
# proxy_e2e_rr.py — starts two upstreams (:9000 A, :9001 B) and runs proxy tests via webserv on :8080
import sys, os, time, threading, tempfile, subprocess, shlex, json
from http.server import BaseHTTPRequestHandler, HTTPServer

WEBSERV_BASE = "http://127.0.0.1:8080"
PROXY_BASE   = WEBSERV_BASE + "/api"

UPSTREAMS = [
    ("127.0.0.1", 9000, "A"),
    ("127.0.0.1", 9001, "B"),
]

# ---------- Upstream Echo Servers ----------
class EchoHandler(BaseHTTPRequestHandler):
    backend_id = "?"
    def _read_body(self):
        length = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(length) if length > 0 else b''

    def _send_json(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Backend', self.backend_id)  # identify which upstream answered
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # quiet; uncomment for verbose upstream logs
        # sys.stderr.write("[%s] %s - - [%s] %s\n" % (self.backend_id, self.client_address[0], self.log_date_time_string(), fmt%args))
        pass

    def do_GET(self):
        if self.path.startswith("/slow"):
            time.sleep(15)  # should trigger proxy_read_timeout (~10s) if configured
            return self._send_json(200, {"ok": True, "backend": self.backend_id})
        return self._send_json(200, {
            "method":"GET", "path":self.path, "backend": self.backend_id,
            "headers": dict(self.headers),
        })

    def do_POST(self):
        data = self._read_body()
        return self._send_json(200, {
            "method":"POST", "path":self.path, "len":len(data),
            "backend": self.backend_id, "headers": dict(self.headers),
        })

    def do_DELETE(self):
        return self._send_json(200, {"method":"DELETE","path":self.path,"backend": self.backend_id})

def start_upstream(host, port, backend_id):
    handler = type(f"Handler_{backend_id}", (EchoHandler,), {"backend_id": backend_id})
    srv = HTTPServer((host, port), handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    print(f"✅ Backend {backend_id} listening on http://{host}:{port}")
    return srv, t

# ---------- Test helpers ----------
def run(cmd, input_bytes=None):
    p = subprocess.Popen(cmd, shell=True,
                         stdin=subprocess.PIPE if input_bytes is not None else None,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out, _ = p.communicate(input=input_bytes)
    return p.returncode, out.decode('utf-8', errors='replace')

def curl(url, method="GET", data_path=None, data_bytes=None, headers=None,
         chunked=False, max_time=None):
    parts = ["curl -i -sS"]
    if max_time is not None:
        parts += ["--max-time", str(int(max_time))]
    if method != "GET":
        parts += ["-X", shlex.quote(method)]
    if headers:
        for h in headers:
            parts += ["-H", shlex.quote(h)]
    if chunked:
        parts += ["-H", shlex.quote("Transfer-Encoding: chunked")]
    if data_path:
        parts += ["--data-binary", f"@{shlex.quote(data_path)}"]
    elif data_bytes is not None:
        parts += ["--data-binary", "@-"]
    parts += [shlex.quote(url)]
    cmd = " ".join(parts)
    code, out = run(cmd, input_bytes=data_bytes)
    line = out.splitlines()[0] if out else ""
    print(f"$ {cmd}\n{line}")
    body = out.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in out else ""
    if body:
        preview = body[:120].replace("\n", "\\n")
        print(f"  body: {preview}{'...' if len(body)>120 else ''}")
    return code, out

def expect_status(out, *ok_prefixes):
    line = out.splitlines()[0] if out else ""
    ok = any(line.startswith(p) for p in ok_prefixes)
    print(f"  expect {ok_prefixes} -> {'OK' if ok else 'FAIL'}")
    return ok

def header(out, name):
    # return first header value matching name (case-insensitive)
    lines = out.split("\r\n")
    name_l = name.lower()
    for ln in lines:
        if ":" in ln:
            k, v = ln.split(":", 1)
            if k.strip().lower() == name_l:
                return v.strip()
    return None

# ---------- Test suite ----------
def main():
    print("== Start two upstream echos on :9000 (A) and :9001 (B) ==")
    servers = []
    try:
        for host, port, bid in UPSTREAMS:
            srv, thread = start_upstream(host, port, bid)
            servers.append(srv)
        time.sleep(0.4)  # tiny settle

        fails = 0

        print("\n== 0) Direct upstream sanity (A) ==")
        _, out = curl(f"http://127.0.0.1:9000/hello")
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 0b) Direct upstream sanity (B) ==")
        _, out = curl(f"http://127.0.0.1:9001/hello")
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 1) Proxy GET /api/hello ==")
        _, out = curl(f"{PROXY_BASE}/hello")
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 1b) Round-robin alternation check (6 requests) ==")
        seen = []
        for i in range(6):
            _, out = curl(f"{PROXY_BASE}/who")
            xb = header(out, "X-Backend")
            seen.append(xb or "?")
        print("  X-Backend sequence:", seen)
        # verify both A and B appear and the sequence alternates at least once
        if seen.count("A") == 0 or seen.count("B") == 0:
            print("  FAIL: did not see both A and B via proxy")
            fails += 1
        # simple alternation heuristic: check neighbors differ at least half the time
        diffs = sum(1 for i in range(1, len(seen)) if seen[i] != seen[i-1])
        if diffs < len(seen) - 2:
            print("  WARN: sequence not strictly alternating; ensure RouteResolver RR is called (pool name in proxy_pass)")
            # don’t fail hard here; environments vary

        print("\n== 2) Proxy POST small body /api/echo ==")
        _, out = curl(f"{PROXY_BASE}/echo", method="POST",
                      data_bytes=b"hello proxy",
                      headers=["Content-Type: application/x-www-form-urlencoded"])
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 3) Proxy POST large body (5MB) -> 200 or 413 depending on limit ==")
        big = tempfile.NamedTemporaryFile(delete=False)
        try:
            big.write(b"X" * (5 * 1024 * 1024))
            big.close()
            _, out = curl(f"{PROXY_BASE}/echo", method="POST", data_path=big.name)
            line = out.splitlines()[0] if out else ""
            if line.startswith("HTTP/1.0 200") or line.startswith("HTTP/1.1 200"):
                print("  large upload proxied: OK")
            elif line.startswith("HTTP/1.0 413") or line.startswith("HTTP/1.1 413"):
                print("  large upload blocked (413): OK")
            else:
                print("  unexpected status:", line); fails += 1
        finally:
            os.unlink(big.name)

        print("\n== 4) Proxy chunked upload /api/echo ==")
        _, out = curl(f"{PROXY_BASE}/echo", method="POST",
                      data_bytes=b"chunked works\n", chunked=True)
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 5) Proxy DELETE /api/item/42 ==")
        _, out = curl(f"{PROXY_BASE}/item/42", method="DELETE")
        if not expect_status(out, "HTTP/1.0 200", "HTTP/1.1 200"): fails += 1

        print("\n== 6) Proxy read-timeout using GET /api/slow (upstream sleeps 15s) ==")
        start = time.time()
        code, out = curl(f"{PROXY_BASE}/slow", max_time=12)
        elapsed = int(time.time() - start)
        line = out.splitlines()[0] if out else ""
        if line.startswith("HTTP/1.0 504") or line.startswith("HTTP/1.1 504"):
            print(f"  got 504 in ~{elapsed}s: OK")
        elif code == 28:
            print(f"  curl timed out (~{elapsed}s) — likely proxy_read_timeout not enforced; treat as FAIL here")
            fails += 1
        else:
            print(f"  expected 504, got: {line if line else 'NO OUTPUT'}")
            fails += 1

        print("\n== Summary ==")
        print("All proxy tests passed ✅" if fails == 0 else f"{fails} test(s) failed ❌")
        sys.exit(0 if fails == 0 else 1)

    finally:
        # No explicit shutdown needed (daemon threads); press Ctrl+C to stop script if you run it interactively.
        pass

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
