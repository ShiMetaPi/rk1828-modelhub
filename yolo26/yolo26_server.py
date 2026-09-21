#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
YOLO26 Python 包装：启动 C++ yolo26_engine 进程，暴露 HTTP 接口。
页面通过 /api/capture 提交 JPEG（已 letterbox 成 640×640 的方图），
返回引擎 JSON：目标框/类别/分数 + 实例分割 mask（bbox-local 1-bit base64）。

管线：浏览器抓帧 letterbox JPEG → yolo26_engine(C++) W8A8 推理 → JSON 回传，
画框画 mask 由浏览器 canvas 完成（引擎不产图，撑得起实时流）。
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
_THIS_DIR      = os.path.dirname(os.path.abspath(__file__))
YOLO_MODEL_DIR = os.environ.get("YOLO_MODEL_DIR", os.path.join(_THIS_DIR, "model"))
YOLO_BIN       = os.environ.get("YOLO_BIN", os.path.join(_THIS_DIR, "yolo26_engine"))
YOLO_SOCK      = os.environ.get("YOLO_SOCK", "/tmp/yolo26_engine.sock")
PORT           = 8092
TASK           = "seg"  # 当前任务（页面 /api/task 可切换）
TASKS          = ("det", "seg", "pose")  # 已接入的任务
# 帧文件走 tmpfs（裸 RGB 一帧 1.2MB，避免 eMMC 读写拖慢实时环路）
TMPDIR         = "/dev/shm" if os.path.isdir("/dev/shm") else "/tmp"

# ── C++ 引擎管理 ─────────────────────────────────────────────────────
_engine_proc = None  # module-level 防止 GC 回收 Popen（会导致 SIGPIPE 杀子进程）
_load_state = {"state": "idle", "err": "", "task": "", "load_ms": 0}
_load_lock = threading.Lock()

def _engine_alive():
    """检查 C++ 引擎进程是否在跑（排除僵尸：pgrep 会把 <defunct> 也算上，
    僵尸算活着会导致引擎死了永不再拉起）"""
    try:
        r = subprocess.run(['ps', '-eo', 'stat,comm'], capture_output=True, timeout=3)
        for line in r.stdout.decode().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == 'yolo26_engine' and not parts[0].startswith('Z'):
                return True
        return False
    except Exception:
        return False

def start_engine():
    """启动 C++ 引擎进程（socket 立即可用，模型按 load 命令懒加载）"""
    global _engine_proc
    if not _engine_alive():
        if os.path.exists(YOLO_SOCK):
            try:
                os.unlink(YOLO_SOCK)
            except Exception:
                pass
        _engine_proc = subprocess.Popen(
            [YOLO_BIN, YOLO_MODEL_DIR, YOLO_SOCK],
            stdout=open("/tmp/yolo26_engine.stdout", "a"),
            stderr=subprocess.STDOUT,
            start_new_session=True)
    # 等 socket 就绪（引擎 bind 很快，给 10s 足够）
    for _ in range(20):
        if os.path.exists(YOLO_SOCK) and _engine_alive():
            return True
        time.sleep(0.5)
    print("[yolo26] WARN: 引擎进程 10s 内未就绪", flush=True)
    return False

def stop_engine():
    global _engine_proc
    _engine_proc = None
    try:
        subprocess.run(['pkill', '-x', 'yolo26_engine'], capture_output=True, timeout=5)
    except Exception:
        pass
    if os.path.exists(YOLO_SOCK):
        try:
            os.unlink(YOLO_SOCK)
        except Exception:
            pass

def _raw_send(req: dict, timeout=180) -> dict:
    """向引擎 socket 发 JSON 命令并读一行响应"""
    return json.loads(_raw_send_bytes(
        (json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"), timeout))

def _raw_send_bytes(req_bytes: bytes, timeout=180) -> bytes:
    """同上但返回原始行字节（capture 透传用，免二次编解码）"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(YOLO_SOCK)
        s.sendall(req_bytes)
        buf = b""
        while b"\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        return buf
    finally:
        s.close()

def sock_send(req: dict, timeout=180) -> dict:
    return json.loads(sock_send_raw(req, timeout).decode("utf-8"))

def sock_send_raw(req: dict, timeout=180) -> bytes:
    """直接发；失败才认为是引擎死了（每次都 fork ps 探活一次 30-50ms，
    实测是每帧最大隐形开销，绝不能放进热路径）"""
    payload = (json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8")
    try:
        return _raw_send_bytes(payload, timeout)
    except Exception:
        pass
    spawned = not _engine_alive()
    if not start_engine():
        raise RuntimeError("引擎启动失败（看 /tmp/yolo26_engine.stdout）")
    if spawned and req.get("cmd") == "infer":
        # 刚拉起的是空引擎：先按当前任务装模型再重放请求。
        # 不然引擎端兜底只会装 kTasks[0]（seg），pose/det 页签就永远丢帧
        _raw_send({"cmd": "load", "qid": 3, "task": TASK}, timeout=timeout)
        _load_state.update(state="ready", task=TASK)
    return _raw_send_bytes(payload, timeout)

def _preload():
    """后台加载模型（页面打开时触发，用户取景的功夫模型就加载好了）
    全程持锁：和 /api/task 切换串行化，避免两边交错把对方刚加载的模型顶掉"""
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
        _load_state["state"] = "loading"
        try:
            start_engine()
            resp = _raw_send({"cmd": "load", "qid": 1, "task": TASK}, timeout=300)
            if resp.get("ev") == "done":
                _load_state.update(state="ready", task=resp.get("task", TASK),
                                   load_ms=resp.get("load_ms", 0), err="")
            else:
                _load_state.update(state="error", err=resp.get("msg", "加载失败"))
        except Exception as e:
            _load_state.update(state="error", err=str(e))
        print("[yolo26] preload: %s" % _load_state, flush=True)

def ensure_preload_async():
    with _load_lock:
        if _load_state["state"] in ("loading", "ready"):
            return
    threading.Thread(target=_preload, daemon=True).start()

# ── NPU 生命周期管理（page-driven，与 tts/asr/depth 同款）────────────
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
    print(f"[yolo26] {reason}，服务退出，可重新运行 start.sh", flush=True)
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
        if st["state"] == "ready" and _engine_alive() and os.path.exists(YOLO_SOCK):
            try:
                r = _raw_send({"cmd": "ping"}, timeout=5)
                if r.get("loaded"):
                    st["state"] = "ready"
                    st["task"] = r.get("task", "")
            except Exception:
                pass
        self.send_json(st)

    def do_GET(self):
        touch()
        if self.path == "/api/status":
            self._status()
            return
        if self.path == "/api/beat":
            # 页面 5 秒一次的心跳：只要页面开着，300s 无活动兜底就不该触发
            self.send_json({"ok": 1})
            return
        if self.path.startswith("/api/img/"):
            # 测试钩子：取 model/ 下的测试图（?img= 金标准流程用），只放行图片后缀
            name = os.path.basename(self.path.split("?")[0])
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                self.send_json({"error": "只支持 jpg/png"}, code=400)
                return
            p = os.path.join(YOLO_MODEL_DIR, name)
            if not os.path.exists(p):
                self.send_json({"error": "图片不存在"}, code=404)
                return
            with open(p, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path.split("?")[0] in ("/", "/index.html"):
            path = os.path.join(os.path.dirname(__file__), "yolo26.html")
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
                self.send_json({"error": "yolo26.html 不存在"}, code=404)
        else:
            self.send_json({"error": "not found"}, code=404)

    def do_POST(self):
        touch()
        if self.path == "/api/status":
            self._status()

        elif self.path.split("?")[0] == "/api/capture":
            global _seq, _active
            # ?fmt=rgb|rgba&w=&h= → 裸内容带（摄像头实时环路，免 JPEG 编解码；
            # rgba 连浏览器端剥 alpha 都省了，引擎 C++ 端剥）
            raw_mode = False
            raw_w = raw_h = 0
            raw_bpp = 3
            if "?" in self.path:
                from urllib.parse import urlparse, parse_qs
                q = parse_qs(urlparse(self.path).query)
                fmt = q.get("fmt", [""])[0]
                if fmt in ("rgb", "rgba"):
                    raw_mode = True
                    raw_bpp = 4 if fmt == "rgba" else 3
                    raw_w = int(q.get("w", ["640"])[0])
                    raw_h = int(q.get("h", ["640"])[0])
            content_len = int(self.headers.get("Content-Length", 0))
            if content_len < 1024:
                self.send_json({"error": "图片数据缺失"}, code=400)
                return
            if content_len > 15 * 1024 * 1024:
                self.send_json({"error": "图片过大（上限 15MB）"}, code=400)
                return
            t_read0 = time.time()
            data = self.rfile.read(content_len)
            t_read1 = time.time()
            if not raw_mode and data[:2] != b"\xff\xd8":
                # JPEG SOI 校验（浏览器 canvas.toBlob('image/jpeg')）
                self.send_json({"error": "不是 JPEG 图片"}, code=400)
                return
            if raw_mode and content_len != raw_w * raw_h * raw_bpp:
                self.send_json({"error": "raw 尺寸不符"}, code=400)
                return

            with _active_lock:
                if _active > 0:
                    # 实时流场景：上一帧还在算就丢帧（页面单飞也不会到这）
                    self.send_json({"drop": 1}, code=200)
                    return
                _active += 1
            _seq += 1
            jid = _seq
            in_path = f"{TMPDIR}/yolo26_in_{jid}.{'raw' if raw_mode else 'jpg'}"
            with open(in_path, "wb") as f:
                f.write(data)
            t_file1 = time.time()
            req = {"cmd": "infer", "qid": jid, "img": in_path}
            if raw_mode:
                req.update({"fmt": "rgba" if raw_bpp == 4 else "rgb",
                            "w": raw_w, "h": raw_h})
            try:
                raw_resp = sock_send_raw(req, timeout=300)
            except Exception as e:
                with _active_lock:
                    _active -= 1
                self.send_json({"error": f"引擎通信失败: {e}"}, code=500)
                return
            t_eng1 = time.time()
            with _active_lock:
                _active -= 1

            if not raw_resp.startswith(b'{"ev":"done"'):
                try:
                    resp = json.loads(raw_resp.decode("utf-8"))
                    msg = resp.get("msg", "推理失败")
                except Exception:
                    msg = "引擎响应异常"
                self.send_json({"error": msg}, code=500)
                return
            # done 响应原样透传（JSON 由浏览器解析）
            self.send_json_bytes(raw_resp)
            try:
                os.unlink(in_path)
            except OSError:
                pass
            # 逐段耗时日志（性能定位用）：收包 / 写盘 / 引擎往返 / 回发
            t_send1 = time.time()
            p = raw_resp.find(b'"total":')
            eng_total = raw_resp[p + 8:].split(b',')[0].split(b'}')[0].decode() \
                if p >= 0 else "?"
            with open("/tmp/yolo26_http.log", "a") as lf:
                lf.write("recv=%.1f file=%.1f eng=%.1f send=%.1f eng_total=%s\n" % (
                    (t_read1 - t_read0) * 1000, (t_file1 - t_read1) * 1000,
                    (t_eng1 - t_file1) * 1000, (t_send1 - t_eng1) * 1000,
                    eng_total))

        elif self.path == "/api/task":
            # 切换任务：引擎 load 换模型（约 0.3~0.6s），同任务重复切换是幂等 no-op
            global TASK
            ln = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(ln) or b"{}")
            except Exception:
                body = {}
            t = body.get("task", "")
            if t not in TASKS:
                self.send_json({"error": f"未知任务 {t}（可选：{'/'.join(TASKS)}）"},
                               code=400)
                return
            try:
                # 和 _preload 同一把锁串行：切换期间不许再冒出默认任务的加载
                with _load_lock:
                    resp = sock_send({"cmd": "load", "qid": 2, "task": t}, timeout=300)
                    if resp.get("ev") != "done":
                        self.send_json({"error": resp.get("msg", "切换失败")}, code=500)
                        return
                    TASK = t
                    _load_state.update(state="ready", task=t,
                                       load_ms=resp.get("load_ms", 0), err="")
            except Exception as e:
                self.send_json({"error": f"引擎通信失败: {e}"}, code=500)
                return
            print(f"[yolo26] 任务切换 → {t}（{resp.get('load_ms', 0):.0f}ms）", flush=True)
            self.send_json({"task": t, "load_ms": resp.get("load_ms", 0)})

        elif self.path == "/api/bye":
            # 迟到信标防护：3 秒内有页面活动（刷新后新页面已开跑）就不杀引擎，
            # 否则 F5 / 换窗口会把刚加载好的模型误杀、下个请求又从头加载
            if time.time() - LAST_SEEN[0] > 3:
                stop_engine()
                _load_state.update(state="idle")
            BYE_AT[0] = time.time()
            self.send_json({"status": "released"})

        else:
            self.send_json({"error": "not found"}, code=404)

    def send_json(self, data, code=200):
        self.send_json_bytes(json.dumps(data, ensure_ascii=False).encode("utf-8"),
                             code)

    def send_json_bytes(self, body, code=200):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    print(f"[yolo26] listening :{PORT}  model_dir={YOLO_MODEL_DIR} task={TASK}", flush=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    SERVER = server

    def _open_browser():
        time.sleep(2)
        if os.environ.get("YOLO26_OPEN_BROWSER") != "1":
            return  # 默认不开（后台维护/测试时避免乱弹窗口，页面还会自动开摄像头）
        try:
            env = dict(os.environ)
            env.setdefault("DISPLAY", ":0")
            subprocess.Popen(
                ["chromium", "--no-sandbox", "--disable-gpu", "--no-first-run",
                 "--new-window", f"http://127.0.0.1:{PORT}"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            print("[yolo26] 已打开浏览器 http://127.0.0.1:%d" % PORT, flush=True)
        except Exception as e:
            print("[yolo26] 自动打开浏览器失败: %s" % e, flush=True)

    threading.Thread(target=_open_browser, daemon=True).start()
    server.serve_forever()
    print("[yolo26] 已退出", flush=True)
