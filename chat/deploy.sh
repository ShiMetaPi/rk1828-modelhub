#!/bin/sh
# chat demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh [w4|w8|all]  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the model assets needed by the chat demo from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script, no central md5sum.txt)
#   - Places them under the demo dir: W4 -> <demo>/model/w4, W8 -> <demo>/model/w8
#     (chosen at runtime by the chat_web start script)
#   - Copies minicpm5.jinja from the repo into <demo>/model/
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo (owner/name), default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-chat
#   W4_DIR          W4 model dir, default <script dir>/model/w4
#   W8_DIR          W8 model dir, default <script dir>/model/w8
#   DL_DIR          download cache, default /userdata/tmp/chat
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
# Sync source from git (silent skip if not in a clone, e.g. tarball deploy)
if git -C "$HERE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Syncing source from $(git -C "$HERE" config --get remote.origin.url 2>/dev/null || echo git)..."
  git -C "$HERE" pull --ff-only 2>&1 | sed 's/^/  /'
  if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "warning: git pull failed, continuing with local source"
  fi
fi
REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-chat}"
W4_DIR="${W4_DIR:-$HERE/model/w4}"
W8_DIR="${W8_DIR:-$HERE/model/w8}"
DL="${DL_DIR:-/userdata/tmp/chat}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

WHAT="${1:-all}"
case "$WHAT" in
  w4|w8|all) ;;
  *) echo "unknown target: $WHAT (choose w4 / w8 / all, default all)"; exit 1 ;;
esac

# Asset list: asset_name|target_relative_path (flat under W4_DIR / W8_DIR)
W4_FILES="MiniCPM5-2B.rknn|MiniCPM5-2B.rknn
MiniCPM5-2B.weight|MiniCPM5-2B.weight
MiniCPM5-2B.embed.bin|MiniCPM5-2B.embed.bin
MiniCPM5-2B.tokenizer.gguf|MiniCPM5-2B.tokenizer.gguf"

W8_FILES="MiniCPM5-2B-w8.rknn|MiniCPM5-2B.rknn
MiniCPM5-2B-w8.weight|MiniCPM5-2B.weight"

# MD5 checksum table (asset_name -> md5; kept out of the target path so a reinstall path change doesn't break it)
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

deploy_set() {   # $1=list $2=dir prefix
  echo "$1" | while IFS='|' read -r name rel; do
    [ -n "$name" ] || continue
    deploy_one "$name" "$2/$rel"
  done
}

want_w4=0; want_w8=0
case "$WHAT" in
  w4)  want_w4=1 ;;
  w8)  want_w8=1; want_w4=1 ;;  # W8 shares W4's tokenizer/embed (agreed in repo history)
  all) want_w4=1; want_w8=1 ;;
esac

[ "$want_w4" = 1 ] && deploy_set "$W4_FILES" "$W4_DIR"
[ "$want_w8" = 1 ] && deploy_set "$W8_FILES" "$W8_DIR"

# chat template (ships with the repo; rkllm3-server loads it at runtime)
if [ "$want_w4" = 1 ] && [ -f "$HERE/minicpm5.jinja" ]; then
  mkdir -p "$HERE/model"
  cp -f "$HERE/minicpm5.jinja" "$HERE/model/minicpm5.jinja"
  echo "ok: $HERE/model/minicpm5.jinja in place"
fi

echo
echo "Done. To start:  cd $HERE && sh start.sh"
