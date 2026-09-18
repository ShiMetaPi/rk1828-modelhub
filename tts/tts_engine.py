#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TTS Python 包装：启动 C++ tts_engine 进程，暴露 HTTP 接口。
页面通过 /api/speak 提交文本，返回 WAV 文件。

管线：文本 → tts_engine(C++) → WAV
依赖：标准库 http.server（无 Flask）
"""
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── 配置 ──────────────────────────────────────────────────────────────
TTS_MODEL_DIR = os.environ.get("TTS_MODEL_DIR", "/userdata/models/qwen3-tts")
TTS_BIN       = os.environ.get("TTS_BIN", "/root/tts_demo/tts_engine")
TTS_SOCK      = os.environ.get("TTS_SOCK", "/tmp/tts_engine.sock")
OUTPUT_DIR    = "/tmp"
PORT          = 8088

# ── C++ 引擎管理（pgrep 检测存活）──────────────────────────────
_engine_proc = None  # module-level 防止 GC 回收 Popen（会导致 SIGPIPE 杀子进程）

def _engine_alive():
    """检查 C++ 引擎进程是否在跑（排除 Python 进程）"""
    try:
        # 精确匹配 C++ 二进制路径前缀，避免匹配 python3 tts_engine.py
        r = subprocess.run(['pgrep', '-f', '^/root/tts_demo/tts_engine '],
                          capture_output=True, timeout=3)
        return r.returncode == 0 and r.stdout.strip()
    except Exception:
        return False

def start_engine():
    """启动 C++ 引擎（如未运行）"""
    global _engine_proc
    if _engine_alive():
        return
    os.makedirs(os.path.dirname(TTS_SOCK), exist_ok=True)
    # 清理僵尸 socket（如果对应进程已死）
    if os.path.exists(TTS_SOCK) and not _engine_alive():
        try:
            os.unlink(TTS_SOCK)
        except Exception:
            pass
    # start_new_session=True → fork + setsid 成独立 session
    # _engine_proc 存模块级防止 GC 回收 Popen（避免 SIGPIPE 杀子进程）
    _engine_proc = subprocess.Popen(
        [TTS_BIN, TTS_MODEL_DIR, TTS_SOCK],
        stdout=open("/tmp/tts_engine.stdout", "a"),
        stderr=subprocess.STDOUT,
        start_new_session=True)
    for _ in range(60):
        if os.path.exists(TTS_SOCK) and _engine_alive():
            break
        time.sleep(0.5)
    print("[tts] engine started", flush=True)

def stop_engine():
    """停止 C++ 引擎"""
    global _engine_proc
    _engine_proc = None  # 允许 GC 回收（但 pkill 已杀进程，不会 SIGPIPE）
    try:
        subprocess.run(['pkill', '-f', '^/root/tts_demo/tts_engine '],
                      capture_output=True, timeout=5)
    except Exception:
        pass
    if os.path.exists(TTS_SOCK):
        try:
            os.unlink(TTS_SOCK)
        except Exception:
            pass

# ── Socket 客户端 ─────────────────────────────────────────────────────
def sock_send(req: dict) -> dict:
    """向 tts_engine 发 JSON 命令，接收响应."""
    if not _engine_alive():
        start_engine()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(300)
    try:
        s.connect(TTS_SOCK)
        s.sendall((json.dumps(req, ensure_ascii=False) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        return json.loads(buf.decode("utf-8"))
    finally:
        s.close()

# ── NPU 生命周期管理（page-driven）──────────────────────────────────
LAST_SEEN = [time.time()]
KEEPALIVE_INTERVAL = 15
RELEASE_AFTER = 75
_engine_lock = threading.Lock()

def _keepalive():
    while True:
        time.sleep(KEEPALIVE_INTERVAL)
        with _engine_lock:
            if time.time() - LAST_SEEN[0] > RELEASE_AFTER:
                stop_engine()

threading.Thread(target=_keepalive, daemon=True).start()

def touch():
    LAST_SEEN[0] = time.time()

# ── HTTP 接口 ────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def _status(self):
        """探活 C++ 引擎，返回状态 JSON。"""
        try:
            sock_send({"cmd": "ping"})
            self.send_json({"status": "ready", "engine": "ok"})
        except Exception as e:
            self.send_json({"status": "loading", "engine": str(e)})

    def do_GET(self):
        if self.path == "/api/status":
            self._status()
            return
        if self.path == "/" or self.path == "/index.html":
            path = os.path.join(os.path.dirname(__file__), "tts.html")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_json({"error": "tts.html 不存在"}, code=404)
        else:
            self.send_json({"error": "not found"}, code=404)

    def do_POST(self):
        touch()
        if self.path == "/api/status":
            self._status()

        elif self.path == "/api/speak":
            content_len = int(self.headers.get("Content-Length", 0))
            body_bytes = self.rfile.read(content_len) if content_len else b"{}"
            try:
                data = json.loads(body_bytes.decode("utf-8", errors="replace"))
            except Exception:
                data = {}
            text = data.get("text", "").strip()
            if not text:
                self.send_json({"error": "text 为空"}, code=400)
                return

            instruct = data.get("instruct", "").strip()
            speaker = data.get("speaker", "").strip()

            wav_out = os.path.join(OUTPUT_DIR, f"tts_{os.getpid()}_{len(text)}.wav")
            try:
                resp = sock_send({"cmd": "speak", "text": text, "instruct": instruct, "speaker": speaker, "qid": 1})
            except Exception as e:
                self.send_json({"error": f"引擎通信失败: {e}"}, code=500)
                return

            if resp.get("ev") == "error":
                self.send_json({"error": resp.get("msg", "未知错误")}, code=500)
                return

            wav_path = resp.get("wav", "")
            if not os.path.exists(wav_path):
                self.send_json({"error": f"WAV 文件未生成: {wav_path}"}, code=500)
                return

            with open(wav_path, "rb") as f:
                wav_data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", len(wav_data))
            self.send_header("Content-Disposition", "attachment; filename=output.wav")
            self.end_headers()
            self.wfile.write(wav_data)

        elif self.path == "/api/bye":
            stop_engine()
            self.send_json({"status": "released"})

        else:
            self.send_json({"error": "not found"}, code=404)

    def send_json(self, data, code=200):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    start_engine()
    print(f"[tts] listening :{PORT}  model_dir={TTS_MODEL_DIR}", flush=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.serve_forever()
