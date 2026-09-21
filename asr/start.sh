#!/bin/sh
# ASR Demo start script (run on the board)
# Usage: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
export ASR_DIR="$DEMO_DIR"

# -- dependency check --
if [ ! -x "$DEMO_DIR/rknn_qwen3_asr_demo_online" ]; then
  echo "[error] rknn_qwen3_asr_demo_online not found (official demo package binary)"
  exit 1
fi
if [ ! -d "$DEMO_DIR/lib" ]; then
  echo "[error] lib/ not found (librknn3_api.so etc.)"
  exit 1
fi
if [ ! -f "$DEMO_DIR/mel_128_filters.txt" ]; then
  echo "[error] mel_128_filters.txt not found (must be in the run dir)"
  exit 1
fi
if [ ! -f "$DEMO_DIR/model/llm.rknn" ]; then
  echo "[error] models not in place: $DEMO_DIR/model/ (run deploy.sh to fetch)"
  exit 1
fi

# -- NPU availability check --
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
  TOTAL=5120
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
if pgrep -f '^python3 asr_engine.py$' >/dev/null 2>&1; then
  echo "[asr] old instance detected, stopping first..."
  pkill -f '^python3 asr_engine.py$' 2>/dev/null
  sleep 2
fi
pkill -x rknn_qwen3_asr_demo_online 2>/dev/null || true
sleep 1

check_npu

# -- start Python web service --
echo "[asr] starting web service :8090"
python3 asr_engine.py
