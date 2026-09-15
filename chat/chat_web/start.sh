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

PY=/userdata/mem0env/bin/python3   # mem0 venv（含长期记忆）；没装则回退系统 python3
[ -x "$PY" ] || PY=python3
setsid nohup "$PY" "$DIR/chat_web.py" > /tmp/chat_web.log 2>&1 < /dev/null &
sleep 2
echo "--- chat_web log ---"; tail -5 /tmp/chat_web.log
echo "浏览器打开 http://$(hostname -I | awk "{print $1}"):8089（页面打开后模型自动加载 15~30 秒）"
