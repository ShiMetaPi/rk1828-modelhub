#!/bin/sh
# 用法: sh start.sh [stop]
# 生命周期页面驱动（2026-09-15）：运行后自动打开浏览器，页面打开自动拉起
# rkllm3-server，关掉页面自动释放 NPU 并退出服务（重新运行 start.sh 即可）。
# 启动时不动其他 demo——NPU 被占会在页面报错提示。
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

# 前台跑：浏览器自动打开（约 2 秒后），页面关闭后服务自动退出，
# 本脚本随之返回，重新运行 start.sh 即可（幂等，不会 address already in use）
python3 "$DIR/chat_web.py"
# 兜底清场（正常路径 bye 退出前已释放模型）
pkill -f "rkllm3-serv[e]r" 2>/dev/null
echo "[chat] 已退出，可重新运行 start.sh"
