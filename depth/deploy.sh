#!/bin/sh
# Depth demo 部署脚本（在板子上跑）
#
# 用法:
#   GITHUB_REPO=owner/name sh deploy.sh
#
# 做的事:
#   - 从 GitHub Releases 下载 Depth-Anything-V3 的 6 个模型文件
#   - MD5 校验（资产名→md5 内联在脚本里）
#   - 放到 model/ 子目录（引擎按 model/da3_base_* 相对路径读取）
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO    必填，仓库（owner/name）
#   RELEASE_TAG    Release 标签，默认 models-depth
#   MODEL_DIR      模型目录，默认 <脚本所在目录>/model
#   DL_DIR         下载缓存，默认 /userdata/tmp/depth
set -e

REPO="${GITHUB_REPO:?先设置 GITHUB_REPO=owner/name}"
TAG="${RELEASE_TAG:-models-depth}"
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/depth}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"

# 资产清单：资产名|目标相对路径（平铺在 MODEL_DIR 下）
FILES="da3_base_local.rknn|da3_base_local.rknn
da3_base_local.weight|da3_base_local.weight
da3_base_global.rknn|da3_base_global.rknn
da3_base_global.weight|da3_base_global.weight
da3_base_head.rknn|da3_base_head.rknn
da3_base_head.weight|da3_base_head.weight"

# MD5 校验表（资产名→md5）
MD5="
da3_base_local.rknn|9f2e86389a96d7b0ad5e5206f838e8b4
da3_base_local.weight|9929f1e6ca9f4650b27d6324edd9971f
da3_base_global.rknn|14c58d92fbdcdaa0a52c9b1b74ef00c2
da3_base_global.weight|4c87f07680975559a41f6a9dc4bff618
da3_base_head.rknn|03b46daa6eb5c03d54d95721bac3a156
da3_base_head.weight|126a188a1f7f50bf7cee3ce5525c6314
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

echo "开始部署 Depth 模型 → $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "全部完成。启动:  cd /root/depth_demo && sh start.sh"
