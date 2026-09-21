#!/usr/bin/env python3
"""带 Range 支持的静态文件服务器。

python -m http.server 不支持 Range 请求，板上 deploy.sh 的 curl -C - 断点续传会报
"HTTP server doesn't seem to support byte ranges. Cannot resume"。
用法与其一致：
    python range_server.py 8000 --directory release_mirror
"""
import argparse
import os
import re
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


# 自适应限速器：全局一份，跨连接共享。
# 块大小固定 256KB（块大才是 yt6801 掉链根因，见其 8KB RX FIFO），只自适应调节每块后的 sleep：
#   整文件传完 → 加速 15%；写 socket 掉链（网卡 RST）→ 减速一半。
# 夹在 [4ms≈64MB/s, 60ms≈4MB/s]，起始 20MB/s（历史上证安全）。
class _AdaptivePacer:
    def __init__(self):
        self._lock = threading.Lock()
        self._sleep = 0.012
        self._min = 0.004
        self._max = 0.060

    def sleep(self):
        with self._lock:
            return self._sleep

    def speedup(self):
        with self._lock:
            self._sleep = max(self._sleep * 0.85, self._min)

    def slowdown(self):
        with self._lock:
            self._sleep = min(self._sleep * 2.0, self._max)


PACER = _AdaptivePacer()


class RangeHandler(SimpleHTTPRequestHandler):
    """在 SimpleHTTPRequestHandler 上补单区间 Range（bytes=N- / N-M / -N）。"""

    protocol_version = 'HTTP/1.1'    # Content-Length 两条路径都必有，可开长连接

    def copyfile(self, src, dst):
        # 自适应限速（见 _AdaptivePacer）：256KB 小块平滑输出，每块后 sleep 由 PACER 决定。
        # 每 16MB 无掉链 → 加一档速（大文件内部也会爬坡）；写 socket 掉链（yt6801 网卡 RST）→ 减速一半。
        total = 0
        clean_chunks = 0
        try:
            while True:
                buf = src.read(1 << 18)
                if not buf:
                    break
                dst.write(buf)
                total += len(buf)
                if total >= (1 << 18):
                    time.sleep(PACER.sleep())
                    total = 0
                    clean_chunks += 1
                    if clean_chunks >= 64:   # 16MB 无掉链，往上爬一档
                        PACER.speedup()
                        clean_chunks = 0
        except (ConnectionResetError, BrokenPipeError, OSError):
            PACER.slowdown()
            raise
        else:
            PACER.speedup()

    def end_headers(self):
        self.send_header('Accept-Ranges', 'bytes')
        super().end_headers()

    def send_head(self):
        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.exists(path):
            return super().send_head()          # 目录列表 / 404 走原逻辑

        size = os.path.getsize(path)
        rng = self.headers.get('Range')
        m = re.match(r'^bytes=(\d*)-(\d*)$', rng.strip()) if rng else None
        if not m:
            return super().send_head()          # 非 Range 或多区间（curl 不会发）→ 全量

        start_s, end_s = m.group(1), m.group(2)
        if start_s == '':                       # bytes=-N：最后 N 字节
            start, end = max(0, size - int(end_s)), size - 1
        else:
            start = int(start_s)
            end = min(int(end_s), size - 1) if end_s else size - 1
        if start > end or start >= size:
            self.send_error(416, 'requested range not satisfiable')
            return None

        f = open(path, 'rb')
        f.seek(start)
        self.send_response(206)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.send_header('Content-Length', str(end - start + 1))
        self.send_header('Last-Modified',
                         self.date_time_string(os.path.getmtime(path)))
        self.end_headers()
        return f


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?', type=int, default=8000)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--directory', default=os.getcwd())
    args = ap.parse_args()

    handler = lambda *a, **kw: RangeHandler(*a, directory=args.directory, **kw)
    srv = ThreadingHTTPServer((args.bind, args.port), handler)
    print('range_server on %s:%d serving %s' % (args.bind, args.port, args.directory), flush=True)
    srv.serve_forever()
