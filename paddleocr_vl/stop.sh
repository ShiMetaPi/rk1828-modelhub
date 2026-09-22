#!/bin/sh
# Force-stop the PaddleOCR-VL demo (kicks ocr_server.py + its C++ engine child).
HERE=$(cd "$(dirname "$0")" && pwd)
PIDFILE="$HERE/.ocr_server.pid"
SOCK="/tmp/ocr_engine.sock"

pkill -f "ocr_server.py" 2>/dev/null || true
pkill -f "ocr_engine"    2>/dev/null || true
[ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null || true
[ -f "$PIDFILE" ] && rm -f "$PIDFILE"
rm -f "$SOCK"
echo "stopped"