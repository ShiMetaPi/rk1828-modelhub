#!/bin/sh
# ASR demo 部署脚本（在板子上跑）
#
# 用法:
#   sh deploy.sh（默认拉本仓库 Releases；GITHUB_REPO=owner/name 可换源）
#
# 做的事:
#   - 从 GitHub Releases 下载 ASR demo 所需的 6 个模型文件（online 版）
#   - MD5 校验（资产名→md5 内联在脚本里）
#   - 放到 model/ 子目录（二进制按 model/xxx 相对路径读取）
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO    仓库（owner/name），默认 ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG    Release 标签，默认 models-asr
#   MODEL_DIR      模型目录，默认 <脚本所在目录>/model
#   DL_DIR         下载缓存，默认 /userdata/tmp/asr
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-asr}"
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/asr}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"

# 资产清单：资产名|目标相对路径（平铺在 MODEL_DIR 下）
FILES="encoder_online.rknn|encoder_online.rknn
encoder_online.weight|encoder_online.weight
llm.rknn|llm.rknn
llm.weight|llm.weight
llm.tokenizer.gguf|llm.tokenizer.gguf
llm.embed.bin|llm.embed.bin"

# MD5 校验表（资产名→md5）
MD5="
encoder_online.rknn|68523c90e3942758f01c52d84b9ab12a
encoder_online.weight|d520632385f971f561fbaa87562c32c1
llm.rknn|9020056ee759dfc19c1bb8c6d13c6915
llm.weight|ee642979eeb689e2eaa5443b4eec03b0
llm.tokenizer.gguf|ae959f877d12e9201241df4853bfea76
llm.embed.bin|98c57744ec9c9249ea8a07aea4be692b
"

md5_of() {
  echo "$MD5" | awk -v n="$1" -F'|' '$1==n {print $2}'
}

fetch() {
  curl -fL --retry 5 --retry-delay 3 -C - -o "$2" "$1"
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
  echo "↓ $name"
  fetch "$BASE/$name" "$DL/$name"
  echo "$want_md5  $DL/$name" | md5sum -c - || { echo "✗ $name 校验失败，已保留在 $DL/$name"; exit 1; }
  mv -f "$DL/$name" "$dest"
  echo "✔ $dest 就位"
}

echo "开始部署 ASR 模型 → $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "全部完成。启动:  cd /root/asr_demo && sh start.sh"
