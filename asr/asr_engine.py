#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ASR Python 包装：调官方 rknn_qwen3_asr_demo_online 二进制转写音频，
页面通过 SSE 收流式字幕（commit 定稿 / unfix 未定稿 / final 全文）。

管线：浏览器录音或上传音频（浏览器解码重采样成 16kHz mono PCM16 WAV）
      → POST /api/transcribe → 官方二进制逐轮转写 → SSE 推事件
依赖：标准库 http.server（无 Flask）
"""
import json
import os
import queue
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── 配置 ──────────────────────────────────────────────────────────────
ASR_DIR  = os.environ.get("ASR_DIR", "/root/asr_demo")
ASR_BIN  = os.path.join(ASR_DIR, "rknn_qwen3_asr_demo_online")
MODEL    = os.path.join(ASR_DIR, "model")
PORT     = 8090
MAX_BODY = 40 * 1024 * 1024   # 40MB ≈ 17 分钟 16kHz PCM16

# ── 转写任务（同一时刻只允许一个，NPU 独占）──────────────────────────
_job = None                    # {"id", "phase", "round", "proc"}
_job_lock = threading.Lock()
_job_seq = [0]

# ── SSE 订阅者（每个连接一个队列）────────────────────────────────────
_sse_clients = []
_sse_lock = threading.Lock()

def emit(ev: dict):
    """向所有 SSE 订阅者推一个事件。"""
    data = json.dumps(ev, ensure_ascii=False)
    with _sse_lock:
        for q in list(_sse_clients):
            try:
                q.put_nowait(data)
            except queue.Full:
                pass

# ── NPU 占用检查（与其他 demo 互斥）─────────────────────────────────
def npu_busy_mb():
    """返回 NPU 已用显存 MB（解析失败返回 0）。"""
    try:
        r = subprocess.run(["rknn-smi"], capture_output=True, timeout=5)
        lines = r.stdout.decode(errors="replace").splitlines()
        used = lines[2].split()[4]          # 第 3 行第 5 列，如 123MB / 1.2GB
        num = float(used.replace("MB", "").replace("GB", ""))
        if "GB" in used:
            num *= 1000
        return num
    except Exception:
        return 0

# ── 转写线程 ─────────────────────────────────────────────────────────
def run_transcribe(jid, wav_path):
    global _job
    proc = None
    try:
        emit({"ev": "phase", "jid": jid, "phase": "loading",
              "msg": "模型加载中（首次约 1 分钟，之后约 15 秒）…"})
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = os.path.join(ASR_DIR, "lib")
        # stdbuf -oL：官方二进制是块缓冲，行缓冲才能逐轮流出
        proc = subprocess.Popen(
            ["stdbuf", "-oL", ASR_BIN,
             os.path.join(MODEL, "encoder_online.rknn"),
             os.path.join(MODEL, "encoder_online.weight"),
             os.path.join(MODEL, "llm.rknn"),
             os.path.join(MODEL, "llm.weight"),
             os.path.join(MODEL, "llm.tokenizer.gguf"),
             os.path.join(MODEL, "llm.embed.bin"),
             "0xFF", "0xFF", wav_path],
            cwd=ASR_DIR, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        with _job_lock:
            _job["proc"] = proc

        expect = None          # 下一行是 commit / unfix 的文本
        in_final = False
        final_lines = []
        for line in proc.stdout:
            line = line.rstrip("\n")
            if in_final:
                if line.strip():
                    final_lines.append(line.strip())
                continue
            if "Final Commit Result" in line:
                in_final = True
                with _job_lock:
                    _job["phase"] = "finalizing"
                continue
            m = re.match(r"=+ Online Round (\d+) =+", line.strip())
            if m:
                with _job_lock:
                    _job["phase"] = "transcribing"
                    _job["round"] = int(m.group(1))
                emit({"ev": "round", "jid": jid, "n": int(m.group(1))})
                continue
            if line.strip() == "commit_add_text:":
                expect = "commit"
                continue
            if line.strip() == "unfix_text:":
                expect = "unfix"
                continue
            if expect:
                text = line.strip()
                emit({"ev": expect, "jid": jid, "text": text})
                expect = None
                continue
            m = re.match(r"perf: audio=([\d.]+) ms.*ttft=([\d.]+) ms", line.strip())
            if m:
                emit({"ev": "perf", "jid": jid,
                      "audio_ms": float(m.group(1)), "ttft_ms": float(m.group(2))})
        rc = proc.wait()

        final_text = " ".join(final_lines).strip()
        emit({"ev": "final", "jid": jid, "text": final_text, "rc": rc})
        print(f"[asr] job {jid} 完成 rc={rc} final={len(final_text)} 字", flush=True)
    except Exception as e:
        emit({"ev": "error", "jid": jid, "msg": f"转写失败: {e}"})
        print(f"[asr] job {jid} 异常: {e}", flush=True)
    finally:
        if proc is not None and proc.poll() is None:
            try:
                proc.kill()
            except Exception:
                pass
        try:
            os.unlink(wav_path)
        except OSError:
            pass
        with _job_lock:
            _job = None

# ── 页面驱动生命周期（与 tts 同款）───────────────────────────────────
LAST_SEEN = [time.time()]
BYE_AT = [None]
SERVER = None
KEEPALIVE_INTERVAL = 5
BYE_EXIT_AFTER = 10           # 页面关闭后多久退出服务（宽限，防 F5 误杀）
SERVICE_EXIT_AFTER = 300      # 心跳彻底消失的兜底

def _exit_service(reason):
    print(f"[asr] {reason}，服务退出，可重新运行 start.sh", flush=True)
    with _job_lock:
        proc = _job.get("proc") if _job else None
    if proc is not None and proc.poll() is None:
        try:
            proc.kill()
        except Exception:
            pass
    if SERVER is not None:
        SERVER.shutdown()
    else:
        os._exit(0)

def _keepalive():
    while True:
        time.sleep(KEEPALIVE_INTERVAL)
        now = time.time()
        with _job_lock:
            job_active = _job is not None
        if job_active:
            continue   # 转写中不判空闲退出（长音频可能持续数分钟）
        if (BYE_AT[0] is not None and now - BYE_AT[0] > BYE_EXIT_AFTER) \
                or now - LAST_SEEN[0] > SERVICE_EXIT_AFTER:
            _exit_service("页面已关闭" if BYE_AT[0] is not None else "长时间无活动")
            return

threading.Thread(target=_keepalive, daemon=True).start()

def touch():
    LAST_SEEN[0] = time.time()
    BYE_AT[0] = None

# ── HTTP 接口 ────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        touch()
        if self.path == "/api/status":
            with _job_lock:
                st = dict(_job) if _job else None
            if st is None:
                self.send_json({"status": "idle"})
            else:
                self.send_json({"status": st.get("phase", "loading"),
                                "round": st.get("round", 0)})
            return
        if self.path == "/api/events":
            self.sse_stream()
            return
        if self.path in ("/", "/index.html", "/asr.html"):
            path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "asr.html")
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
                self.send_json({"error": "asr.html 不存在"}, code=404)
        else:
            self.send_json({"error": "not found"}, code=404)

    def do_POST(self):
        global _job
        touch()
        if self.path == "/api/transcribe":
            content_len = int(self.headers.get("Content-Length", 0))
            if content_len < 44 + 3200:
                self.send_json({"error": "音频太短（不足 0.1 秒）"}, code=400)
                return
            if content_len > MAX_BODY:
                self.send_json({"error": "音频过大（上限约 17 分钟）"}, code=400)
                return
            data = self.rfile.read(content_len)
            if data[:4] != b"RIFF":
                self.send_json({"error": "不是 WAV 文件"}, code=400)
                return

            with _job_lock:
                if _job is not None:
                    self.send_json({"error": "正在转写中，请等当前任务完成"}, code=409)
                    return
                _job_seq[0] += 1
                jid = _job_seq[0]
                _job = {"id": jid, "phase": "loading", "round": 0}

            used = npu_busy_mb()
            if used > 200:
                with _job_lock:
                    _job = None
                self.send_json({"error": f"NPU 被其他 demo 占用（{used:.0f}MB），请先关掉它"}, code=503)
                return

            wav_path = f"/tmp/asr_in_{jid}.wav"
            with open(wav_path, "wb") as f:
                f.write(data)
            threading.Thread(target=run_transcribe, args=(jid, wav_path), daemon=True).start()
            self.send_json({"ok": True, "jid": jid, "bytes": len(data)})
            return

        if self.path == "/api/bye":
            with _job_lock:
                proc = _job.get("proc") if _job else None
            if proc is not None and proc.poll() is None:
                try:
                    proc.kill()
                except Exception:
                    pass
                emit({"ev": "error", "msg": "页面关闭，转写已中止"})
            BYE_AT[0] = time.time()
            self.send_json({"status": "released"})
            return

        self.send_json({"error": "not found"}, code=404)

    def sse_stream(self):
        q = queue.Queue(maxsize=256)
        with _sse_lock:
            _sse_clients.append(q)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(b"retry: 2000\n\n")
            self.wfile.flush()
            while True:
                try:
                    data = q.get(timeout=15)
                    self.wfile.write(f"data: {data}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with _sse_lock:
                if q in _sse_clients:
                    _sse_clients.remove(q)

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
    if not os.path.exists(ASR_BIN):
        print(f"[asr] 找不到 {ASR_BIN}，先部署官方 demo 包", flush=True)
        raise SystemExit(1)
    print(f"[asr] listening :{PORT}  dir={ASR_DIR}", flush=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    SERVER = server

    def _open_browser():
        time.sleep(2)
        try:
            env = dict(os.environ)
            env.setdefault("DISPLAY", ":0")
            subprocess.Popen(
                ["chromium", "--no-sandbox", "--disable-gpu", "--no-first-run",
                 "--new-window", f"http://127.0.0.1:{PORT}"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            print("[asr] 已打开浏览器 http://127.0.0.1:%d" % PORT, flush=True)
        except Exception as e:
            print("[asr] 自动打开浏览器失败: %s" % e, flush=True)

    threading.Thread(target=_open_browser, daemon=True).start()
    server.serve_forever()
    print("[asr] 已退出", flush=True)
