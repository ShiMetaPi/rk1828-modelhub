#!/bin/sh
# TTS Demo start script (run on the board)
# Usage: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-$DEMO_DIR/model}"
TTS_BIN="${DEMO_DIR}/tts_engine"

# -- NPU availability check --
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
  TOTAL=5120
  if [ -n "$USED" ]; then
    # strip the MB/GB suffix
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

# -- compile C++ (if not already built) --
if [ ! -x "$TTS_BIN" ]; then
  echo "[tts] first run, compiling..."
  SDK_ROOT="${TTS_SDK_ROOT:-$DEMO_DIR/../vl/engine}"
  if [ ! -d "$SDK_ROOT/sdk" ]; then
    echo "[error] vl_demo SDK not found; deploy vl_demo first"
    exit 1
  fi
  mkdir -p build
  cd build
  cmake -DSDK_ROOT="$SDK_ROOT" ..
  make -j$(nproc)
  cd ..
  mv build/tts_engine .
  echo "[tts] compile done"
fi

# -- idempotent: stop any running instance first (re-run won't hit address-already-in-use) --
if pgrep -f '^python3 tts_engine.py$' >/dev/null 2>&1 || pgrep -x tts_engine >/dev/null 2>&1; then
  echo "[tts] old instance detected, stopping first..."
  pkill -f '^python3 tts_engine.py$' 2>/dev/null
  pkill -x tts_engine 2>/dev/null
  sleep 2
  rm -f /tmp/tts_engine.sock
fi

check_npu

# -- start Python web service --
export TTS_MODEL_DIR="$MODEL_DIR"
export TTS_BIN="$TTS_BIN"
export TTS_SOCK="/tmp/tts_engine.sock"

echo "[tts] starting web service :8088"
python3 tts_engine.py
