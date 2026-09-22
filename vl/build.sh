#!/bin/sh
# 板上编译：vl_engine（服务+CLI，mock + 真实 Qwen2.5-VL 双后端）
# 依赖：板上 g++、libjpeg（dev 头文件）、模型包 lib/librknn3_api.so（链接期）
set -e
cd "$(dirname "$0")"
MODEL_LIB="${VL_LIB_DIR:-$PWD/lib}"

# ── 依赖探测：缺什么列什么，缺则退出 ─────────────────────────────
check_deps() {
  missing=0
  echo "[依赖] 检查编译环境…"
  if ! command -v g++ >/dev/null 2>&1; then
    echo "  缺 g++"; missing=1
  fi
  if [ ! -f /usr/include/jpeglib.h ]; then
    echo "  缺 libjpeg 头文件"; missing=1
  fi
  if [ ! -e "$MODEL_LIB/librknn3_api.so" ]; then
    echo "  缺 librknn3_api.so"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[依赖] 补齐上面缺的组件后重跑"
    return 1
  fi
  echo "[依赖] 就绪"
  return 0
}

check_deps || exit 1

g++ -O2 -std=c++11 -Wall -o vl_engine \
    -Iengine -Iengine/sdk \
    engine/main.cc engine/capture.cc engine/server.cc \
    engine/infer_mock.cc engine/infer_real.cc \
    engine/sdk/libtokenizer.a \
    -L"$MODEL_LIB" -lrknn3_api -ljpeg -lpthread
echo BUILD_OK
