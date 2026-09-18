#!/bin/sh
# ASR Demo 启动脚本（在板子上跑）
# 用法: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
export ASR_DIR="$DEMO_DIR"

# ── 依赖检查 ─────────────────────────────────────────────────────────
if [ ! -x "$DEMO_DIR/rknn_qwen3_asr_demo_online" ]; then
  echo "[错误] 找不到 rknn_qwen3_asr_demo_online（官方 demo 包二进制）"
  exit 1
fi
if [ ! -d "$DEMO_DIR/lib" ]; then
  echo "[错误] 找不到 lib/（librknn3_api.so 等）"
  exit 1
fi
if [ ! -f "$DEMO_DIR/mel_128_filters.txt" ]; then
  echo "[错误] 找不到 mel_128_filters.txt（必须在运行目录下）"
  exit 1
fi
if [ ! -f "$DEMO_DIR/model/llm.rknn" ]; then
  echo "[错误] 模型未就位：$DEMO_DIR/model/（运行 deploy.sh 拉取）"
  exit 1
fi

# ── NPU 可用性检查 ──────────────────────────────────────────────────
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
  TOTAL=5120
  if [ -n "$USED" ]; then
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

# ── 幂等：已有实例在跑则先停掉 ──────────────────────────────────────
if pgrep -f '^python3 asr_engine.py$' >/dev/null 2>&1; then
  echo "[asr] 检测到旧实例，先停止…"
  pkill -f '^python3 asr_engine.py$' 2>/dev/null
  sleep 2
fi
pkill -x rknn_qwen3_asr_demo_online 2>/dev/null || true
sleep 1

check_npu

# ── 启动 Python Web 服务 ─────────────────────────────────────────────
echo "[asr] 启动 Web 服务 :8090"
python3 asr_engine.py
