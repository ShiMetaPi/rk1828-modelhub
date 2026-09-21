#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Depth Python 包装：启动 C++ depth_engine 进程，暴露 HTTP 接口。
页面通过 /api/capture 提交 JPEG，返回深度热力图 PNG（数据在响应头 X-Depth-Stats）。

管线：浏览器抓帧 JPEG → depth_engine(C++) 三段模型 → turbo 深度图 PNG
依赖：标准库 http.server（无 Flask）
"""
import json
import os
import socket
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── 配置 ──────────────────────────────────────────────────────────────
_THIS_DIR       = os.path.dirname(os.path.abspath(__file__))
DEPTH_MODEL_DIR = os.environ.get("DEPTH_MODEL_DIR", os.path.join(_THIS_DIR, "model"))
DEPTH_BIN       = os.environ.get("DEPTH_BIN", os.path.join(_THIS_DIR, "depth_engine"))
DEPTH_SOCK      = os.environ.get("DEPTH_SOCK", "/tmp/depth_engine.sock")
PORT            = 8091

# ── C++ 引擎管理 ─────────────────────────────────────────────────────
_engine_proc = None  # module-level 防止 GC 回收 Popen（会导致 SIGPIPE 杀子进程）
_load_state = {"state": "idle", "err": "", "views": 0, "res": "", "load_ms": 0}
_load_lock = threading.Lock()

def _engine_alive():
    """检查 C++ 引擎进程是否在跑（排除僵尸：pgrep 会把 <defunct> 也算上，
    僵尸算活着会导致引擎死了永不再拉起）"""
    try:
        r = subprocess.run(['ps', '-eo', 'stat,comm'], capture_output=True, timeout=3)
        for line in r.stdout.decode().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == 'depth_engine' and not parts[0].startswith('Z'):
                return True
        return False
    except Exception:
        return False

def start_engine():
    """启动 C++ 引擎进程（socket 立即可用，模型按 load 命令懒加载）"""
    global _engine_proc
    if not _engine_alive():
        if os.path.exists(DEPTH_SOCK):
            try:
                os.unlink(DEPTH_SOCK)
            except Exception:
                pass
        _engine_proc = subprocess.Popen(
            [DEPTH_BIN, DEPTH_MODEL_DIR, DEPTH_SOCK],
            stdout=open("/tmp/depth_engine.stdout", "a"),
            stderr=subprocess.STDOUT,
            start_new_session=True)
    # 等 socket 就绪（引擎 bind 很快，给 10s 足够）
    for _ in range(20):
        if os.path.exists(DEPTH_SOCK) and _engine_alive():
            return True
        time.sleep(0.5)
    print("[depth] WARN: 引擎进程 10s 内未就绪", flush=True)
    return False

def stop_engine():
    global _engine_proc
    _engine_proc = None
    try:
        subprocess.run(['pkill', '-x', 'depth_engine'], capture_output=True, timeout=5)
    except Exception:
        pass
    if os.path.exists(DEPTH_SOCK):
        try:
            os.unlink(DEPTH_SOCK)
        except Exception:
            pass

def _raw_send(req: dict, timeout=180) -> dict:
    """向引擎 socket 发 JSON 命令并读一行响应"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(DEPTH_SOCK)
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

def sock_send(req: dict, timeout=180) -> dict:
    if not (os.path.exists(DEPTH_SOCK) and _engine_alive()):
        if not start_engine():
            raise RuntimeError("引擎启动失败（看 /tmp/depth_engine.stdout）")
    return _raw_send(req, timeout)

def _preload():
    """后台加载三段模型（页面打开时触发，用户取景的功夫模型就加载好了）"""
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
        _load_state["state"] = "loading"
    try:
        start_engine()
        resp = _raw_send({"cmd": "load", "qid": 1}, timeout=300)
        if resp.get("ev") == "done":
            _load_state.update(state="ready", views=resp.get("views", 0),
                               res=resp.get("res", ""), load_ms=resp.get("load_ms", 0),
                               err="")
        else:
            _load_state.update(state="error", err=resp.get("msg", "加载失败"))
    except Exception as e:
        _load_state.update(state="error", err=str(e))
    print("[depth] preload: %s" % _load_state, flush=True)

def ensure_preload_async():
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
    threading.Thread(target=_preload, daemon=True).start()

# ── NPU 生命周期管理（page-driven，与 tts/asr 同款）─────────────────
LAST_SEEN = [time.time()]
BYE_AT = [None]
SERVER = None
KEEPALIVE_INTERVAL = 5
RELEASE_AFTER = 75        # 空闲多久释放 NPU（杀引擎进程 = 卸模型）
BYE_EXIT_AFTER = 10       # 页面关闭后多久退出服务（宽限期，防 F5 刷新误杀）
SERVICE_EXIT_AFTER = 300  # 长时间无任何活动则退出（浏览器异常死掉的兜底）
_active = 0               # 进行中的推理数，>0 时不释放引擎
_active_lock = threading.Lock()

def _exit_service(reason):
    print(f"[depth] {reason}，服务退出，可重新运行 start.sh", flush=True)
    stop_engine()
    if SERVER is not None:
        SERVER.shutdown()
    else:
        os._exit(0)

def _reap_children():
    """回收引擎子进程残骸（pkill 杀掉的引擎会留僵尸挂在 python 下）"""
    for _ in range(8):
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid == 0:
                break
        except (ChildProcessError, OSError):
            break

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
            _load_state.update(state="idle")
        if (BYE_AT[0] is not None and now - BYE_AT[0] > BYE_EXIT_AFTER) \
                or now - LAST_SEEN[0] > SERVICE_EXIT_AFTER:
            _exit_service("页面已关闭" if BYE_AT[0] is not None else "长时间无活动")
            return

threading.Thread(target=_keepalive, daemon=True).start()

def touch():
    LAST_SEEN[0] = time.time()
    BYE_AT[0] = None

# ── HTTP 接口 ────────────────────────────────────────────────────────
_seq = 0

class Handler(BaseHTTPRequestHandler):
    def _status(self):
        ensure_preload_async()
        st = dict(_load_state)
        # 加载中就不 ping 了（引擎在同步加载，accept 队列会卡住 ping 直到超时）
        if st["state"] == "ready" and _engine_alive() and os.path.exists(DEPTH_SOCK):
            try:
                r = _raw_send({"cmd": "ping"}, timeout=5)
                if r.get("loaded"):
                    st["state"] = "ready"
                    st["views"] = r.get("views", 0)
                    st["res"] = r.get("res", "")
                    st["load_ms"] = r.get("load_ms", 0)
            except Exception:
                pass
        self.send_json(st)

    def do_GET(self):
        touch()
        if self.path == "/api/status":
            self._status()
            return
        # /demo/<name>：金标准图 / 对比稿页面的受限静态路由
        # 只放行 demo_ 前缀（金标准图对）和 depth_ 前缀（各设计稿页面），扩展名白名单，防任意文件读取
        if self.path.startswith("/demo/"):
            # 先剥掉查询串再取文件名，否则 ?nocam=1 会拼进名字里匹配不到白名单
            name = os.path.basename(urllib.parse.unquote(self.path[len("/demo/"):].split("?")[0]))
            ctype = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png",
                     ".html": "text/html; charset=utf-8"}.get(os.path.splitext(name)[1].lower())
            full = os.path.join(os.path.dirname(__file__), name)
            if ctype and (name.startswith("demo_") or name.startswith("depth_")) \
                    and os.path.exists(full):
                with open(full, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_json({"error": "not found"}, code=404)
            return
        if self.path == "/" or self.path == "/index.html":
            path = os.path.join(os.path.dirname(__file__), "depth.html")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_json({"error": "depth.html 不存在"}, code=404)
        else:
            self.send_json({"error": "not found"}, code=404)

    def do_POST(self):
        touch()
        if self.path == "/api/status":
            self._status()

        elif self.path == "/api/capture":
            global _seq, _active
            content_len = int(self.headers.get("Content-Length", 0))
            if content_len < 1024:
                self.send_json({"error": "图片数据缺失"}, code=400)
                return
            if content_len > 15 * 1024 * 1024:
                self.send_json({"error": "图片过大（上限 15MB）"}, code=400)
                return
            data = self.rfile.read(content_len)
            # JPEG SOI 校验（浏览器 canvas.toBlob('image/jpeg')）
            if data[:2] != b"\xff\xd8":
                self.send_json({"error": "不是 JPEG 图片"}, code=400)
                return

            with _active_lock:
                if _active > 0:
                    self.send_json({"error": "正在处理上一张，稍等"}, code=409)
                    return
                _active += 1
            _seq += 1
            jid = _seq
            in_path = f"/tmp/depth_in_{jid}.jpg"
            out_path = f"/tmp/depth_out_{jid}.png"
            with open(in_path, "wb") as f:
                f.write(data)
            try:
                resp = sock_send({"cmd": "infer", "qid": jid, "img": in_path, "out": out_path},
                                 timeout=300)
            except Exception as e:
                with _active_lock:
                    _active -= 1
                self.send_json({"error": f"引擎通信失败: {e}"}, code=500)
                return
            with _active_lock:
                _active -= 1

            if resp.get("ev") == "error":
                self.send_json({"error": resp.get("msg", "推理失败")}, code=500)
                return
            if not os.path.exists(out_path):
                self.send_json({"error": "深度图未生成"}, code=500)
                return

            with open(out_path, "rb") as f:
                png = f.read()
            stats = json.dumps({"ms": resp.get("ms", {}), "dmin": resp.get("dmin"),
                                "dmax": resp.get("dmax"), "views": resp.get("views", 0),
                                "res": _load_state.get("res", "")}, ensure_ascii=False)
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("X-Depth-Stats", stats.encode("utf-8").decode("latin-1"))
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            self.wfile.write(png)
            # 输入图不再需要；输出图留着也无妨，重启清 /tmp
            try:
                os.unlink(in_path)
            except OSError:
                pass

        elif self.path == "/api/bye":
            stop_engine()
            _load_state.update(state="idle")
            BYE_AT[0] = time.time()
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
    print(f"[depth] listening :{PORT}  model_dir={DEPTH_MODEL_DIR}", flush=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    SERVER = server

    def _open_browser():
        time.sleep(2)
        if os.environ.get("DEPTH_OPEN_BROWSER") != "1":
            return  # 默认不开（后台维护/测试时避免乱弹窗口，页面还会自动开摄像头）
        try:
            env = dict(os.environ)
            env.setdefault("DISPLAY", ":0")
            subprocess.Popen(
                ["chromium", "--no-sandbox", "--disable-gpu", "--no-first-run",
                 "--new-window", f"http://127.0.0.1:{PORT}"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            print("[depth] 已打开浏览器 http://127.0.0.1:%d" % PORT, flush=True)
        except Exception as e:
            print("[depth] 自动打开浏览器失败: %s" % e, flush=True)

    threading.Thread(target=_open_browser, daemon=True).start()
    server.serve_forever()
    print("[depth] 已退出", flush=True)
