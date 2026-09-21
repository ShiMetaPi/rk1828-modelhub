#!/bin/sh
# 用法: sh start.sh [mock|real|stop]     默认 real（Qwen2.5-VL 真推理）
# 生命周期页面驱动（2026-09-15）：运行后自动打开浏览器，引擎由 watch.sh
# 按页面活动拉起/释放；页面关闭后整个服务退出（重新运行 start.sh 即可）。
# 启动时不动其他 demo——NPU 被占会在页面报错提示（所有 demo 的通用规则）。
DIR=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-real}
# 停本 demo 旧进程（模式加括号防止 pkill 匹配到自身——历史踩坑）
pkill -f "[v]l_engine" 2>/dev/null
pkill -f "[v]l/web/web.py" 2>/dev/null
pkill -f "[v]l/watch.sh" 2>/dev/null
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

# watch 守护后台跑（引擎拉起/释放）；web 前台跑——页面关闭后 web 自动退出，
# 本脚本随之清场返回，重新运行 start.sh 即可（幂等，不会 address already in use）
setsid nohup sh "$DIR/watch.sh" $MODE > /tmp/vl_watch.log 2>&1 < /dev/null &
python3 "$DIR/web/web.py"
# 兜底清场（正常路径 web.py 退出前已自行杀掉 watch/引擎）
pkill -f "[v]l/watch.sh" 2>/dev/null
pkill -f "[v]l_engine" 2>/dev/null
echo "[vl] 已退出，可重新运行 start.sh"
