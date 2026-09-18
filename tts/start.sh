#!/bin/sh
# TTS Demo 启动脚本（在板子上跑）
# 用法: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-/userdata/models/qwen3-tts}"
TTS_BIN="${DEMO_DIR}/tts_engine"

# ── 依赖探测：缺什么列什么，缺则退出 ─────────────────────────────
check_deps() {
  missing=0
  echo "[依赖] 检查运行环境…"

  # 运行必需（不管有没有编译都查）
  if ! command -v python3 >/dev/null 2>&1; then
    echo "  缺 python3"; missing=1
  fi
  if [ ! -e /usr/lib/librknn3_api.so ] && ! ldconfig -p 2>/dev/null | grep -qE 'librknn3_api\.so '; then
    echo "  缺 librknn3_api.so"; missing=1
  fi

  # 编译必需（tts_engine 还没编译出来时才查）
  if [ ! -x "$TTS_BIN" ]; then
    if ! command -v g++ >/dev/null 2>&1; then
      echo "  缺 g++"; missing=1
    fi
    if ! command -v make >/dev/null 2>&1; then
      echo "  缺 make"; missing=1
    fi
    if ! command -v cmake >/dev/null 2>&1; then
      echo "  缺 cmake"; missing=1
    fi
    if [ ! -f /root/chat_demo/include/rknn3_api.h ]; then
      echo "  缺 rknn3_api.h"; missing=1
    fi
  fi

  if [ "$missing" = 1 ]; then
    echo "[依赖] 补齐上面缺的组件后重跑"
    return 1
  fi
  echo "[依赖] 就绪"
  return 0
}

check_deps || exit 1

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
  mkdir -p build
  cd build
  cmake ..
  make -j$(nproc)
  cd ..
  mv build/tts_engine .
  echo "[tts] 编译完成"
fi

check_npu

# ── 启动 Python Web 服务 ─────────────────────────────────────────────
export TTS_MODEL_DIR="$MODEL_DIR"
export TTS_BIN="$TTS_BIN"
export TTS_SOCK="/tmp/tts_engine.sock"

echo "[tts] 启动 Web 服务 :8088"
python3 tts_engine.py
