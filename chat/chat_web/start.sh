#!/bin/sh
# 用法: sh start.sh [stop]
# 生命周期页面驱动（2026-09-15）：打开页面自动拉起 rkllm3-server，
# 关掉页面自动释放 NPU。启动时不动其他 demo——NPU 被占会在页面报错提示。
DIR=$(cd "$(dirname "$0")" && pwd)
pkill -f "[c]hat_web.py" 2>/dev/null   # 手动启动的进程 cmdline 无路径，按文件名匹配
sleep 1
if [ "$1" = "stop" ]; then
    # stop = 聊天整套下线（页面 + 模型服务，腾出 NPU）
    pkill -f "rkllm3-serv[e]r" 2>/dev/null
    echo STOPPED; exit 0
fi

# ── 依赖探测：缺什么列什么，缺则退出 ─────────────────────────────
check_deps() {
  missing=0
  echo "[依赖] 检查运行环境…"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "  缺 python3"; missing=1
  fi
  if ! command -v rkllm3-server >/dev/null 2>&1; then
    echo "  缺 rkllm3-server"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[依赖] 补齐上面缺的组件后重跑"
    return 1
  fi
  echo "[依赖] 就绪"
  return 0
}

check_deps || exit 1

setsid nohup python3 "$DIR/chat_web.py" > /tmp/chat_web.log 2>&1 < /dev/null &
sleep 2
echo "--- chat_web log ---"; tail -5 /tmp/chat_web.log
echo "浏览器打开 http://$(hostname -I | awk "{print $1}"):8089（页面打开后模型自动加载 15~30 秒）"
