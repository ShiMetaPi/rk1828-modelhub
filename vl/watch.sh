#!/bin/sh
# 引擎页面驱动生命周期（所有 demo 通用规则，2026-09-15 定策）:
#   页面有活动（web.py 最近 10 秒内有请求）且引擎没跑 → 查 NPU，没被占才拉引擎
#   页面无活动超过 GRACE 秒且引擎在跑 → 杀引擎释放 NPU
#   启动前 NPU 被其他 demo 占用 → 绝不硬闯（会挂死驱动），原因写 /tmp/vl_watch.msg 由页面透出
DIR=$(cd "$(dirname "$0")" && pwd)
MODEL=/userdata/models/qwen2.5-vl-3b
MODE=${1:-real}
LAST=/tmp/vl_web.last
MSG=/tmp/vl_watch.msg
GRACE=75
BUSY_MB=200

rm -f "$MSG"
while true; do
  now=$(date +%s)
  last=$(stat -c %Y "$LAST" 2>/dev/null || echo 0)
  age=$((now - last))
  if pgrep -f "[v]l_engine" >/dev/null; then
    if [ $age -gt $GRACE ]; then
      echo "$(date '+%H:%M:%S') 页面已关闭，释放引擎" >> /tmp/vl_watch.log
      pkill -f "[v]l_engine"
      sleep 3
    fi
  elif [ $age -le 10 ]; then
    # 占用检查前先等读数回落（对方刚释放的残留，回收有延迟），最多 30 秒
    used=""
    i=0
    while [ $i -lt 6 ]; do
      used=$(rknn-smi info 2>/dev/null | grep -oE '[0-9]+ +/ +5120' | head -1 | grep -oE '^[0-9]+')
      [ -z "$used" ] && break
      [ "$used" -le $BUSY_MB ] && break
      i=$((i+1)); sleep 5
    done
    if [ -n "$used" ] && [ "$used" -gt $BUSY_MB ]; then
      echo "NPU 已被其他 demo 占用（${used}MB/5120MB），先关掉它的页面释放后再打开本页" > "$MSG"
      sleep 10
    else
      rm -f "$MSG"
      export LD_LIBRARY_PATH=$MODEL/lib:$LD_LIBRARY_PATH
      setsid nohup "$DIR/vl_engine" --$MODE --model $MODEL --sock /tmp/vl_engine.sock \
          > /tmp/vl_engine.log 2>&1 < /dev/null &
      sleep 8
    fi
  fi
  sleep 5
done
