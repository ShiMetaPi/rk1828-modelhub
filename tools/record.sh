#!/bin/sh
# 板端录屏（Rockchip MPP 硬件 H.264 编码，几乎不占 CPU，跑 demo 时录也不掉帧）
# 用法：
#   sh record.sh start [名字]   开始录制 → /root/名字_月日_时分秒.mkv（省略名字则 rec）
#   sh record.sh stop           停止（干净收尾，文件立即可播放）
#   sh record.sh status         是否在录

PIDF=/tmp/record.pid
case "$1" in
start)
  if pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "已经在录制中了（record.sh stop 先停）"; exit 1
  fi
  NAME="${2:-rec}"
  OUT="/root/${NAME}_$(date +%m%d_%H%M%S).mkv"
  DISPLAY=:0 nohup gst-launch-1.0 -e \
    ximagesrc use-damage=false show-pointer=true \
    ! video/x-raw,framerate=15/1 \
    ! videoconvert ! mpph264enc ! h264parse \
    ! matroskamux ! filesink location="$OUT" \
    > /tmp/record.log 2>&1 &
  echo $! > $PIDF
  sleep 2
  if ! pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "启动失败，看 /tmp/record.log"; exit 1
  fi
  echo "录制中 → $OUT"
  ;;
stop)
  pkill -INT -f "gst-launch.*filesink" && echo "已停止，文件收尾完成"
  rm -f $PIDF
  ;;
status)
  if pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "录制中：$(ls -t /root/*.mkv 2>/dev/null | head -1)"
  else
    echo "未在录制"
  fi
  ;;
*)
  echo "用法: sh record.sh start [名字] | stop | status"
  ;;
esac
