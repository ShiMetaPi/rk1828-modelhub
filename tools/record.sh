#!/bin/sh
# Board screen recorder (Rockchip MPP hardware H.264 + PulseAudio system sound)
# Near-zero CPU while recording.
# Usage:
#   sh record.sh start [name]   start recording -> /root/<name>_<mmdd_HHMMSS>.mp4
#   sh record.sh stop           stop (clean EOS, file playable immediately)
#   sh record.sh status         show recording state
# Requires: gstreamer1.0-rockchip (mpph264enc), pulseaudio (system audio capture)

export XDG_RUNTIME_DIR=/run/user/0
export DISPLAY=:0

PIDF=/tmp/record.pid

# Monitor source = "what the board is playing". Prefer the default sink's
# monitor (what you hear), fall back to any monitor source.
find_monitor() {
  SINK="$(pactl get-default-sink 2>/dev/null)"
  SRC="$(pactl list short sources 2>/dev/null | cut -f2 | grep -m1 "^${SINK}\.monitor\$")"
  [ -n "$SRC" ] || SRC="$(pactl list short sources 2>/dev/null | cut -f2 | grep -m1 '\.monitor')"
  echo "$SRC"
}

case "$1" in
start)
  if pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "already recording (run: record.sh stop first)"; exit 1
  fi
  NAME="${2:-rec}"
  OUT="/root/${NAME}_$(date +%m%d_%H%M%S).mp4"

  AUDIO=""
  MON="$(find_monitor)"
  if [ -n "$MON" ]; then
    AUDIO="pulsesrc device=$MON do-timestamp=true ! audio/x-raw,channels=2 ! audioresample ! audioconvert ! voaacenc bitrate=192000 ! aacparse ! queue ! mux."
    echo "audio: capturing system sound ($MON)"
  else
    echo "audio: no pulseaudio monitor found, recording video only"
  fi

  nohup gst-launch-1.0 -e \
    ximagesrc use-damage=false show-pointer=true \
    ! video/x-raw,framerate=30/1 \
    ! videoconvert ! mpph264enc bps=30000000 qp-max=28 ! h264parse ! queue ! mux. \
    ${AUDIO} \
    mp4mux name=mux ! filesink location="$OUT" \
    > /tmp/record.log 2>&1 &
  echo $! > $PIDF
  sleep 2
  if ! pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "start failed, see /tmp/record.log"; exit 1
  fi
  echo "recording -> $OUT"
  ;;
stop)
  pkill -INT -f "gst-launch.*filesink" && echo "stopped, file finalized"
  rm -f $PIDF
  ;;
status)
  if pgrep -f "gst-launch.*filesink" >/dev/null; then
    echo "recording: $(ls -t /root/*.mp4 2>/dev/null | head -1)"
  else
    echo "not recording"
  fi
  ;;
*)
  echo "usage: sh record.sh start [name] | stop | status"
  ;;
esac
