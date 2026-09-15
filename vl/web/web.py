#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VL demo Web 层：标准库 only。
转发引擎事件：/stream 预览（MJPEG）、/ask 流式回答、/reset、/status。
与引擎的协议见设计文档第 4 节（Unix socket JSON 行）。
"""
import base64
import json
import os
import queue
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOCK = "/tmp/vl_engine.sock"
PORT = 8080
HERE = os.path.dirname(os.path.abspath(__file__))
LAST = "/tmp/vl_web.last"     # 页面心跳文件：watch.sh 靠它决定引擎拉起/释放
WATCH_MSG = "/tmp/vl_watch.msg"   # watch.sh 拒启原因（NPU 被占等），透出到页面


def touch_last() -> None:
    """任何页面请求（含 MJPEG 流帧）都算页面活着"""
    try:
        os.utime(LAST, None)
    except OSError:
        open(LAST, "w").close()

# ---- 引擎连接与事件分发 ----------------------------------------------------
engine_alive = False        # socket 是否连着
engine_status = ""          # 最近一条全局状态（摄像头上下线等）
status_lock = threading.Lock()
frame_subs = []             # /stream 订阅者（每人一个最新帧槽）
subs_lock = threading.Lock()
qid_queues = {}             # qid -> Queue（/ask 的等待者）
qid_lock = threading.Lock()
next_qid = [1]
send_lock = threading.Lock()
sock_obj = [None]           # 当前引擎连接


def engine_send(line: str) -> bool:
    s = sock_obj[0]
    if s is None:
        return False
    try:
        with send_lock:
            s.sendall((line + "\n").encode("utf-8"))
        return True
    except OSError:
        return False


def push_frame(jpg_bytes: bytes) -> None:
    with subs_lock:
        for q in frame_subs:
            try:
                q.get_nowait()      # 丢旧，只保最新
            except queue.Empty:
                pass
            q.put(jpg_bytes)


def dispatch(line: str) -> None:
    global engine_status
    try:
        ev = json.loads(line)
    except ValueError:
        return
    kind = ev.get("ev")
    if kind == "frame":
        b64 = ev.get("jpeg", "")
        try:
            push_frame(base64.b64decode(b64))
        except Exception:
            pass
        return
    qid = ev.get("qid", 0)
    if kind in ("token", "done", "error", "notice"):
        if qid:
            with qid_lock:
                q = qid_queues.get(qid)
            if q:
                q.put(ev)
        elif kind == "error":   # 全局事件（摄像头/上下文提示）
            with status_lock:
                engine_status = "%s（%s）" % (ev.get("msg", ""), time.strftime("%H:%M:%S"))


def reader_loop() -> None:
    global engine_alive
    buf = b""
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK)
            sock_obj[0] = s
            engine_alive = True
            print("[web] 引擎已连接", flush=True)
            buf = b""
            while True:
                data = s.recv(1 << 20)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.strip():
                        dispatch(line.decode("utf-8", "replace"))
        except OSError:
            pass
        finally:
            engine_alive = False
            sock_obj[0] = None
            try:
                s.close()
            except Exception:
                pass
        with status_lock:
            engine_status = "引擎离线，重连中…（%s）" % time.strftime("%H:%M:%S")
        time.sleep(2)


# ---- HTTP -----------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 安静点
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        touch_last()
        if self.path == "/" or self.path.startswith("/index"):
            with open(os.path.join(HERE, "page.html"), "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        elif self.path == "/status":
            with status_lock:
                st = engine_status
            # 引擎不在时若 watch 拒启（NPU 被其他 demo 占用），把原因透给页面
            if not engine_alive and os.path.exists(WATCH_MSG):
                try:
                    st = open(WATCH_MSG).read().strip()
                except OSError:
                    pass
            body = json.dumps({"engine": engine_alive, "status": st}).encode("utf-8")
            self._send(200, body, "application/json; charset=utf-8")
        elif self.path == "/stream":
            self.stream_mjpeg()
        else:
            self._send(404, b"not found", "text/plain")

    def stream_mjpeg(self) -> None:
        q = queue.Queue(maxsize=1)
        with subs_lock:
            frame_subs.append(q)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            last = b""
            idle = 0
            while True:
                touch_last()   # 流还连着 = 页面还开着
                try:
                    jpg = q.get(timeout=1.0)
                    idle = 0
                except queue.Empty:
                    idle += 1
                    if idle > 120:   # 2 分钟没帧（引擎挂了且没订阅价值）就收摊
                        break
                    continue
                if jpg == last:     # 同帧不重发
                    continue
                last = jpg
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                 b"Content-Length: " + str(len(jpg)).encode() +
                                 b"\r\n\r\n" + jpg + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with subs_lock:
                try:
                    frame_subs.remove(q)
                except ValueError:
                    pass

    def do_POST(self) -> None:
        touch_last()
        if self.path == "/ask":
            self.do_ask()
        elif self.path == "/reset":
            ok = engine_send('{"cmd":"reset"}')
            self._send(200 if ok else 503,
                       ("" if ok else "引擎离线").encode("utf-8"), "text/plain; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")

    def do_ask(self) -> None:
        length = int(self.headers.get("Content-Length", 0))
        question = self.rfile.read(length).decode("utf-8", "replace").strip()
        if not question:
            self._send(400, "空问题".encode("utf-8"), "text/plain; charset=utf-8")
            return
        with qid_lock:
            qid = next_qid[0]
            next_qid[0] += 1
            q = queue.Queue()
            qid_queues[qid] = q
        if not engine_send(json.dumps({"cmd": "ask", "qid": qid, "text": question},
                                      ensure_ascii=False)):
            with qid_lock:
                qid_queues.pop(qid, None)
            self._send(503, "引擎离线".encode("utf-8"), "text/plain; charset=utf-8")
            return
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            while True:
                ev = q.get()
                kind = ev.get("ev")
                if kind == "token":
                    self.wfile.write(ev.get("text", "").encode("utf-8"))
                    self.wfile.flush()
                elif kind == "notice":  # 系统提示（上下文自动清空等），前端渲染成提示条
                    self.wfile.write(("\n@@NOTICE " + ev.get("text", "") + "\n").encode("utf-8"))
                    self.wfile.flush()
                elif kind == "done":
                    stats = json.dumps(ev.get("stats", {}), ensure_ascii=False)
                    self.wfile.write(("\n@@STATS " + stats).encode("utf-8"))
                    self.wfile.flush()
                    return
                elif kind == "error":
                    self.wfile.write(("\n@@ERROR " + ev.get("msg", "")).encode("utf-8"))
                    self.wfile.flush()
                    return
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with qid_lock:
                qid_queues.pop(qid, None)


def main() -> None:
    touch_last()   # 确保 /tmp/vl_web.last 存在（watch.sh 依赖）
    threading.Thread(target=reader_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("[web] http://0.0.0.0:%d" % PORT, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
