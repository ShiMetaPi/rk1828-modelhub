#!/bin/sh
# YOLO26 Demo 启动脚本（在板子上跑）
# 用法: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-/root/yolo26_demo/model}"
YOLO_BIN="${DEMO_DIR}/yolo26_engine"

# ── 依赖探测：缺什么列什么 ──────────────────────────────────────────
missing=0
command -v python3 >/dev/null 2>&1 || { echo "[依赖] 缺 python3"; missing=1; }
command -v rknn-smi >/dev/null 2>&1 || { echo "[依赖] 缺 rknn-smi"; missing=1; }
for f in yolo26n-seg.rknn yolo26n-seg.weight; do
  [ -f "$MODEL_DIR/$f" ] || { echo "[依赖] 缺模型 $MODEL_DIR/$f"; missing=1; }
done
if [ "$missing" = 1 ]; then
  echo "[依赖] 补齐上面缺的组件后重跑"
  exit 1
fi

# ── 编译 C++（如未编译）──────────────────────────────────────────────
if [ ! -x "$YOLO_BIN" ]; then
  echo "[yolo26] 首次运行，正在编译…"
  mkdir -p build
  cd build
  cmake ..
  make -j$(nproc)
  cd ..
  mv build/yolo26_engine .
  echo "[yolo26] 编译完成"
fi

# ── NPU 可用性检查 ──────────────────────────────────────────────────
check_npu() {
  # rknn-smi info 的 RK1828 行第 5 列是 "已用 / 5120"（单位 MB）
  USED=$(rknn-smi info | awk -F'|' '/RK1828/ {gsub(/ /,"",$5); split($5,a,"/"); print a[1]}')
  if [ -n "$USED" ] && [ "$USED" -gt 200 ]; then
    echo "[错误] NPU 被占用 (${USED}MB)，请关闭其他 demo 页面后重试"
    exit 1
  fi
}

# ── 幂等：已有实例在跑则先停掉 ──────────────────────────────────────
if pgrep -f '^python3 yolo26_server.py$' >/dev/null 2>&1 || pgrep -x yolo26_engine >/dev/null 2>&1; then
  echo "[yolo26] 检测到旧实例，先停止…"
  pkill -f '^python3 yolo26_server.py$' 2>/dev/null || true
  pkill -x yolo26_engine 2>/dev/null || true
  sleep 2
  rm -f /tmp/yolo26_engine.sock
fi

check_npu

# ── 启动 Python Web 服务 ────────────────────────────────────────────
export YOLO_MODEL_DIR="$MODEL_DIR"
export YOLO_BIN="$YOLO_BIN"
export YOLO_SOCK="/tmp/yolo26_engine.sock"

echo "[yolo26] 启动 Web 服务 :8092"
python3 yolo26_server.py
