#!/bin/sh
# Build ocr_engine on the board. Pure local CMake; no external deps beyond
# librknn3_api.so (under ../lib/) which deploy.sh pulls.
set -e
cd "$(dirname "$0")"

check_deps() {
  missing=0
  echo "[deps] checking..."
  if ! command -v cmake >/dev/null 2>&1; then
    echo "  missing cmake"; missing=1
  fi
  if ! command -v g++ >/dev/null 2>&1; then
    echo "  missing g++"; missing=1
  fi
  if [ ! -f ../lib/librknn3_api.so ]; then
    echo "  missing ../lib/librknn3_api.so (run deploy.sh first)"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[deps] install/fix above, then re-run"
    return 1
  fi
  echo "[deps] ok"
  return 0
}

check_deps || exit 1

rm -rf build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null
make -j$(nproc 2>/dev/null || echo 2) 2>&1 | tail -20
cd ..
# bin output so the normal path works
[ -f build/ocr_engine ] && cp -f build/ocr_engine ./ocr_engine && echo "BUILD_OK"