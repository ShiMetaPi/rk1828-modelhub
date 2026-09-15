#!/usr/bin/env python3
"""MiniCPM5-2B 板上 Web 聊天 —— rkllm3-server 的本地 UI（对齐 vl_demo 架构）

板上运行: sh start.sh            （或 python3 chat_web.py）
浏览器:   http://<板子IP>:8089

生命周期（页面驱动，2026-09-15 定策）:
  - 页面开着 = 服务在跑: 页面 3 秒一次的 /api/status 轮询即心跳，
    首个心跳自动拉起 rkllm3-server（15~30 秒）
  - 页面关掉 = 释放 NPU: pagehide → sendBeacon('/api/bye') 立即释放；
    断电/崩溃兜底——心跳消失 75 秒后自动杀服务、清对话历史
  - 启动前 NPU 已被占（其他 demo 在跑）→ 报错拒绝启动，绝不硬闯
    （硬闯会把 NPU 驱动挂死到只能重启板子）。本策略是所有 demo 的通用规则

其余: 上下文满了自动裁剪 + 文字摘要续命 + 聊天框提示; 板端 SSE →
JSONL 行协议流式转发 {"ev":"t|notice|stats|error|done", ...}。

实测依据（2026-09-14）:
  - 模型上下文上限 2048（服务默认 4096 被自动压到 2048，启动日志可见）
  - stream:true SSE 可用（一块≈1 token）；无 /tokenize 端点 → 按字符估
"""

import json
import os
import re
import subprocess
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---- 配置 ----
W4_DIR = "/root/rknn_MiniCPM5_2B_demo/model"
W8_DIR = "/root/w8a16"
VOCAB = W4_DIR + "/MiniCPM5-2B.tokenizer.gguf"   # 词表/embed 与量化无关，两版共用
EMBED = W4_DIR + "/MiniCPM5-2B.embed.bin"
CHAT_TEMPLATE = "/root/rknn_MiniCPM5_2B_demo/minicpm5.jinja"

MODEL_PORT = 8081   # rkllm3-server
WEB_PORT = 8089     # 本页面

MAX_CTX = int(os.environ.get("CHAT_WEB_MAXCTX", 2048))  # 测试自动清理可调小
RESERVE = 512       # 给回答留的余量（真超了只会截断回答，不致命）
MARGIN = 64         # 估算误差缓冲
RECAP_TURNS = 3     # 摘要收纳最近几轮被裁掉的旧对话
RECAP_Q, RECAP_A = 60, 120   # 摘要里问/答截断（字符）
KEEP_TURNS = 2      # 裁剪后保留最近几轮原文
MAX_TOKENS = 1024   # 单条回答上限
TEMPERATURE = 0.7
REPEAT_PENALTY = 1.1   # 不加会复读机循环（实测）

GRACE_SECS = 75     # 心跳消失多久算"页面已关"（覆盖后台标签页限速到 1 次/分钟）
NPU_CAP_MB = 5120   # NPU 设备内存池（rknn-smi）
NPU_BUSY_MB = 200   # 已用量超过这个数就算被其他 demo 占用

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 绕系统代理

# ---- 全局状态 ----
state = {
    "phase": "idle",   # idle（页面没开，未占 NPU） | boot | switching | ready | down
    "quant": None,     # 当前量化 w4 | w8
    "detail": "",
}
state_lock = threading.Lock()
gen_lock = threading.Lock()   # 板子单槽，一次只生成一条
ensure_lock = threading.Lock()   # 同时只允许一个拉起/释放流程
last_seen = [0.0]      # 最近一次页面心跳（monotonic）

history = []           # [{"role","content"}] 权威对话（不含摘要 system）
hist_tokens = 0        # 上一轮请求的 prompt+completion 估算值


def run(cmd, timeout=30):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          timeout=timeout)


def set_phase(phase, quant=None, detail=""):
    with state_lock:
        state["phase"] = phase
        if quant is not None:
            state["quant"] = quant
        state["detail"] = detail


def heartbeat():
    last_seen[0] = time.monotonic()


def running_quant():
    """:8081 在跑哪个量化版本；不通返回 None"""
    try:
        with OPENER.open("http://127.0.0.1:%d/v1/models" % MODEL_PORT, timeout=3) as r:
            data = json.loads(r.read())
        model_id = (data.get("data") or [{}])[0].get("id", "")
        return "w8" if "w8" in model_id.lower() else "w4"
    except Exception:
        return None


def npu_used_mb():
    """rknn-smi 读 NPU 已用内存（MB）；读不到返回 None"""
    out = run("rknn-smi info 2>/dev/null | grep -oE '[0-9]+ +/ +%d' | head -1"
              % NPU_CAP_MB).stdout
    m = re.search(r"(\d+)\s*/\s*%d" % NPU_CAP_MB, out)
    return int(m.group(1)) if m else None


def release(reason):
    """释放 NPU：杀 rkllm3-server、清对话历史，回到 idle 等下次页面打开"""
    with ensure_lock:
        run("pkill -f 'rkllm3-serv[e]r' || true")   # 括号防 pkill 匹配自身
        reset_history()
        set_phase("idle", detail=reason)


def ensure_server(quant):
    """页面打开后拉起 rkllm3-server（指定量化）。

    换量化 = 先释放自己的旧模型；NPU 占用检查放在释放之后——不然自己的
    旧模型会被误判成"其他 demo"。读数回收有延迟，最多等 30 秒回落。
    """
    with ensure_lock:
        cur = running_quant()
        if cur == quant:
            set_phase("ready", quant)
            return True
        if cur is not None:
            set_phase("switching", quant, "正在释放旧模型 %s …" % cur.upper())
            run("pkill -f 'rkllm3-serv[e]r' || true")
            time.sleep(2)
        # 通用规则：启动前 NPU 被其他 demo 占用 → 报错退出，绝不硬闯。
        # 读数回落最多等 30 秒（自己刚释放的残留/对方正在释放都算进来）
        set_phase("switching", quant, "等待 NPU 释放 …")
        t0 = time.time()
        while True:
            used = npu_used_mb()
            if used is None or used <= NPU_BUSY_MB:
                break
            if time.time() - t0 > 30:
                set_phase("down", quant,
                          "NPU 已被其他 demo 占用（%dMB/%dMB），先关掉它的页面释放后再打开本页"
                          % (used, NPU_CAP_MB))
                return False
            time.sleep(2)
        set_phase("switching", quant,
                  "正在加载 MiniCPM5-2B %s（15~30 秒）…" % quant.upper())
        run("pkill -f 'rkllm3-serv[e]r' || true")   # 保险：清掉不应答的僵尸实例
        time.sleep(1)
        d = W4_DIR if quant == "w4" else W8_DIR
        cmd = ("rkllm3-server"
               " --model %s/MiniCPM5-2B.rknn --weight %s/MiniCPM5-2B.weight"
               " --vocab %s --embed %s"
               " --chat-template-file %s"
               " --host 0.0.0.0 --port %d --alias minicpm5-%s"
               " > /tmp/rkllm3-server.log 2>&1" % (d, d, VOCAB, EMBED,
                                                    CHAT_TEMPLATE, MODEL_PORT, quant))
        subprocess.Popen(cmd, shell=True, start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        while time.time() - t0 < 120:
            time.sleep(2)
            if running_quant() == quant:
                set_phase("ready", quant)
                return True
            if run("pgrep -f 'rkllm3-serv[e]r' >/dev/null").returncode != 0:
                break
        log = run("tail -3 /tmp/rkllm3-server.log").stdout
        set_phase("down", quant, "rkllm3-server 拉不起来：%s" % log.strip())
        return False


def lifecycle_monitor():
    """兜底释放：页面没了（没发 beacon 也没心跳）75 秒后自动释放 NPU"""
    while True:
        time.sleep(5)
        with state_lock:
            phase = state["phase"]
        if phase == "ready" and time.monotonic() - last_seen[0] > GRACE_SECS:
            print("[chat_web] 页面心跳消失 %ds，释放模型" % GRACE_SECS, flush=True)
            release("页面已关闭，模型已释放；重新打开本页自动拉起")


# ---- 上下文管理 ------------------------------------------------------------

CJK_RE = re.compile(r"[一-鿿　-〿＀-￯]")


def est_tokens(text):
    """没有 /tokenize 端点，按字符估：中文≈1.6字/token，其他≈4字/token"""
    cjk = len(CJK_RE.findall(text))
    other = len(text) - cjk
    return int(cjk / 1.6 + other / 4) + 1


def history_tokens():
    n = 40  # chatml 模板开销（im_start/end、角色名、换行）
    for m in history:
        n += est_tokens(m["content"]) + 4
    return n


def build_recap(pairs):
    """被裁掉的旧轮 → 文字摘要（同 vl_demo BuildRecap：只收文字，截断防膨胀）"""
    parts = ["［此前对话文字摘要，供你延续语境，无需回应］"]
    for q, a in pairs[-RECAP_TURNS:]:
        parts.append("问:%s 答:%s。" % (q[:RECAP_Q], a[:RECAP_A]))
    return "".join(parts)


def trim_if_needed():
    """上下文要满了: 旧轮→摘要 system，保留最近 KEEP_TURNS 轮原文。返回提示文案或 None"""
    global history, hist_tokens
    threshold = MAX_CTX - RESERVE
    before = history_tokens()
    if before + MARGIN <= threshold:
        hist_tokens = before
        return None
    pairs = [(history[i]["content"], history[i + 1]["content"])
             for i in range(0, len(history) - 1, 2)]
    keep = KEEP_TURNS
    while True:
        old_pairs, keep_pairs = pairs[:-keep] or [], pairs[-keep:]
        recap = build_recap(old_pairs) if old_pairs else ""
        history = ([{"role": "system", "content": recap}] if recap else []) + \
                  [m for p in keep_pairs for m in
                   ({"role": "user", "content": p[0]},
                    {"role": "assistant", "content": p[1]})]
        after = history_tokens()
        if after + MARGIN <= threshold or keep == 1:
            break
        keep = 1   # 保留 2 轮仍超（单轮特别长）——只留最近 1 轮
    hist_tokens = after
    return "上下文已满，自动清理（约 %d→%d token）：旧对话已压缩成摘要，最近 %d 轮保留原文" \
        % (before, after, keep)


def reset_history():
    global history, hist_tokens
    history = []
    hist_tokens = 0


# ---- 推理（SSE → JSONL）-----------------------------------------------------

THINK_RE = re.compile(r"<think>.*?(?:</think>|$)", re.S)


class ThinkStripper:
    """流式去 <think>：空思考块（enable_thinking=false）整块在首个 delta 到达"""

    def __init__(self):
        self.pending = ""
        self.in_think = False

    def feed(self, piece):
        if self.in_think:
            self.pending += piece
            end = self.pending.find("</think>")
            if end < 0:
                return None
            rest = self.pending[end + 8:].lstrip("\n")
            self.pending, self.in_think = "", False
            return rest or None
        if self.pending == "" and piece.startswith("<think>"):
            self.in_think = True
            return self.feed(piece[len("<think>"):])
        return piece


def do_chat(handler, text):
    """处理一条问答：裁剪检查 → 板端 SSE → JSONL 流给页面"""
    if not gen_lock.acquire(blocking=False):
        handler.send_event({"ev": "error", "text": "上一条还没答完，稍等一下"})
        return
    try:
        with state_lock:
            phase, quant = state["phase"], state["quant"]
        if phase != "ready":
            handler.send_event({"ev": "error",
                                "text": "模型未就绪（%s），稍后再试" % phase})
            return

        notice = trim_if_needed()
        if notice:
            handler.send_event({"ev": "notice", "text": notice})

        history.append({"role": "user", "content": text})
        payload = json.dumps({
            "messages": history,
            "max_tokens": MAX_TOKENS,
            "temperature": TEMPERATURE,
            "repeat_penalty": REPEAT_PENALTY,
            "stream": True,
            # 模板支持 enable_thinking：False 时预置空 <think></think>，直接出答案
            # （默认关——实测开思考会啰嗦几百 token 甚至烧光 max_tokens 没正文）
            "chat_template_kwargs": {"enable_thinking": False},
        }).encode()
        req = urllib.request.Request(
            "http://127.0.0.1:%d/v1/chat/completions" % MODEL_PORT, data=payload,
            headers={"Content-Type": "application/json"})

        t0 = time.time()
        t_first = None
        answer = []
        n_tokens = 0
        stripper = ThinkStripper()
        try:
            with OPENER.open(req, timeout=300) as resp:
                for raw in resp:   # SSE 按行迭代
                    line = raw.decode("utf-8", "replace").strip()
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    try:
                        obj = json.loads(data)
                    except ValueError:
                        continue
                    delta = (obj.get("choices") or [{}])[0].get("delta") or {}
                    piece = delta.get("content")
                    if not piece:
                        continue
                    if t_first is None:
                        t_first = time.time()
                    n_tokens += 1  # 一块≈1 token
                    piece = stripper.feed(piece)
                    if piece:
                        answer.append(piece)
                        handler.send_event({"ev": "t", "text": piece})
        except Exception as e:
            history.pop()   # 失败的问题不入史
            handler.send_event({"ev": "error", "text": "推理失败：%s" % e})
            return

        full = "".join(answer).strip()
        if not full:
            history.pop()
            handler.send_event({"ev": "error", "text": "模型没有输出正文（可能思考被截断）"})
            return
        history.append({"role": "assistant", "content": full})

        t_end = time.time()
        prefill_ms = (t_first - t0) * 1000 if t_first else 0
        decode_s = max(0.01, t_end - (t_first or t0))
        handler.send_event({"ev": "stats",
                            "tokens": n_tokens,
                            "prefill_ms": int(prefill_ms),
                            "tps": round(n_tokens / decode_s, 1),
                            "ctx": history_tokens()})
    finally:
        handler.send_event({"ev": "done"})
        gen_lock.release()


# ---- HTTP --------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # 静默访问日志

    # -- 流式事件 --
    def start_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def send_event(self, obj):
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")
        self.wfile.flush()

    def end_stream(self):
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    # -- 路由 --
    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            with open(os.path.join(os.path.dirname(__file__), "chat.html"),
                      "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/status":
            heartbeat()   # 页面在看着我 = 生命周期信号
            with state_lock:
                st = dict(state)
            # 就绪状态也要防服务被外部杀掉
            if st["phase"] == "ready" and running_quant() is None:
                st["phase"] = "down"
                st["detail"] = "rkllm3-server 已停（被外部杀掉），重试拉起中…"
            # idle（页面刚打开）或 down（被拒启/被杀，比如占用的对方 demo 已关）
            # → 只要没有进行中的拉起流程，就（重）试拉起模型
            if st["phase"] in ("idle", "down") and ensure_lock.acquire(False):
                ensure_lock.release()
                quant = st["quant"] or "w4"
                st["detail"] = st["detail"] or \
                    "正在加载 MiniCPM5-2B %s（15~30 秒）…" % quant.upper()
                threading.Thread(target=ensure_server, args=(quant,),
                                 daemon=True).start()
            body = json.dumps(st, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""

        if self.path == "/api/chat":
            heartbeat()
            self.start_stream()
            try:
                do_chat(self, raw.decode("utf-8", "replace").strip())
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # 用户关页面/刷新，正常
            finally:
                try:
                    self.end_stream()
                except Exception:
                    pass

        elif self.path == "/api/bye":
            # pagehide beacon：正常关页立即释放 NPU（75 秒兜底管崩溃场景）
            heartbeat()
            threading.Thread(
                target=lambda: (time.sleep(0.5), release("页面已关闭，模型已释放；重新打开本页自动拉起")),
                daemon=True).start()
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/api/select":
            heartbeat()
            quant = raw.decode("utf-8", "replace").strip()
            if quant not in ("w4", "w8"):
                body = b'{"error":"quant must be w4|w8"}'
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            reset_history()   # 不同模型对话不连续
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            threading.Thread(target=ensure_server, args=(quant,), daemon=True).start()

        elif self.path == "/api/reset":
            heartbeat()
            reset_history()
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_error(404)


def main():
    last_seen[0] = 0.0   # 启动即无页面：不拉服务，等第一个心跳
    threading.Thread(target=lifecycle_monitor, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", WEB_PORT), Handler)
    print("[chat_web] http://0.0.0.0:%d（页面驱动：打开页面自动拉模型，关掉自动释放）"
          % WEB_PORT, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
