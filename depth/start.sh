#!/bin/sh
# Depth Demo start script (run on the board)
# Usage: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-$DEMO_DIR/model}"
DEPTH_BIN="${DEMO_DIR}/depth_engine"

# -- dependency check: list what's missing --
missing=0
command -v python3 >/dev/null 2>&1 || { echo "[deps] missing python3"; missing=1; }
command -v rknn-smi >/dev/null 2>&1 || { echo "[deps] missing rknn-smi"; missing=1; }
for f in da3_base_local.rknn da3_base_local.weight da3_base_global.rknn \
         da3_base_global.weight da3_base_head.rknn da3_base_head.weight; do
  [ -f "$MODEL_DIR/$f" ] || { echo "[deps] missing model $MODEL_DIR/$f (run deploy.sh to fetch)"; missing=1; }
done
if [ "$missing" = 1 ]; then
  echo "[deps] install the missing components above and re-run"
  exit 1
fi

# -- compile C++ (if not already built) --
if [ ! -x "$DEPTH_BIN" ]; then
  echo "[depth] first run, compiling..."
  mkdir -p build
  cd build
  cmake ..
  make -j$(nproc)
  cd ..
  mv build/depth_engine .
  echo "[depth] compile done"
fi

# -- NPU availability check --
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
  if [ -n "$USED" ]; then
    USED_NUM=$(echo "$USED" | sed 's/MB$//; s/GB$//' | tr -d ' ')
    if echo "$USED" | grep -q GB; then
      USED_NUM=$(echo "$USED_NUM * 1000" | bc)
    fi
    if [ "$USED_NUM" -gt 200 ]; then
      echo "[error] NPU busy (${USED}); close the other demo's page and retry"
      exit 1
    fi
  fi
}

# -- idempotent: stop any running instance first --
if pgrep -f '^python3 depth_engine.py$' >/dev/null 2>&1 || pgrep -x depth_engine >/dev/null 2>&1; then
  echo "[depth] old instance detected, stopping first..."
  pkill -f '^python3 depth_engine.py$' 2>/dev/null || true
  pkill -x depth_engine 2>/dev/null || true
  sleep 2
  rm -f /tmp/depth_engine.sock
fi

check_npu

# -- start Python web service --
export DEPTH_MODEL_DIR="$MODEL_DIR"
export DEPTH_BIN="$DEPTH_BIN"
export DEPTH_SOCK="/tmp/depth_engine.sock"
export DEPTH_OPEN_BROWSER=1   # auto-open browser for normal use; running depth_engine.py directly in background won't pop it

echo "[depth] starting web service :8091"
python3 depth_engine.py
