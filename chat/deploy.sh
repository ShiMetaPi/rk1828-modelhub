#!/bin/sh
# chat demo 部署脚本（在板子上跑）
#
# 用法:
#   GITHUB_REPO=owner/name sh deploy.sh [w4|w8|all]
#
# 做的事:
#   - 从 GitHub Releases 下载 chat demo 所需的模型资产
#   - MD5 校验（资产名→md5 内联在脚本里，没有中央 md5sum.txt）
#   - 放到 demo 默认路径下；W4 用 /root/rknn_MiniCPM5_2B_demo，
#     W8 用 /root/w8a16（两者独立目录，运行时由 chat_web 启动脚本选择）
#   - 把仓库里的 minicpm5.jinja 复制到 W4 目录
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO    必填，仓库（owner/name）
#   RELEASE_TAG    Release 标签，默认 models-chat
#   W4_DIR         W4 模型目录，默认 /root/rknn_MiniCPM5_2B_demo
#   W8_DIR         W8 模型目录，默认 /root/w8a16
#   DL_DIR         下载缓存，默认 /userdata/tmp/chat
set -e

REPO="${GITHUB_REPO:?先设置 GITHUB_REPO=owner/name}"
TAG="${RELEASE_TAG:-models-chat}"
W4_DIR="${W4_DIR:-/root/rknn_MiniCPM5_2B_demo}"
W8_DIR="${W8_DIR:-/root/w8a16}"
DL="${DL_DIR:-/userdata/tmp/chat}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
HERE=$(cd "$(dirname "$0")" && pwd)

WHAT="${1:-all}"
case "$WHAT" in
  w4|w8|all) ;;
  *) echo "不认识的目标: $WHAT（可选 w4 / w8 / all，默认 all）"; exit 1 ;;
esac

# 资产清单：资产名|目标相对路径
W4_FILES="MiniCPM5-2B.rknn|model/MiniCPM5-2B.rknn
MiniCPM5-2B.weight|model/MiniCPM5-2B.weight
MiniCPM5-2B.embed.bin|model/MiniCPM5-2B.embed.bin
MiniCPM5-2B.tokenizer.gguf|model/MiniCPM5-2B.tokenizer.gguf"

W8_FILES="MiniCPM5-2B-w8.rknn|MiniCPM5-2B.rknn
MiniCPM5-2B-w8.weight|MiniCPM5-2B.weight"

# MD5 校验表（资产名→md5，不放在目标路径避免重装时路径变化）
MD5="
MiniCPM5-2B.rknn|MiniCPM5-2B W4|5d9b19e6101a8e27052e99c111a6ebdf
MiniCPM5-2B.weight|MiniCPM5-2B W4|ad5e0758112acee3a50531f3a44ba372
MiniCPM5-2B.embed.bin|MiniCPM5-2B W4|5880a4563d4c2f2e89a9e524d7fc3dfc
MiniCPM5-2B.tokenizer.gguf|MiniCPM5-2B W4|edc5b118c19664f418c94c3826ddc920
MiniCPM5-2B-w8.rknn|MiniCPM5-2B W8|2d3430cf2543959454370a7959cf563c
MiniCPM5-2B-w8.weight|MiniCPM5-2B W8|c102b703ee537f61386ab846557345bd
"

md5_of() {
  echo "$MD5" | awk -v n="$1" -F'|' '$1==n {print $3}'
}

fetch() {
  curl -fL --retry 5 --retry-delay 3 -C - -o "$2" "$1"
}

deploy_one() {   # $1=资产名 $2=目标绝对路径
  name=$1; dest=$2
  want_md5=$(md5_of "$name")
  [ -n "$want_md5" ] || { echo "✗ md5 表里没有 $name，拒绝部署"; exit 1; }

  # 已就位且校验通过 → 跳过
  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "✔ $dest 已存在且校验通过，跳过"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"

  # 分卷探测：找 .part-aa/.part-ab/...
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

deploy_set() {   # $1=清单 $2=目录前缀
  echo "$1" | while IFS='|' read -r name rel; do
    [ -n "$name" ] || continue
    deploy_one "$name" "$2/$rel"
  done
}

want_w4=0; want_w8=0
case "$WHAT" in
  w4)  want_w4=1 ;;
  w8)  want_w8=1; want_w4=1 ;;  # W8 共享 W4 的 tokenizer/embed（已在仓库历史约定）
  all) want_w4=1; want_w8=1 ;;
esac

[ "$want_w4" = 1 ] && deploy_set "$W4_FILES" "$W4_DIR"
[ "$want_w8" = 1 ] && deploy_set "$W8_FILES" "$W8_DIR"

# 聊天模板（仓库自带，运行时 rkllm3-server 要加载）
if [ "$want_w4" = 1 ] && [ -f "$HERE/minicpm5.jinja" ]; then
  mkdir -p "$W4_DIR"
  cp -f "$HERE/minicpm5.jinja" "$W4_DIR/minicpm5.jinja"
  echo "✔ $W4_DIR/minicpm5.jinja 就位"
fi

echo
echo "全部完成。启动:  cd /root/chat_demo && sh start.sh"