#!/bin/sh
# YOLO26 Demo start script (run on the board)
# Usage: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-$DEMO_DIR/model}"
YOLO_BIN="${DEMO_DIR}/yolo26_engine"

# -- dependency check: list what's missing --
missing=0
command -v python3 >/dev/null 2>&1 || { echo "[deps] missing python3"; missing=1; }
command -v rknn-smi >/dev/null 2>&1 || { echo "[deps] missing rknn-smi"; missing=1; }
for f in yolo26n-seg.rknn yolo26n-seg.weight; do
  [ -f "$MODEL_DIR/$f" ] || { echo "[deps] missing model $MODEL_DIR/$f"; missing=1; }
done
if [ "$missing" = 1 ]; then
  echo "[deps] install the missing components above and re-run"
  exit 1
fi

# -- compile C++ (if not already built) --
if [ ! -x "$YOLO_BIN" ]; then
  echo "[yolo26] first run, compiling..."
  mkdir -p build
  cd build
  cmake ..
  make -j$(nproc)
  cd ..
  mv build/yolo26_engine .
  echo "[yolo26] compile done"
fi

# -- NPU availability check --
check_npu() {
  # column 5 of the RK1828 row in rknn-smi info is "used / 5120" (unit MB)
  USED=$(rknn-smi info | awk -F'|' '/RK1828/ {gsub(/ /,"",$5); split($5,a,"/"); print a[1]}')
  if [ -n "$USED" ] && [ "$USED" -gt 200 ]; then
    echo "[error] NPU busy (${USED}MB); close the other demo's page and retry"
    exit 1
  fi
}

# -- idempotent: stop any running instance first --
if pgrep -f '^python3 yolo26_server.py$' >/dev/null 2>&1 || pgrep -x yolo26_engine >/dev/null 2>&1; then
  echo "[yolo26] old instance detected, stopping first..."
  pkill -f '^python3 yolo26_server.py$' 2>/dev/null || true
  pkill -x yolo26_engine 2>/dev/null || true
  sleep 2
  rm -f /tmp/yolo26_engine.sock
fi

check_npu

# -- start Python web service --
export YOLO_MODEL_DIR="$MODEL_DIR"
export YOLO_BIN="$YOLO_BIN"
export YOLO_SOCK="/tmp/yolo26_engine.sock"
export YOLO26_OPEN_BROWSER=1   # auto-open browser for normal use; running yolo26_server.py directly in background won't pop it

echo "[yolo26] starting web service :8092"
python3 yolo26_server.py
