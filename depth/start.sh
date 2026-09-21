#!/bin/sh
# Depth Demo 启动脚本（在板子上跑）
# 用法: sh start.sh

set -e

cd "$(dirname "$0")"
DEMO_DIR="$(pwd)"
MODEL_DIR="${MODEL_DIR:-/root/depth_demo/model}"
DEPTH_BIN="${DEMO_DIR}/depth_engine"

# ── 依赖探测：缺什么列什么 ──────────────────────────────────────────
missing=0
command -v python3 >/dev/null 2>&1 || { echo "[依赖] 缺 python3"; missing=1; }
command -v rknn-smi >/dev/null 2>&1 || { echo "[依赖] 缺 rknn-smi"; missing=1; }
for f in da3_base_local.rknn da3_base_local.weight da3_base_global.rknn \
         da3_base_global.weight da3_base_head.rknn da3_base_head.weight; do
  [ -f "$MODEL_DIR/$f" ] || { echo "[依赖] 缺模型 $MODEL_DIR/$f（跑 deploy.sh 拉取）"; missing=1; }
done
if [ "$missing" = 1 ]; then
  echo "[依赖] 补齐上面缺的组件后重跑"
  exit 1
fi

# ── 编译 C++（如未编译）──────────────────────────────────────────────
if [ ! -x "$DEPTH_BIN" ]; then
  echo "[depth] 首次运行，正在编译…"
  mkdir -p build
  cd build
  cmake ..
  make -j$(nproc)
  cd ..
  mv build/depth_engine .
  echo "[depth] 编译完成"
fi

# ── NPU 可用性检查 ──────────────────────────────────────────────────
check_npu() {
  USED=$(rknn-smi | awk 'NR==3 {print $5}' | tr -d ' ')
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
if pgrep -f '^python3 depth_engine.py$' >/dev/null 2>&1 || pgrep -x depth_engine >/dev/null 2>&1; then
  echo "[depth] 检测到旧实例，先停止…"
  pkill -f '^python3 depth_engine.py$' 2>/dev/null || true
  pkill -x depth_engine 2>/dev/null || true
  sleep 2
  rm -f /tmp/depth_engine.sock
fi

check_npu

# ── 启动 Python Web 服务 ────────────────────────────────────────────
export DEPTH_MODEL_DIR="$MODEL_DIR"
export DEPTH_BIN="$DEPTH_BIN"
export DEPTH_SOCK="/tmp/depth_engine.sock"
export DEPTH_OPEN_BROWSER=1   # 日常使用自动开浏览器；后台直接跑 depth_engine.py 则不弹

echo "[depth] 启动 Web 服务 :8091"
python3 depth_engine.py
