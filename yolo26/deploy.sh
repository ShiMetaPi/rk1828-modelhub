#!/bin/sh
# YOLO26 demo 部署脚本（在板子上跑）
#
# 用法:
#   GITHUB_REPO=owner/name sh deploy.sh
#
# 做的事:
#   - 从 GitHub Releases 下载 YOLO26n det/seg/pose 三模型（W8A8 INT8，rknn + weight）
#   - MD5 校验（资产名→md5 内联在脚本里）
#   - 放到 model/ 子目录
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO    必填，仓库（owner/name）
#   RELEASE_TAG    Release 标签，默认 models-yolo26
#   MODEL_DIR      模型目录，默认 <脚本所在目录>/model
#   DL_DIR         下载缓存，默认 /userdata/tmp/yolo26
set -e

REPO="${GITHUB_REPO:?先设置 GITHUB_REPO=owner/name}"
TAG="${RELEASE_TAG:-models-yolo26}"
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/yolo26}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"

# 资产清单：资产名|目标相对路径（平铺在 MODEL_DIR 下）
FILES="yolo26n-seg.rknn|yolo26n-seg.rknn
yolo26n-seg.weight|yolo26n-seg.weight
yolo26n-pose.rknn|yolo26n-pose.rknn
yolo26n-pose.weight|yolo26n-pose.weight
yolo26n-det.rknn|yolo26n-det.rknn
yolo26n-det.weight|yolo26n-det.weight"

# MD5 校验表（资产名→md5）
MD5="
yolo26n-seg.rknn|4ae8326f9594811704de36af4e609f06
yolo26n-seg.weight|7302f68ab1feab0239c76ad25c05c3e1
yolo26n-pose.rknn|d42a9b7311276a847e2b626f7ba10195
yolo26n-pose.weight|ed01556b82a4f63d8f4af204467ebb48
yolo26n-det.rknn|97b254b899a147de965b5eed317a693e
yolo26n-det.weight|01cf7d64f4a689f5d7d19d02cd8aac16
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

echo "开始部署 YOLO26 模型 → $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "全部完成。启动:  cd /root/yolo26_demo && sh start.sh"
