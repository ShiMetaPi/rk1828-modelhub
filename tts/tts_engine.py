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

_start_lock = threading.Lock()  # 串行化引擎启动，避免并发请求启动多个引擎进程抢 NPU

def start_engine():
    """启动 C++ 引擎（如未运行）。持锁串行化，避免并发启动多个进程。"""
    global _engine_proc
    with _start_lock:
        if not _engine_alive():
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
        # 无条件等 socket 就绪（最多 90s）：进程 spawn 后立刻存在
        # （_engine_alive() 为 True）但 socket 要等模型加载完才 bind，
        # 这期间直接 connect 会报 ENOENT，所以这里必须覆盖该窗口。
        for _ in range(180):
            if os.path.exists(TTS_SOCK) and _engine_alive():
                break
            time.sleep(0.5)
        if not (os.path.exists(TTS_SOCK) and _engine_alive()):
            print("[tts] WARN: 引擎 90s 内未就绪，可能加载失败", flush=True)
    print("[tts] engine started", flush=True)

def stop_engine():
    """停止 C++ 引擎。持 _start_lock 与 start_engine 互斥，避免杀死正在加载的引擎。"""
    global _engine_proc
    with _start_lock:
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

# 后台异步加载引擎（page-driven：页面打开即触发，避免阻塞 HTTP 服务启动）
_load_thread = None
_load_lock = threading.Lock()

def ensure_engine_async():
    """幂等地在后台线程加载引擎。已就绪或已在加载则直接返回。"""
    global _load_thread
    if os.path.exists(TTS_SOCK) and _engine_alive():
        return
    with _load_lock:
        if _load_thread is not None and _load_thread.is_alive():
            return
        _load_thread = threading.Thread(target=start_engine, daemon=True)
        _load_thread.start()

# ── Socket 客户端 ─────────────────────────────────────────────────────
def _raw_send(req: dict) -> dict:
    """向已就绪的 socket 发 JSON 命令并读一行响应。调用前需确认引擎已就绪。"""
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

def sock_send(req: dict) -> dict:
    """向 tts_engine 发 JSON 命令，接收响应."""
    global _active
    is_speak = req.get("cmd") == "speak"
    if is_speak:
        with _active_lock:
            _active += 1
    try:
        start_engine()  # 总是确保引擎就绪（进程已在加载时也会等 socket 建好）
        if not os.path.exists(TTS_SOCK):
            raise RuntimeError("引擎加载失败（90s 内 socket 未就绪，可能 NPU 异常）")
        return _raw_send(req)
    finally:
        if is_speak:
            with _active_lock:
                _active -= 1

# ── NPU 生命周期管理（page-driven）──────────────────────────────────
LAST_SEEN = [time.time()]
KEEPALIVE_INTERVAL = 15
RELEASE_AFTER = 75
_engine_lock = threading.Lock()
_active = 0            # 正在进行的合成请求数，>0 时不释放引擎（避免生成中途被停）
_active_lock = threading.Lock()

def _keepalive():
    while True:
        time.sleep(KEEPALIVE_INTERVAL)
        with _engine_lock:
            if time.time() - LAST_SEEN[0] > RELEASE_AFTER and _active == 0:
                stop_engine()

threading.Thread(target=_keepalive, daemon=True).start()

def touch():
    LAST_SEEN[0] = time.time()

# ── HTTP 接口 ────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def _status(self):
        """探活 C++ 引擎，返回状态 JSON（不阻塞：加载中直接返回 loading）。"""
        ensure_engine_async()
        if os.path.exists(TTS_SOCK) and _engine_alive():
            try:
                _raw_send({"cmd": "ping"})
                self.send_json({"status": "ready", "engine": "ok"})
            except Exception as e:
                self.send_json({"status": "loading", "engine": str(e)})
        else:
            self.send_json({"status": "loading", "engine": "模型加载中…"})

    def do_GET(self):
        touch()
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
    # Web 服务立即监听；引擎由页面打开时经 /api/status 触发后台加载，
    # 避免启动脚本被 ~40s 的模型加载阻塞。
    print(f"[tts] listening :{PORT}  model_dir={TTS_MODEL_DIR}", flush=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)

    # 服务就绪后自动在板子桌面打开浏览器（DISPLAY 默认 :0，桌面经 x11vnc/noVNC 呈现）
    def _open_browser():
        time.sleep(2)  # 等 serve_forever 进入监听循环
        try:
            env = dict(os.environ)
            env.setdefault("DISPLAY", ":0")
            subprocess.Popen(
                ["chromium", "--no-sandbox", "--disable-gpu", "--no-first-run",
                 "--new-window", f"http://127.0.0.1:{PORT}"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            print("[tts] 已打开浏览器 http://127.0.0.1:%d" % PORT, flush=True)
        except Exception as e:
            print("[tts] 自动打开浏览器失败: %s" % e, flush=True)

    threading.Thread(target=_open_browser, daemon=True).start()
    server.serve_forever()
