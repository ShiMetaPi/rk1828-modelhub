#!/bin/sh
# TTS demo 部署脚本（在板子上跑）
#
# 用法:
#   sh deploy.sh（默认拉本仓库 Releases；GITHUB_REPO=owner/name 可换源）
#
# 做的事:
#   - 从 GitHub Releases 下载 TTS demo 所需的 12 个模型文件
#   - MD5 校验（资产名→md5 内联在脚本里，没有中央 md5sum.txt）
#   - 全部平铺放到一个目录（引擎按 model_dir 下平铺文件名读取，没有子目录）
#   - talker.weight（2.83GB）在 Release 里是分卷，这里自动重拼
#   - 可重复执行：已就位且校验通过的文件自动跳过
#
# 可用环境变量覆盖默认:
#   GITHUB_REPO    仓库（owner/name），默认 ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG    Release 标签，默认 models-tts
#   MODEL_DIR       模型根目录（平铺），默认 /userdata/models/qwen3-tts
#   DL_DIR          下载缓存，默认 /userdata/tmp/tts
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-tts}"
MODEL_DIR="${MODEL_DIR:-/userdata/models/qwen3-tts}"
DL="${DL_DIR:-/userdata/tmp/tts}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
HERE=$(cd "$(dirname "$0")" && pwd)

# 资产清单：资产名|目标相对路径（全部平铺在 MODEL_DIR 下）
FILES="talker.rknn|talker.rknn
talker.weight|talker.weight
code_predictor.rknn|code_predictor.rknn
code_predictor.weight|code_predictor.weight
speech_decoder.rknn|speech_decoder.rknn
speech_decoder.weight|speech_decoder.weight
text_projection.rknn|text_projection.rknn
text_projection.weight|text_projection.weight
tokenizer.json|tokenizer.json
codec_embed.fp16.bin|codec_embed.fp16.bin
talker_input_embed.fp16.bin|talker_input_embed.fp16.bin
talker_text_embed.fp16.bin|talker_text_embed.fp16.bin
spk_embed.rknn|spk_embed.rknn
spk_embed.weight|spk_embed.weight
spk_mel_128x513.f32|spk_mel_128x513.f32"

# MD5 校验表（资产名→md5，不放在目标路径避免重装时路径变化）
MD5="
talker.rknn|talker|24fe3e1a01a82971b30691b6cea228a6
talker.weight|talker|3f027e49ce8af44ffaa7976d0bb7f88d
code_predictor.rknn|code_predictor|f45fd97b25c4d36b804f755d9b55b3f0
code_predictor.weight|code_predictor|7dedc656d7d678d374258bde235ea5da
speech_decoder.rknn|speech_decoder|3837bab622d8be92d2897cb8e91fb5b7
speech_decoder.weight|speech_decoder|29aa65349746e9a46bb6bd73f3988d0e
text_projection.rknn|text_projector|2e7ca679baee8e14504683bfc4e5a975
text_projection.weight|text_projector|0690ef06b4c283bc58c9e3557d92a6a9
tokenizer.json|embeds|d50b61a09d1a789c3fd5b3a3e2788362
codec_embed.fp16.bin|embeds|b53bbabf564e25efb64f5fde74595df9
talker_input_embed.fp16.bin|embeds|290aa87436980f4598ecd52428aa95c0
talker_text_embed.fp16.bin|embeds|73bc6a2e689f6cd3c73cf830c5755518
spk_embed.rknn|spk_encoder|ec194212e0e1511e07d1e7e855752833
spk_embed.weight|spk_encoder|59957211db575ea3cfce41e5659491e5
spk_mel_128x513.f32|spk_encoder|62ed21d88f1bcb7345c21324e1236f35
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

echo "开始部署 TTS 模型 → $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "全部完成。启动:  cd /root/tts_demo && sh start.sh"
