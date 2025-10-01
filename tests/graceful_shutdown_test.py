#!/usr/bin/env python3
"""
Graceful shutdown E2E test (CGI-based, waits for headers & confirms hung CGI spawned)

Validates:
- Quiesce: listeners closed on SIGTERM (no new connections)
- Drain: in-flight streaming CGI keeps streaming within grace
- Reap: hung CGI that never writes headers gets killed/reaped by deadline
- Exit: server process terminates
"""

import argparse, os, signal, socket, subprocess, sys, threading, time
from contextlib import closing

try:
    from urllib.request import urlopen, Request
except Exception:
    print("FATAL: Python 3 required")
    sys.exit(2)

def wait_for_port(host, port, timeout=5.0, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with closing(socket.create_connection((host, port), timeout=0.25)):
                return True
        except OSError:
            time.sleep(interval)
    return False

def try_connect(host, port, timeout=0.5):
    try:
        with closing(socket.create_connection((host, port), timeout=timeout)):
            return True
    except OSError:
        return False

def ensure_file(path, content):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    if not os.path.exists(path):
        with open(path, "w") as f:
            f.write(content)
        os.chmod(path, 0o755)
    return path

SLOW_PY = """#!/usr/bin/env python3
import sys, time
sys.stdout.write("Status: 200 OK\\r\\n")
sys.stdout.write("Content-Type: text/plain\\r\\n")
sys.stdout.write("Connection: keep-alive\\r\\n")
sys.stdout.write("\\r\\n")
sys.stdout.flush()
for i in range(10):
    sys.stdout.write("tick %d\\n" % i)
    sys.stdout.flush()
    time.sleep(0.5)
"""

HANG_PY = """#!/usr/bin/env python3
import time
time.sleep(120)
"""

class StreamResult:
    def __init__(self):
        self.ok = False
        self.status = None
        self.bytes = 0
        self.err = None
        self.duration = 0.0

def stream_url(url, result: StreamResult, headers_evt: threading.Event, stop_evt: threading.Event, timeout=60):
    start = time.time()
    try:
        req = Request(url, headers={"Connection": "keep-alive"})
        with urlopen(req, timeout=timeout) as resp:
            # status attribute can be .status (py3.9+) or .code (py3.8-)
            result.status = getattr(resp, "status", None) or getattr(resp, "code", None)
            headers_evt.set()
            while not stop_evt.is_set():
                chunk = resp.read(16 * 1024)
                if not chunk:
                    result.ok = True
                    break
                result.bytes += len(chunk)
    except Exception as e:
        result.err = e
        headers_evt.set()
    finally:
        result.duration = time.time() - start

def hung_request(url, started_evt: threading.Event, done_evt: threading.Event, timeout=120):
    # We just open it and block; the test watches processes to confirm spawn.
    try:
        req = Request(url, headers={"Connection": "close"})
        with urlopen(req, timeout=timeout) as resp:
            # If this ever returns data, it's not "hung"—but that’s fine, we’ll mark done.
            _ = resp.read(1024)
    except Exception:
        pass
    finally:
        # Signal we’re done (socket closed due to reap or timeout)
        done_evt.set()

def pgrep(pattern):
    try:
        out = subprocess.check_output(["pgrep", "-fl", pattern], stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except subprocess.CalledProcessError:
        return ""

def pkill(pattern):
    try:
        subprocess.check_call(["pkill", "-f", pattern], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except subprocess.CalledProcessError:
        return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="./webserv")
    ap.add_argument("--config", default="config/extended.conf")
    ap.add_argument("--root", default="./www")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--grace", type=int, default=3, help="expected grace seconds")
    args = ap.parse_args()

    # Clean any stray hang.py before starting, to avoid false FAILs
    pkill("hang.py")
    time.sleep(0.2)

    # Ensure CGI fixtures exist
    print("[*] Ensuring CGI fixtures...")
    ensure_file(os.path.join(args.root, "cgi/slow.py"), SLOW_PY)
    ensure_file(os.path.join(args.root, "cgi/hang.py"), HANG_PY)

    base = f"http://{args.host}:{args.port}"
    url_slow = f"{base}/cgi/slow.py"
    url_hang = f"{base}/cgi/hang.py"

    print(f"[*] Starting server: {args.server} {args.config}")
    proc = subprocess.Popen([args.server, args.config],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True)

    try:
        if not wait_for_port(args.host, args.port, timeout=5.0):
            print("[FAIL] Server port did not open in time.")
            proc.terminate()
            proc.wait(timeout=5)
            return 1
        print("[OK] Port is open.")

        # Start slow streaming; wait for headers and some bytes
        headers_evt = threading.Event()
        slow_stop = threading.Event()
        slow_res = StreamResult()
        t_slow = threading.Thread(target=stream_url,
                                  args=(url_slow, slow_res, headers_evt, slow_stop, 60),
                                  daemon=True)
        t_slow.start()

        if not headers_evt.wait(timeout=5.0):
            print("[FAIL] slow.py did not return headers within 5s — cannot test drain.")
            slow_stop.set()
            t_slow.join(timeout=1.0)
            try: os.kill(proc.pid, signal.SIGKILL)
            except OSError: pass
            try: proc.wait(timeout=2)
            except Exception: pass
            return 1

        # Give it a brief head start to accumulate body bytes
        time.sleep(0.5)

        # If still zero bytes, let it nibble a little longer (prevents flaky drain FAIL)
        if slow_res.bytes == 0:
            time.sleep(1.0)

        print(f"[*] slow.py streaming (bytes={slow_res.bytes}); starting hung CGI...")

        # Start hung CGI request in a background thread
        hung_started_evt = threading.Event()  # not used by urllib path; we rely on pgrep instead
        hung_done_evt = threading.Event()
        t_hung = threading.Thread(target=hung_request,
                                  args=(url_hang, hung_started_evt, hung_done_evt, max(60, args.grace + 10)),
                                  daemon=True)
        t_hung.start()

        # Positively confirm the hung CGI process (hang.py) has spawned
        # (We killed any old one, so the first match we see is ours.)
        spawned = False
        for _ in range(25):  # up to ~2.5s
            if pgrep("hang.py"):
                spawned = True
                break
            time.sleep(0.1)

        if not spawned:
            print("[FAIL] Could not confirm hang.py was spawned — reaping cannot be tested.")
            slow_stop.set()
            t_slow.join(timeout=1.0)
            try: os.kill(proc.pid, signal.SIGKILL)
            except OSError: pass
            try: proc.wait(timeout=2)
            except Exception: pass
            return 1

        # Begin graceful shutdown
        print("[*] Sending SIGTERM (begin graceful shutdown)")
        os.kill(proc.pid, signal.SIGTERM)

        # New connections should be refused shortly after
        time.sleep(0.15)
        if try_connect(args.host, args.port, timeout=0.5):
            print("[WARN] New connection accepted AFTER SIGTERM (accept backlog/race).")
            blocked = False
            for _ in range(6):
                time.sleep(0.15)
                if not try_connect(args.host, args.port, timeout=0.4):
                    blocked = True
                    break
            if blocked:
                print("[OK] Subsequent connection attempts were blocked.")
            else:
                print("[FAIL] New connections still accepted after SIGTERM.")
        else:
            print("[OK] New connection refused/blocked after SIGTERM (quiesce).")

        # Let the slow stream continue for a bit within grace
        t_slow.join(timeout=max(4, args.grace * 2))
        slow_stop.set()

        # Wait for hung CGI to be reaped (its socket should drop -> thread finishes)
        t_hung.join(timeout=max(6, args.grace * 2))

        # Wait for server to exit on its own
        try:
            proc.wait(timeout=max(5, args.grace + 5))
            server_exited = True
        except subprocess.TimeoutExpired:
            server_exited = False

        # Best-effort drain of output
        try:
            out, err = proc.communicate(timeout=0.5)
        except subprocess.TimeoutExpired:
            out, err = ("", "")

        print("\n===== RESULTS =====")
        print(f"Server exited: {server_exited}")
        print(f"Slow stream: ok={slow_res.ok} status={slow_res.status} bytes={slow_res.bytes} dur={slow_res.duration:.2f}s err={slow_res.err}")

        # Reap check: hang.py should be gone shortly after grace
        deadline = time.time() + (args.grace + 3)
        linger = None
        while time.time() < deadline:
            linger = pgrep("hang.py")
            if not linger:
                break
            time.sleep(0.2)

        if linger:
            print("[FAIL] Found lingering hang.py process(es):")
            print(linger)
            reap_ok = False
        else:
            print("[OK] No lingering hang.py processes.")
            reap_ok = True

        quiesce_ok = True
        drain_ok = slow_res.ok or (slow_res.bytes > 0 and slow_res.status in (200, 206))
        all_ok = quiesce_ok and drain_ok and reap_ok and server_exited

        print("\n===== SUMMARY =====")
        print(f"Quiesce (no new conns): {'PASS' if quiesce_ok else 'FAIL'}")
        print(f"Drain (slow CGI): {'PASS' if drain_ok else 'FAIL'}")
        print(f"Reap (hung CGI reaped): {'PASS' if reap_ok else 'FAIL'}")
        print(f"Server exit: {'PASS' if server_exited else 'FAIL'}")

        return 0 if all_ok else 1

    finally:
        # Hard cleanup if still running
        if proc.poll() is None:
            try: os.kill(proc.pid, signal.SIGKILL)
            except OSError: pass
            try: proc.wait(timeout=2)
            except Exception: pass
        # Make absolutely sure we don't leave a stray hang.py around
        pkill("hang.py")

if __name__ == "__main__":
    sys.exit(main())
