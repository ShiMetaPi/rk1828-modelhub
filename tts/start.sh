#!/bin/sh
# TTS Demo 启动脚本（在板子上跑）
# 用法: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-/root/Qwen3_TTS_deploy}"
TTS_BIN="${DEMO_DIR}/tts_engine"

# ── NPU 可用性检查 ──────────────────────────────────────────────────
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
  TOTAL=5120
  if [ -n "$USED" ]; then
    # 去掉 MB/GB 后缀
    USED_NUM=$(echo "$USED" | sed 's/MB$//; s/GB$//' | tr -d ' ')
    if echo "$USED" | grep -q GB; then
      USED_NUM=$(echo "$USED_NUM * 1000" | bc)
    fi
    if [ "$USED_NUM" -gt 200 ]; then
      echo "[错误] NPU 被占用 (${USED})，请关闭其他 demo 页面后重试"
      exit 1
    fi
  fi
}

# ── 编译 C++（如未编译） ───────────────────────────────────────────
if [ ! -x "$TTS_BIN" ]; then
  echo "[tts] 首次运行，正在编译…"
  SDK_ROOT="/root/vl_demo/engine"
  if [ ! -d "$SDK_ROOT/sdk" ]; then
    echo "[错误] 找不到 vl_demo SDK，请先部署 vl_demo"
    exit 1
  fi
  mkdir -p build
  cd build
  cmake -DSDK_ROOT="$SDK_ROOT" ..
  make -j$(nproc)
  cd ..
  mv build/tts_engine .
  echo "[tts] 编译完成"
fi

# ── 幂等：已有实例在跑则先停掉（重复运行不再报 address already in use） ──
if pgrep -f '^python3 tts_engine.py$' >/dev/null 2>&1 || pgrep -x tts_engine >/dev/null 2>&1; then
  echo "[tts] 检测到旧实例，先停止…"
  pkill -f '^python3 tts_engine.py$' 2>/dev/null
  pkill -x tts_engine 2>/dev/null
  sleep 2
  rm -f /tmp/tts_engine.sock
fi

check_npu

# ── 启动 Python Web 服务 ─────────────────────────────────────────────
export TTS_MODEL_DIR="$MODEL_DIR"
export TTS_BIN="$TTS_BIN"
export TTS_SOCK="/tmp/tts_engine.sock"

echo "[tts] 启动 Web 服务 :8088"
python3 tts_engine.py
