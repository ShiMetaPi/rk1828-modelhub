#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PaddleOCR-VL Python web layer: HTTP API + C++ engine process management.

Page-driven lifecycle (matches vl/tts/asr/depth/yolo26 demos):
  - GET /              -> ocr.html
  - GET /api/status    -> engine load state (loads on first call)
  - POST /api/ocr      -> multipart image upload; SSE streams {"ev":"token",text:..}
                          followed by {"ev":"done", vision_ms, llm_ms, ...}
  - POST /api/bye      -> page closed, unload NPU
  - GET /api/ping      -> liveness

Engine C++ binary path is passed via --engine-bin; model dir via --model-dir.
The C++ engine runs as a subprocess bound to a Unix socket /tmp/ocr_engine.sock.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── config (overridable via CLI / env) ────────────────────────────────
_THIS_DIR   = os.path.dirname(os.path.abspath(__file__))
PORT        = int(os.environ.get("OCR_PORT", "8093"))
ENGINE_BIN  = os.environ.get("OCR_ENGINE_BIN", os.path.join(_THIS_DIR, "engine", "ocr_engine"))
MODEL_DIR   = os.environ.get("OCR_MODEL_DIR",  _THIS_DIR)
ENGINE_LIB  = os.environ.get("OCR_ENGINE_LIB", os.path.join(_THIS_DIR, "lib"))
SOCK        = os.environ.get("OCR_SOCK", "/tmp/ocr_engine.sock")
ENGINE_LOG  = os.environ.get("OCR_ENGINE_LOG", os.path.join(_THIS_DIR, "ocr_engine.log"))

KEEPALIVE_INTERVAL = 5
RELEASE_AFTER      = 75     # seconds of no traffic -> unload NPU
BYE_EXIT_AFTER     = 10     # grace period after /api/bye -> exit python
SERVICE_EXIT_AFTER = 300    # absolute cap: any demo idling > 5 min exits

LAST_SEEN = [time.time()]
BYE_AT    = [None]
SERVER    = None
_active   = 0
_active_lock = threading.Lock()
_load_state  = {"state": "idle", "err": "", "load_ms": 0}
_load_lock   = threading.Lock()

# ── C++ engine process management ──────────────────────────────────────
_engine_proc = None


def _engine_alive():
    """Engine is alive AND not a zombie (pkill'd children linger as <defunct>
    and would otherwise trick us into never restarting)."""
    try:
        r = subprocess.run(['ps', '-eo', 'stat,comm'], capture_output=True, timeout=3)
        for line in r.stdout.decode().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == 'ocr_engine' and not parts[0].startswith('Z'):
                return True
        return False
    except Exception:
        return False


def _engine_env():
    """Set LD_LIBRARY_PATH so the engine finds librknn3_api.so at runtime."""
    e = dict(os.environ)
    lib_paths = [ENGINE_LIB]
    existing = e.get("LD_LIBRARY_PATH", "")
    if existing:
        lib_paths.append(existing)
    e["LD_LIBRARY_PATH"] = ":".join(lib_paths)
    return e


def start_engine():
    global _engine_proc
    if _engine_alive():
        return True
    try:
        if os.path.exists(SOCK):
            try: os.unlink(SOCK)
            except Exception: pass
        log_fp = open(ENGINE_LOG, "a")
        _engine_proc = subprocess.Popen(
            [ENGINE_BIN, MODEL_DIR, SOCK, "0xff", "0xff", "0xff"],
            stdout=log_fp,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            env=_engine_env())
    except Exception as e:
        print(f"[ocr] start_engine failed: {e}", flush=True)
        return False
    for _ in range(40):
        if os.path.exists(SOCK) and _engine_alive():
            return True
        time.sleep(0.25)
    print("[ocr] WARN: engine did not bind socket in 10s", flush=True)
    return False


def stop_engine():
    global _engine_proc
    _engine_proc = None
    try:
        subprocess.run(['pkill', '-x', 'ocr_engine'], capture_output=True, timeout=5)
    except Exception:
        pass
    if os.path.exists(SOCK):
        try: os.unlink(SOCK)
        except Exception: pass


def _reap_children():
    for _ in range(8):
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid == 0:
                break
        except (ChildProcessError, OSError):
            break


def _send_req(req: dict, timeout=30):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCK)
        s.sendall((json.dumps(req, ensure_ascii=False) + "\n").encode())
        # Block until the engine's load emits {"ev":"done"} or {"ev":"error"}.
        buf = b""
        while b"\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        return json.loads(buf.decode("utf-8").strip().splitlines()[-1])
    finally:
        s.close()


def _preload():
    """Background thread: ask engine to load models on first /api/status."""
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
        _load_state["state"] = "loading"
    try:
        if not start_engine():
            _load_state.update(state="error", err="engine process failed to start")
            return
        resp = _send_req({"cmd": "load", "qid": 0}, timeout=300)
        if resp.get("ev") == "done":
            _load_state.update(state="ready", load_ms=resp.get("load_ms", 0), err="")
        else:
            _load_state.update(state="error", err=resp.get("msg", "load failed"))
    except Exception as e:
        _load_state.update(state="error", err=str(e))
    print(f"[ocr] preload: {_load_state}", flush=True)


def ensure_preload_async():
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
    threading.Thread(target=_preload, daemon=True).start()


def _exit_service(reason):
    print(f"[ocr] {reason}; service exiting (re-run start.sh to bring back)", flush=True)
    stop_engine()
    if SERVER is not None:
        SERVER.shutdown()
    else:
        os._exit(0)


def _keepalive():
    while True:
        time.sleep(KEEPALIVE_INTERVAL)
        _reap_children()
        now = time.time()
        with _active_lock:
            active = _active
        if active > 0:
            continue
        if now - LAST_SEEN[0] > RELEASE_AFTER and _engine_alive():
            stop_engine()
            with _load_lock:
                _load_state.update(state="idle", err="")
        if (BYE_AT[0] is not None and now - BYE_AT[0] > BYE_EXIT_AFTER) \
                or now - LAST_SEEN[0] > SERVICE_EXIT_AFTER:
            _exit_service("page closed" if BYE_AT[0] is not None else "long idle")
            return


threading.Thread(target=_keepalive, daemon=True).start()


def touch():
    LAST_SEEN[0] = time.time()
    BYE_AT[0] = None


# ── HTTP layer ────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def _set_cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")

    def send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self._set_cors()
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass  # quiet

    def do_GET(self):
        touch()
        path = self.path.split("?")[0]
        if path == "/api/status":
            ensure_preload_async()
            self.send_json(dict(_load_state))
            return
        if path == "/api/ping":
            self.send_json({"ok": True, "alive": _engine_alive(), "loaded": _load_state["state"] == "ready"})
            return
        if path in ("/", "/index.html"):
            full = os.path.join(_THIS_DIR, "ocr.html")
            if not os.path.exists(full):
                self.send_json({"error": "ocr.html missing"}, code=404)
                return
            with open(full, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_json({"error": "not found"}, code=404)

    def do_POST(self):
        touch()
        path = self.path.split("?")[0]

        if path == "/api/bye":
            stop_engine()
            with _load_lock:
                _load_state.update(state="idle", err="")
            BYE_AT[0] = time.time()
            self.send_json({"status": "released"})
            return

        if path == "/api/ocr":
            return self._handle_ocr()

        self.send_json({"error": "not found"}, code=404)

    # ── /api/ocr: multipart upload + SSE stream ────────────────────────
    def _handle_ocr(self):
        global _active
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("multipart/form-data"):
            self.send_json({"error": "multipart/form-data required"}, code=400)
            return

        # parse multipart manually (small payload; avoid extra deps)
        m = re.match(r'multipart/form-data;\s*boundary=(.+)', ctype)
        if not m:
            self.send_json({"error": "no boundary"}, code=400)
            return
        boundary = ('--' + m.group(1)).encode()
        content_len = int(self.headers.get("Content-Length", 0))
        if content_len <= 0:
            self.send_json({"error": "empty body"}, code=400)
            return
        body = self.rfile.read(content_len)

        # find file part
        parts = body.split(boundary)
        img_bytes = None
        img_ext = "jpg"
        for p in parts:
            if b'Content-Disposition' not in p:
                continue
            hdr_end = p.find(b'\r\n\r\n')
            if hdr_end < 0:
                continue
            headers = p[:hdr_end].decode('utf-8', errors='ignore')
            data = p[hdr_end + 4:]
            if data.endswith(b'\r\n'):
                data = data[:-2]
            if 'name="file"' in headers or 'name="image"' in headers:
                img_bytes = data
                if 'filename="' in headers:
                    fn = headers.split('filename="', 1)[1].split('"', 1)[0]
                    ext = os.path.splitext(fn)[1].lower().lstrip('.')
                    if ext in ("jpg", "jpeg", "png"):
                        img_ext = "jpg" if ext == "jpeg" else ext
                break
        if not img_bytes:
            self.send_json({"error": "no file part"}, code=400)
            return
        if len(img_bytes) > 20 * 1024 * 1024:
            self.send_json({"error": "image too large (>20MB)"}, code=400)
            return

        # serialise single inference
        with _active_lock:
            if _active > 0:
                self.send_json({"error": "busy with another request"}, code=409)
                return
            _active += 1

        qid = int(time.time() * 1000) & 0x7fffffff
        in_path = f"/tmp/ocr_in_{qid}.{img_ext}"
        with open(in_path, "wb") as f:
            f.write(img_bytes)

        # SSE response
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self._set_cors()
        self.end_headers()

        # helper to send SSE event and flush
        def emit(ev: dict):
            line = "data: " + json.dumps(ev, ensure_ascii=False) + "\n\n"
            try:
                self.wfile.write(line.encode("utf-8"))
                self.wfile.flush()
            except Exception as e:
                raise RuntimeError(f"client disconnected: {e}")

        # make sure engine is up + loaded
        try:
            if not start_engine():
                emit({"ev": "error", "msg": "engine failed to start"})
                with _active_lock: _active -= 1
                return
            if _load_state["state"] != "ready":
                # synchronous load right now (user just submitted, can't wait async)
                _load_state["state"] = "loading"
                resp = _send_req({"cmd": "load", "qid": qid}, timeout=300)
                if resp.get("ev") == "done":
                    _load_state.update(state="ready", load_ms=resp.get("load_ms", 0), err="")
                else:
                    _load_state.update(state="error", err=resp.get("msg", "load failed"))
                    emit({"ev": "error", "msg": resp.get("msg", "load failed")})
                    with _active_lock: _active -= 1
                    return
        except Exception as e:
            emit({"ev": "error", "msg": f"engine init: {e}"})
            with _active_lock: _active -= 1
            return

        # open streaming socket to engine
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(180)
            s.connect(SOCK)
            s.sendall((json.dumps({"cmd": "infer", "qid": qid,
                                   "img": in_path,
                                   "prompt": "ocr"}) + "\n").encode())

            buf = b""
            while True:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    emit({"ev": "error", "msg": "engine timeout"})
                    break
                if not chunk:
                    break
                buf += chunk
                # process complete lines
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        ev = json.loads(line.decode("utf-8"))
                    except Exception:
                        continue
                    emit(ev)
                    if ev.get("ev") == "done" or ev.get("ev") == "error":
                        s.close()
                        with _active_lock: _active -= 1
                        try: os.unlink(in_path)
                        except OSError: pass
                        return
        except Exception as e:
            try: emit({"ev": "error", "msg": f"socket: {e}"})
            except Exception: pass
        finally:
            try: s.close()
            except Exception: pass
            with _active_lock:
                _active -= 1
            try: os.unlink(in_path)
            except OSError: pass


# ── main ──────────────────────────────────────────────────────────────
def main():
    global PORT, ENGINE_BIN, MODEL_DIR, ENGINE_LIB, SOCK, ENGINE_LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--engine-bin", default=ENGINE_BIN)
    ap.add_argument("--model-dir", default=MODEL_DIR)
    ap.add_argument("--engine-lib", default=ENGINE_LIB)
    ap.add_argument("--sock", default=SOCK)
    ap.add_argument("--engine-log", default=ENGINE_LOG)
    a = ap.parse_args()
    PORT, ENGINE_BIN, MODEL_DIR, ENGINE_LIB, SOCK, ENGINE_LOG = \
        a.port, a.engine_bin, a.model_dir, a.engine_lib, a.sock, a.engine_log

    print(f"[ocr] listening :{PORT}  model_dir={MODEL_DIR}  engine={ENGINE_BIN}", flush=True)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    global SERVER
    SERVER = srv

    # open browser in DISPLAY=:0 unless explicitly suppressed
    if os.environ.get("OCR_OPEN_BROWSER") == "1":
        def _open():
            time.sleep(2)
            try:
                env = dict(os.environ)
                env.setdefault("DISPLAY", ":0")
                subprocess.Popen(
                    ["chromium", "--no-sandbox", "--disable-gpu", "--no-first-run",
                     "--new-window", f"http://127.0.0.1:{PORT}"],
                    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    start_new_session=True)
                print(f"[ocr] opened browser http://127.0.0.1:{PORT}", flush=True)
            except Exception as e:
                print(f"[ocr] open browser failed: {e}", flush=True)
        threading.Thread(target=_open, daemon=True).start()

    srv.serve_forever()
    print("[ocr] exited", flush=True)


if __name__ == "__main__":
    main()