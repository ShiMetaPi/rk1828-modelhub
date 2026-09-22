#!/bin/sh
# TTS demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the 12 model files needed by the TTS demo from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script, no central md5sum.txt)
#   - Places everything flat in one dir (engine reads flat filenames under model_dir, no subdirs)
#   - talker.weight (2.83GB) is split in the Release; reassembled here automatically
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo (owner/name), default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-tts
#   MODEL_DIR       model root dir (flat), default <script dir>/model
#   DL_DIR          download cache, default /userdata/tmp/tts
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
# Sync source from git (silent skip if not in a clone, e.g. tarball deploy)
if git -C "$HERE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Syncing source from $(git -C "$HERE" config --get remote.origin.url 2>/dev/null || echo git)..."
  out=$(git -C "$HERE" pull --ff-only 2>&1) || rc=$?
  printf '%s\n' "$out" | sed 's/^/  /'
  if [ "${rc:-0}" -ne 0 ]; then
    echo "warning: git pull failed, continuing with local source"
  fi
fi
REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-tts}"
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/tts}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list: asset_name|target_relative_path (all flat under MODEL_DIR)
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

# MD5 checksum table (asset_name -> md5; kept out of the target path so a reinstall path change doesn't break it)
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
  curl -fL --retry 10 --retry-delay 3 --retry-all-errors --limit-rate 20M -C - -o "$2" "$1"
}

deploy_one() {   # $1=asset name $2=target absolute path
  name=$1; dest=$2
  want_md5=$(md5_of "$name")
  [ -n "$want_md5" ] || { echo "error: $name not in md5 table, aborting"; exit 1; }

  # already in place and verified -> skip
  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "skip: $dest already present and verified"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"

  # split-part probe: look for .part-aa/.part-ab/...
  if curl -fsI "$BASE/$name.part-aa" >/dev/null 2>&1; then
    echo "fetching $name (split parts)"
    rm -f "$DL/$name".part-*
    for suf in aa ab ac ad ae af ag ah; do
      curl -fsI "$BASE/$name.part-$suf" >/dev/null 2>&1 || break
      fetch "$BASE/$name.part-$suf" "$DL/$name.part-$suf"
      echo "  part-$suf done"
    done
    cat "$DL/$name".part-* > "$DL/$name"
    rm -f "$DL/$name".part-*
  else
    echo "fetching $name"
    fetch "$BASE/$name" "$DL/$name"
  fi

  echo "$want_md5  $DL/$name" | md5sum -c - || { echo "error: $name checksum failed, kept at $DL/$name"; exit 1; }
  mv -f "$DL/$name" "$dest"
  echo "ok: $dest in place"
}

echo "Deploying TTS models -> $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "Done. To start:  cd $HERE && sh start.sh"
