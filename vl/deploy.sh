#!/bin/sh
# vl demo 部署脚本（在板子上跑）
#
# 用法:
#   sh deploy.sh（默认拉本仓库 Releases；GITHUB_REPO=owner/name 可换源）
#
# 做的事:
#   - 从 GitHub Releases 下载 vl demo 所需的模型 + .so
#   - MD5 校验（资产名→md5 内联在脚本里）
#   - 模型放 /userdata/models/qwen2.5-vl-3b/，.so 也放这里
#     （vl_engine 编译期链接 -L 这个目录）
#   - 编译 vl_engine（依赖 gcc/libjpeg-dev/librknn3_api.so）
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO     仓库，默认 ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     Release 标签，默认 models-vl
#   MODEL_DIR       模型根目录，默认 /userdata/models/qwen2.5-vl-3b
#   DL_DIR          下载缓存，默认 /userdata/tmp/vl
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-vl}"
MODEL_DIR="${MODEL_DIR:-/userdata/models/qwen2.5-vl-3b}"
DL="${DL_DIR:-/userdata/tmp/vl}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
HERE=$(cd "$(dirname "$0")" && pwd)

# 资产清单：资产名|目标相对路径
FILES="Qwen2.5-VL-3B-llm.rknn|model/Qwen2.5-VL-3B-llm.rknn
Qwen2.5-VL-3B-llm.weight|model/Qwen2.5-VL-3B-llm.weight
Qwen2.5-VL-3B-llm.embed.bin|model/Qwen2.5-VL-3B-llm.embed.bin
Qwen2.5-VL-3B-llm.tokenizer.gguf|model/Qwen2.5-VL-3B-llm.tokenizer.gguf
Qwen2.5-VL-3B-vision.rknn|model/Qwen2.5-VL-3B-vision.rknn
Qwen2.5-VL-3B-vision.weight|model/Qwen2.5-VL-3B-vision.weight
librknn3_api.so|lib/librknn3_api.so
librknn3_api_rkcp.so|lib/librknn3_api_rkcp.so
librga.so|lib/librga.so
libSpeedUP.so|lib/libSpeedUP.so"

# MD5 校验表
MD5="
Qwen2.5-VL-3B-llm.rknn|VL LLM|997554665f00c02b340d65ebb29fb557
Qwen2.5-VL-3B-llm.weight|VL LLM|5be5ab29886ce36d011782fcd9ebcefd
Qwen2.5-VL-3B-llm.embed.bin|VL LLM|db2b8cac8590332654c72cbab881b48d
Qwen2.5-VL-3B-llm.tokenizer.gguf|VL LLM|f598aac872e2e157a28b52686a3c9d4e
Qwen2.5-VL-3B-vision.rknn|VL vision|899754108725eca4c6bcd6b2067e59b3
Qwen2.5-VL-3B-vision.weight|VL vision|879c810f606b7776ff25372d89667456
librknn3_api.so|VL lib|313ebdcadf8d80576bfcd0ec708c9a2d
librknn3_api_rkcp.so|VL lib|0c2245905b89c20b48d705a82951d847
librga.so|VL lib|fac578d390b934ba392b25a855e37875
libSpeedUP.so|VL lib|84c93792a8cb5fb6abf460960b0abd4e
"

md5_of() {
  echo "$MD5" | awk -v n="$1" -F'|' '$1==n {print $3}'
}

fetch() {
  curl -fL --retry 10 --retry-delay 3 --retry-all-errors --limit-rate 20M -C - -o "$2" "$1"
}

deploy_one() {   # $1=资产名 $2=目标绝对路径
  name=$1; dest=$2
  want_md5=$(md5_of "$name")
  [ -n "$want_md5" ] || { echo "✗ md5 表里没有 $name，拒绝部署"; exit 1; }

  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "✔ $dest 已存在且校验通过，跳过"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"

  if curl -fsI "$BASE/$name.part-aa" >/dev/null 2>&1; then
    echo "↓ $name（分卷）"
    rm -f "$DL/$name".part-*
    for suf in aa ab ac ad ae af ag ah; do
      curl -fsI "$BASE/$name.part-$suf" >/dev/null 2>&1 || break
      fetch "$BASE/$name.part-$suf" "$DL/$name.part-$suf"
      echo "  part-$suf 完成"
    done
    cat "$DL/$name".part-* > "$DL/$name"
    rm -f "$DL/$name".part-*
  else
    echo "↓ $name"
    fetch "$BASE/$name" "$DL/$name"
  fi

  echo "$want_md5  $DL/$name" | md5sum -c - || { echo "✗ $name 校验失败，已保留在 $DL/$name"; exit 1; }
  mv -f "$DL/$name" "$dest"
  echo "✔ $dest 就位"
}

# 1. 下放所有模型和 .so
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

# 2. 编译 vl_engine
echo
echo "编译 vl_engine..."
if command -v g++ >/dev/null 2>&1; then
  if [ -d "$HERE/engine" ]; then
    cd "$HERE" && sh build.sh && echo "✔ vl_engine 已编译"
  else
    echo "✗ 缺 $HERE/engine 目录，跳过编译（自己 build）"
  fi
else
  echo "✗ 板子上没 g++，跳过编译（apt install -y g++ libjpeg-dev 后再 sh build.sh）"
fi

echo
echo "全部完成。启动:  cd $HERE && sh start.sh"