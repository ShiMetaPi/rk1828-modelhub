#!/bin/sh
# ASR demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the 6 model files needed by the ASR demo (online variant) from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script)
#   - Places them under model/ (binary reads them by model/xxx relative path)
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo (owner/name), default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-asr
#   MODEL_DIR       model dir, default <script dir>/model
#   DL_DIR          download cache, default /userdata/tmp/asr
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-asr}"
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
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/asr}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list: asset_name|target_relative_path (flat under MODEL_DIR)
FILES="encoder_online.rknn|encoder_online.rknn
encoder_online.weight|encoder_online.weight
llm.rknn|llm.rknn
llm.weight|llm.weight
llm.tokenizer.gguf|llm.tokenizer.gguf
llm.embed.bin|llm.embed.bin"

# MD5 checksum table (asset_name -> md5)
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
  curl -fL --retry 10 --retry-delay 3 --retry-all-errors --limit-rate 20M -C - -o "$2" "$1"
}

deploy_one() {   # $1=asset name $2=target absolute path
  name=$1; dest=$2
  want_md5=$(md5_of "$name")
  [ -n "$want_md5" ] || { echo "error: $name not in md5 table, aborting"; exit 1; }

  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "skip: $dest already present and verified"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"
  echo "fetching $name"
  fetch "$BASE/$name" "$DL/$name"
  echo "$want_md5  $DL/$name" | md5sum -c - || { echo "error: $name checksum failed, kept at $DL/$name"; exit 1; }
  mv -f "$DL/$name" "$dest"
  echo "ok: $dest in place"
}

echo "Deploying ASR models -> $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "Done. To start:  cd $HERE && sh start.sh"
