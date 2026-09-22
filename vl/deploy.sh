#!/bin/sh
# vl demo deploy script (run on the board)
#
# Usage:
#   sh deploy.sh  (pulls this repo's Releases by default; GITHUB_REPO=owner/name to switch source)
#
# What it does:
#   - Downloads the models + .so needed by the vl demo from GitHub Releases
#   - MD5 verification (asset_name -> md5 inlined in this script)
#   - Models go to <demo>/model/, .so files go to <demo>/lib/
#     (vl_engine links -L the lib dir at compile time)
#   - Compiles vl_engine (requires gcc / libjpeg-dev / librknn3_api.so)
#   - Re-runnable: files already in place and verified are skipped
#
# Overridable via env:
#   GITHUB_REPO     repo, default ShiMetaPi/rk1828-modelhub
#   RELEASE_TAG     release tag, default models-vl
#   MODEL_DIR       model root dir, default <script dir>/model
#   LIB_DIR         runtime libs dir, default <script dir>/lib
#   DL_DIR          download cache, default /userdata/tmp/vl
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
TAG="${RELEASE_TAG:-models-vl}"
MODEL_DIR="${MODEL_DIR:-$HERE/model}"
LIB_DIR="${LIB_DIR:-$HERE/lib}"
DL="${DL_DIR:-/userdata/tmp/vl}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
# MIRROR_URL points to a local mirror root (e.g. http://169.254.62.175:8000); when set,
# models are pulled directly from it, bypassing the internet for much faster downloads
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list: asset_name|target_relative_path
MODEL_FILES="Qwen2.5-VL-3B-llm.rknn|Qwen2.5-VL-3B-llm.rknn
Qwen2.5-VL-3B-llm.weight|Qwen2.5-VL-3B-llm.weight
Qwen2.5-VL-3B-llm.embed.bin|Qwen2.5-VL-3B-llm.embed.bin
Qwen2.5-VL-3B-llm.tokenizer.gguf|Qwen2.5-VL-3B-llm.tokenizer.gguf
Qwen2.5-VL-3B-vision.rknn|Qwen2.5-VL-3B-vision.rknn
Qwen2.5-VL-3B-vision.weight|Qwen2.5-VL-3B-vision.weight"
LIB_FILES="librknn3_api.so|librknn3_api.so
librknn3_api_rkcp.so|librknn3_api_rkcp.so
librga.so|librga.so
libSpeedUP.so|libSpeedUP.so"

# MD5 checksum table
MD5="
Qwen2.5-VL-3B-llm.rknn|VL LLM|997554665f00c02b340d65ebb29fb557
Qwen2.5-VL-3B-llm.weight|VL LLM|5be5ab29886ce36d011782fcd9ebcefd
Qwen2.5-VL-3B-llm.embed.bin|VL LLM|db2b8cac8590332654c72cbab881b48d
Qwen2.5-VL-3B-llm.tokenizer.gguf|VL LLM|f598aac872e2e157a28b52686a3c9d4e
Qwen2.5-VL-3B-vision.rknn|VL vision|899754108725eca4c6bcd6b2067e59b3
Qwen2.5-VL-3B-vision.weight|VL vision|879c810f606b7776ff25372d89667456
librknn3_api.so|VL lib|313ebdcadf8d80576bfcd0ec708c9a2d
librknn3_api_rkcp.so|VL lib|0c2245905b89c20b48d705a82951d847
librga.so|VL lib|fac578d390b934ba392b25a855e37875
libSpeedUP.so|VL lib|84c93792a8cb5fb6abf460960b0abd4e
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

  if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
    echo "skip: $dest already present and verified"
    return 0
  fi
  mkdir -p "$(dirname "$dest")" "$DL"

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

# 1. Download all models and .so
echo "$MODEL_FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$MODEL_DIR/$rel"
done
echo "$LIB_FILES" | while IFS='|' read -r name rel; do
  [ -n "$name" ] || continue
  deploy_one "$name" "$LIB_DIR/$rel"
done

# 2. Compile vl_engine
echo
echo "Compiling vl_engine..."
if command -v g++ >/dev/null 2>&1; then
  if [ -d "$HERE/engine" ]; then
    cd "$HERE" && sh build.sh && echo "ok: vl_engine compiled"
  else
    echo "error: missing $HERE/engine dir, skipping compile (build manually)"
  fi
else
  echo "error: g++ not found, skipping compile (run: apt install -y g++ libjpeg-dev, then sh build.sh)"
fi

echo
echo "Done. To start:  cd $HERE && sh start.sh"
