#!/bin/sh
# 用法: sh start.sh [mock|real|stop]     默认 real（Qwen2.5-VL 真推理）
# 生命周期页面驱动（2026-09-15）：本脚本只起 web + watch 守护，
# 引擎由 watch.sh 按页面活动拉起/释放；启动时不动其他 demo——
# NPU 被占会在页面报错提示（所有 demo 的通用规则）。
DIR=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-real}
# 停本 demo 旧进程（模式加括号防止 pkill 匹配到自身——历史踩坑）
pkill -f "[v]l_demo/vl_engine" 2>/dev/null
pkill -f "[v]l_demo/web/web.py" 2>/dev/null
pkill -f "[v]l_demo/watch.sh" 2>/dev/null
sleep 1
if [ "$1" = "stop" ]; then echo STOPPED; exit 0; fi

# ── 依赖探测：缺什么列什么，缺则退出 ─────────────────────────────
check_deps() {
  missing=0
  echo "[依赖] 检查运行环境…"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "  缺 python3"; missing=1
  fi
  if [ ! -x "$DIR/vl_engine" ]; then
    echo "  缺 vl_engine"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[依赖] 补齐上面缺的组件后重跑"
    return 1
  fi
  echo "[依赖] 就绪"
  return 0
}

check_deps || exit 1

nohup python3 "$DIR/web/web.py" > /tmp/vl_web.log 2>&1 &
sleep 1
setsid nohup sh "$DIR/watch.sh" $MODE > /tmp/vl_watch.log 2>&1 < /dev/null &
sleep 2
echo "--- web log ---"; tail -3 /tmp/vl_web.log
echo "浏览器打开 http://$(hostname -I | awk "{print $1}"):8080（页面打开后引擎自动加载约 20 秒）"
