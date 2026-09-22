#!/bin/sh
# Depth demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the 6 model files of Depth-Anything-V3 from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script)
#   - Places them under model/ (engine reads them by model/da3_base_* relative path)
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo (owner/name), default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-depth
#   MODEL_DIR       model dir, default <script dir>/model
#   DL_DIR          download cache, default /userdata/tmp/depth
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-depth}"
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
DL="${DL_DIR:-/userdata/tmp/depth}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list: asset_name|target_relative_path (flat under MODEL_DIR)
FILES="da3_base_local.rknn|da3_base_local.rknn
da3_base_local.weight|da3_base_local.weight
da3_base_global.rknn|da3_base_global.rknn
da3_base_global.weight|da3_base_global.weight
da3_base_head.rknn|da3_base_head.rknn
da3_base_head.weight|da3_base_head.weight"

# MD5 checksum table (asset_name -> md5)
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

echo "Deploying Depth models -> $MODEL_DIR"
echo "$FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done

echo
echo "Done. To start:  sh start.sh"
