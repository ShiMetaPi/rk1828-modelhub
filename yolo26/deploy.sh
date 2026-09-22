#!/bin/sh
# YOLO26 demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the YOLO26n det/seg/pose three models (W8A8 INT8, rknn + weight) from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script)
#   - Places them under model/
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo (owner/name), default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-yolo26
#   MODEL_DIR       model dir, default <script dir>/model
#   DL_DIR          download cache, default /userdata/tmp/yolo26
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-yolo26}"
HERE=$(cd "$(dirname "$0")" && pwd)
# Sync source from git (silent skip if not in a clone, e.g. tarball deploy)
if git -C "$HERE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Syncing source from $(git -C "$HERE" config --get remote.origin.url 2>/dev/null || echo git)..."
  git -C "$HERE" pull --ff-only 2>&1 | sed 's/^/  /'
  if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "warning: git pull failed, continuing with local source"
  fi
fi
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
DL="${DL_DIR:-/userdata/tmp/yolo26}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list: asset_name|target_relative_path (flat under MODEL_DIR)
FILES="yolo26n-seg.rknn|yolo26n-seg.rknn
yolo26n-seg.weight|yolo26n-seg.weight
yolo26n-pose.rknn|yolo26n-pose.rknn
yolo26n-pose.weight|yolo26n-pose.weight
yolo26n-det.rknn|yolo26n-det.rknn
yolo26n-det.weight|yolo26n-det.weight"

# MD5 checksum table (asset_name -> md5)
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

echo "Deploying YOLO26 models -> $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "Done. To start:  sh start.sh"
