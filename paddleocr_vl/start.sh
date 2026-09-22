#!/bin/sh
# Start the PaddleOCR-VL web demo on the board (page-driven lifecycle).
#
# Pattern is identical to vl/tts/asr/depth/yolo26 demos:
#   1. Sanity check: model files exist, librknn3_api.so present, ocr_engine built
#   2. NPU occupancy: refuse if another demo is holding it (rknn-smi)
#   3. Pre-clean: kill any stale ocr_engine / ocr_server.py from a previous run
#   4. Spawn ocr_server.py in background (port 8093); ocr_server.py manages the
#      C++ engine subprocess (load on first request, unload after idle).
#
# Page-close leaves the C++ engine alive for ~3 minutes idle, then ocr_server.py
# sends SIGTERM to release NPU. Subsequent page-open reloads in ~2 seconds.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
PORT="${OCR_PORT:-8093}"
PIDFILE="$HERE/.ocr_server.pid"
SOCK="/tmp/ocr_engine.sock"
LOG="$HERE/ocr_server.log"
ENGINE_LOG="$HERE/ocr_engine.log"

# ── 1. Pre-flight ───────────────────────────────────────────────────────
[ -f "$HERE/engine/ocr_engine" ]     || { echo "ocr_engine missing — run sh deploy.sh first"; exit 1; }
[ -f "$HERE/lib/librknn3_api.so" ]   || { echo "librknn3_api.so missing — run sh deploy.sh first"; exit 1; }
[ -f "$HERE/llm/PaddleOCR-llm.rknn" ] || { echo "llm/ missing — run sh deploy.sh or scp models"; exit 1; }
[ -f "$HERE/vision/PaddleOCR-vision.rknn" ] || { echo "vision/ missing"; exit 1; }

# ── 2. NPU exclusivity ────────────────────────────────────────────────
if command -v rknn-smi >/dev/null 2>&1; then
    if rknn-smi 2>/dev/null | grep -q "Processes"; then
        if rknn-smi 2>/dev/null | awk '/Processes/{flag=1; next} flag' | grep -q "."; then
            echo "NPU appears busy. Other demo running?"
            rknn-smi 2>/dev/null | sed 's/^/  /'
            exit 1
        fi
    fi
fi

# ── 3. Pre-clean ──────────────────────────────────────────────────────
if [ -f "$PIDFILE" ]; then
    old=$(cat "$PIDFILE" 2>/dev/null || echo "")
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
        echo "Killing stale ocr_server (pid=$old)"
        kill "$old" 2>/dev/null || true
        sleep 1
        kill -9 "$old" 2>/dev/null || true
    fi
    rm -f "$PIDFILE"
fi
rm -f "$SOCK"
pkill -f "ocr_server.py" 2>/dev/null || true
pkill -f "ocr_engine"   2>/dev/null || true
sleep 0.5

# ── 4. Start web layer ────────────────────────────────────────────────
echo "Starting ocr_server on port $PORT ..."
mkdir -p "$HERE/engine_output"
cd "$HERE"
nohup python3 ocr_server.py \
    --port "$PORT" \
    --engine-bin "$HERE/engine/ocr_engine" \
    --model-dir "$HERE" \
    --engine-lib "$HERE/lib" \
    --sock "$SOCK" \
    --engine-log "$ENGINE_LOG" \
    > "$LOG" 2>&1 &
echo $! > "$PIDFILE"
sleep 1.5
if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "ocr_server failed to start. Last log:"
    tail -30 "$LOG" | sed 's/^/  /'
    exit 1
fi

echo "PID=$(cat "$PIDFILE")  PORT=$PORT  LOG=$LOG"
echo "Open: http://localhost:$PORT/  (or http://<board-ip>:$PORT/)"
echo "Stop: sh $HERE/stop.sh"