#!/bin/sh
# 模型下载 + 部署脚本（在板子上跑）
#
# 用法:
#   GITHUB_REPO=owner/name sh download_models.sh <chat-w4|chat-w8|vl|all>
#
# 做的事: 从 GitHub Releases 下载模型 → 超过 2GB 的自动下载分卷并合并 →
#         MD5 校验 → 放到 demo 期望的目录。可重复执行：已就位且校验通过
#         的文件自动跳过，断了重跑即可（curl 断点续传）。
#
# 可用环境变量覆盖默认值:
#   GITHUB_REPO  必填，仓库（owner/name）
#   RELEASE_TAG  Release 标签，默认 models
#   CHAT_DIR     W4 模型目录，默认 /root/rknn_MiniCPM5_2B_demo
#   W8_DIR       W8 模型目录，默认 /root/w8a16
#   VL_DIR       VL 模型目录，默认 /userdata/models/qwen2.5-vl-3b
#   DL_DIR       下载缓存目录，默认 /userdata/tmp/models（合并时需要
#                等于文件大小的额外空间，装完自动清理）
set -e

REPO="${GITHUB_REPO:?先设置 GITHUB_REPO=owner/name}"
TAG="${RELEASE_TAG:-models}"
CHAT_DIR="${CHAT_DIR:-/root/rknn_MiniCPM5_2B_demo}"
W8_DIR="${W8_DIR:-/root/w8a16}"
VL_DIR="${VL_DIR:-/userdata/models/qwen2.5-vl-3b}"
DL="${DL_DIR:-/userdata/tmp/models}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
HERE=$(cd "$(dirname "$0")" && pwd)
MD5_FILE="${MD5_FILE:-$HERE/md5sum.txt}"

WHAT="${1:?用法: sh download_models.sh <chat-w4|chat-w8|vl|all>}"
case "$WHAT" in
  chat-w4|chat-w8|vl|all) ;;
  *) echo "不认识的目标: $WHAT（可选 chat-w4 / chat-w8 / vl / all）"; exit 1 ;;
esac

# 文件清单: 资产名|目标绝对路径
CHAT_W4_FILES="MiniCPM5-2B.rknn|model/MiniCPM5-2B.rknn
MiniCPM5-2B.weight|model/MiniCPM5-2B.weight
MiniCPM5-2B.embed.bin|model/MiniCPM5-2B.embed.bin
MiniCPM5-2B.tokenizer.gguf|model/MiniCPM5-2B.tokenizer.gguf"
CHAT_W8_FILES="MiniCPM5-2B-w8.rknn|MiniCPM5-2B.rknn
MiniCPM5-2B-w8.weight|MiniCPM5-2B.weight"
VL_FILES="Qwen2.5-VL-3B-llm.rknn|model/Qwen2.5-VL-3B-llm.rknn
Qwen2.5-VL-3B-llm.weight|model/Qwen2.5-VL-3B-llm.weight
Qwen2.5-VL-3B-llm.embed.bin|model/Qwen2.5-VL-3B-llm.embed.bin
Qwen2.5-VL-3B-llm.tokenizer.gguf|model/Qwen2.5-VL-3B-llm.tokenizer.gguf
Qwen2.5-VL-3B-vision.rknn|model/Qwen2.5-VL-3B-vision.rknn
Qwen2.5-VL-3B-vision.weight|model/Qwen2.5-VL-3B-vision.weight
librknn3_api.so|lib/librknn3_api.so
librknn3_api_rkcp.so|lib/librknn3_api_rkcp.so
librga.so|lib/librga.so
libSpeedUP.so|lib/libSpeedUP.so"

md5_of() {   # 按目标完整路径到 md5sum.txt 里查校验值
  awk -v p="$1" '$2 == p {print $1}' "$MD5_FILE"
}

fetch() {    # $1=URL $2=输出文件（断点续传）
  curl -fL --retry 5 --retry-delay 3 -C - -o "$2" "$1"
}

deploy_one() {   # $1=资产名 $2=目标绝对路径
  name=$1; dest=$2
  want_md5=$(md5_of "$dest")
  [ -n "$want_md5" ] || { echo "✗ md5sum.txt 里没有 $dest，拒绝部署"; exit 1; }

  # 已就位且校验通过 → 跳过
  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "✔ $dest 已存在且校验通过，跳过"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"

  # 分卷 or 整文件：先探测有没有 .part-aa（超过 2GB 的资产按 1900MB 分卷）
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

want_chat_w4=0; want_chat_w8=0; want_vl=0
case "$WHAT" in
  chat-w4) want_chat_w4=1 ;;
  chat-w8) want_chat_w8=1; want_chat_w4=1 ;;   # W8 的词表/embed 共用 W4 的包
  vl)      want_vl=1 ;;
  all)     want_chat_w4=1; want_chat_w8=1; want_vl=1 ;;
esac

[ "$want_chat_w4" = 1 ] && deploy_set "$CHAT_W4_FILES" "$CHAT_DIR"
[ "$want_chat_w8" = 1 ] && deploy_set "$CHAT_W8_FILES" "$W8_DIR"
[ "$want_vl" = 1 ] && deploy_set "$VL_FILES" "$VL_DIR"

# 聊天模板跟仓库走，顺手放到位（chat demo 要用）
if [ "$want_chat_w4" = 1 ] && [ -f "$HERE/../chat/minicpm5.jinja" ]; then
  mkdir -p "$CHAT_DIR"
  cp -f "$HERE/../chat/minicpm5.jinja" "$CHAT_DIR/minicpm5.jinja"
  echo "✔ $CHAT_DIR/minicpm5.jinja 就位"
fi

echo "全部完成。"
