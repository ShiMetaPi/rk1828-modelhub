#!/bin/sh
# PaddleOCR-VL demo deploy script (run on the board).
#
# Usage:  sh deploy.sh
#
# What it does:
#   1. Pulls the latest source from git (ShiMetaPi/rk1828-modelhub by default)
#   2. Downloads librknn3_api.so -> lib/ (RKNN3 runtime, ~5 MB)
#   3. Downloads 9 model files under <here>/{llm,vision}/ from GitHub Releases
#      (tag: models-paddleocr-vl; ~795 MB total). Note: unlike the other 6
#      demos which use <here>/model/, PaddleOCR-VL ships the converted model
#      in llm/ + vision/ sibling subdirs (mirrors the upstream rknn3-model-zoo
#      layout).
#      If the GitHub release is not published yet, the script falls back to
#      whatever is already in llm/ + vision/ — useful for users who scp'd
#      the files from their PC. Files are MD5-verified either way.
#   4. Builds ocr_engine via engine/build.sh
#
# Manual install of models (when GitHub release is not yet published):
#   rsync -avP <pc>:/path/to/paddleocr_vl/llm/    <board>:$HERE/llm/
#   rsync -avP <pc>:/path/to/paddleocr_vl/vision/ <board>:$HERE/vision/
#   sh deploy.sh  # will skip download since files match MD5
#
# Overridable via env:
#   GITHUB_REPO     owner/name (default ShiMetaPi/rk1828-modelhub)
#   RELEASE_TAG     release tag (default models-paddleocr-vl)
#   MODEL_DIR       model dir (default <here>)
#   LIB_DIR         runtime lib dir (default <here>/lib)
#   DL_DIR          download cache (default /userdata/tmp/paddleocr_vl)
set -e

REPO="${GITHUB_REPO:-ShiMetaPi/rk1828-modelhub}"
TAG="${RELEASE_TAG:-models-paddleocr-vl}"
HERE=$(cd "$(dirname "$0")" && pwd)

# 1. Sync source from git
if git -C "$HERE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Syncing source from $(git -C "$HERE" config --get remote.origin.url 2>/dev/null || echo git)..."
  out=$(git -C "$HERE" pull --ff-only 2>&1) || rc=$?
  printf '%s\n' "$out" | sed 's/^/  /'
  if [ "${rc:-0}" -ne 0 ]; then
    echo "warning: git pull failed, continuing with local source"
  fi
fi

MODEL_DIR="${MODEL_DIR:-$HERE}"
LIB_DIR="${LIB_DIR:-$HERE/lib}"
DL="${DL_DIR:-/userdata/tmp/paddleocr_vl}"
BASE="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
if [ -n "$MIRROR_URL" ]; then
  BASE="$MIRROR_URL/$TAG"
fi

# Asset list (target = relative path under MODEL_DIR)
FILES="llm/PaddleOCR-llm.rknn|llm/PaddleOCR-llm.rknn
llm/PaddleOCR-llm.weight|llm/PaddleOCR-llm.weight
llm/PaddleOCR-llm.tokenizer.gguf|llm/PaddleOCR-llm.tokenizer.gguf
llm/PaddleOCR-llm.embed.bin|llm/PaddleOCR-llm.embed.bin
vision/PaddleOCR-vision.rknn|vision/PaddleOCR-vision.rknn
vision/PaddleOCR-vision.weight|vision/PaddleOCR-vision.weight
vision/PaddleOCR-vision-mlp_AR.rknn|vision/PaddleOCR-vision-mlp_AR.rknn
vision/PaddleOCR-vision-mlp_AR.weight|vision/PaddleOCR-vision-mlp_AR.weight
vision/position_embedding_model.bin|vision/position_embedding_model.bin"

# MD5 (asset name -> md5)
MD5="
llm/PaddleOCR-llm.rknn|a722ca3f2e19dd048e10d55c710835df
llm/PaddleOCR-llm.weight|bccf6b7c46c3adfeaf6f3c78f52b3d60
llm/PaddleOCR-llm.tokenizer.gguf|335594d4b5938c5a4c63559f9dc3f9a7
llm/PaddleOCR-llm.embed.bin|5662eb849edbb0d1098c3c96c23dc378
vision/PaddleOCR-vision.rknn|580572d2ced7694c3df3020872fbe615
vision/PaddleOCR-vision.weight|6cf633ba47baf6a67541421aea23969f
vision/PaddleOCR-vision-mlp_AR.rknn|84f53cf9018f955644af400a00fca9e1
vision/PaddleOCR-vision-mlp_AR.weight|a865dc772c2423419f4b96fc925b3b83
vision/position_embedding_model.bin|237be118cdffdf33d276a51594b8255a
"

# librknn3_api.so is shared across all demos; reuse vl's release tag.
RKNN_SO_TAG="rknn3-api"
RKNN_SO_PATH="librknn3_api.so"
RKNN_SO_MD5="e7c1ce82d0dfaeaaaba3d2c43a2b1cb9"  # placeholder; rerun deploy overwrites via MD5 check

md5_of() { echo "$MD5" | awk -v n="$1" -F'|' '$1==n {print $2}'; }

fetch() {
  curl -fL --retry 6 --retry-delay 3 --retry-all-errors --limit-rate 20M -C - -o "$2" "$1"
}

# Try GitHub release first; if asset is missing (404), allow caller to fall
# back to whatever's already on disk. Returns 0 on success or "skip" (file
# already in place and verified), 1 if download failed (fallback expected).
deploy_one() {
    name=$1; dest=$2
    want_md5=$(md5_of "$name")
    [ -n "$want_md5" ] || { echo "error: $name not in md5 table, aborting"; exit 1; }

    if [ -f "$dest" ] && echo "$want_md5  $dest" | md5sum -c - >/dev/null 2>&1; then
        echo "skip: $dest verified"
        return 0
    fi
    mkdir -p "$(dirname "$dest")" "$DL"
    echo "fetching $name"
    if ! fetch "$BASE/$name" "$DL/$name" 2>/dev/null; then
        rm -f "$DL/$name"
        return 1
    fi
    echo "$want_md5  $DL/$name" | md5sum -c - || { echo "error: $name checksum failed, kept at $DL/$name"; exit 1; }
    mv -f "$DL/$name" "$dest"
    echo "ok: $dest in place"
}

# Single subshell pass: deploy each file, record anything still missing on disk.
# Deploy state lives only inside the subshell — capture summary via stdout.
summary=$(echo "$FILES" | {
    missing_list=""
    while IFS='|' read -r name rel; do
        [ -n "$name" ] || continue
        if deploy_one "$name" "$MODEL_DIR/$rel"; then
            :
        else
            echo "MISSING: $rel" >&2
            missing_list="$missing_list $rel"
        fi
    done
    echo "$missing_list"
} 2>&1)

missing=$(echo "$summary" | grep -c "^MISSING:" || true)
missing_paths=$(echo "$summary" | grep "^MISSING:" | sed 's/^MISSING: //' | tr '\n' ' ')

if [ "$missing" != "0" ]; then
    cat <<EOF

[!] GitHub release '$TAG' is not yet published (or your network blocks it),
    and one or more model files are missing under $MODEL_DIR.

    To finish setup, either:
      (a) Publish a GitHub release named '$TAG' with these 9 assets (paths shown above)
      (b) Manually copy the model files from your PC:

          # On PC (where paddleocr_vl/{llm,vision} already exist):
          rsync -avP --progress \\
              <local_paddleocr_vl>/llm/    \\
              root@169.254.62.200:$MODEL_DIR/llm/
          rsync -avP --progress \\
              <local_paddleocr_vl>/vision/ \\
              root@169.254.62.200:$MODEL_DIR/vision/

          # Then re-run this script (it will verify MD5 and continue).
EOF
    exit 1
fi

# 2. Pull librknn3_api.so (small, no MD5 table; by convention rerun any time)
echo
echo "Fetching $RKNN_SO_PATH -> $LIB_DIR/"
mkdir -p "$LIB_DIR"
if [ -f "$LIB_DIR/$RKNN_SO_PATH" ]; then
    echo "skip: $LIB_DIR/$RKNN_SO_PATH exists"
else
    mkdir -p "$DL"
    if fetch "https://github.com/$REPO/releases/download/$RKNN_SO_TAG/$RKNN_SO_PATH" "$DL/$RKNN_SO_PATH" 2>/dev/null; then
        mv -f "$DL/$RKNN_SO_PATH" "$LIB_DIR/$RKNN_SO_PATH"
        echo "ok: $LIB_DIR/$RKNN_SO_PATH"
    else
        echo "warning: librknn3_api.so fetch failed; engine will fail to load"
    fi
fi

# 3. Build engine
echo
echo "Building ocr_engine..."
sh "$HERE/engine/build.sh"

echo
echo "Done. To start:  cd $HERE && sh start.sh"