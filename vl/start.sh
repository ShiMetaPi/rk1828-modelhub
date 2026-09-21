#!/bin/sh
# Usage: sh start.sh [mock|real|stop]     default real (Qwen2.5-VL real inference)
# Page-driven lifecycle (2026-09-15): auto-opens the browser after launch; watch.sh
# brings the engine up / releases it based on page activity; the whole service exits
# when the page closes (re-run start.sh to restart).
# On launch it leaves other demos alone - if the NPU is taken, the page shows an error
# (this is the universal rule across all demos).
DIR=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-real}
# stop this demo's old processes (bracketed pattern so pkill doesn't match itself - past pitfall)
pkill -f "[v]l_engine" 2>/dev/null
pkill -f "[v]l/web/web.py" 2>/dev/null
pkill -f "[v]l/watch.sh" 2>/dev/null
sleep 1
if [ "$1" = "stop" ]; then echo STOPPED; exit 0; fi

# -- dependency check: list what's missing, exit if any --
check_deps() {
  missing=0
  echo "[deps] checking runtime..."
  if ! command -v python3 >/dev/null 2>&1; then
    echo "  missing python3"; missing=1
  fi
  if [ ! -x "$DIR/vl_engine" ]; then
    echo "  missing vl_engine"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[deps] install the missing components above and re-run"
    return 1
  fi
  echo "[deps] ready"
  return 0
}

check_deps || exit 1

# watch runs in background (brings the engine up / releases it); web runs in foreground -
# when the page closes, web exits and this script cleans up and returns; re-run
# start.sh to restart (idempotent, no address-already-in-use)
setsid nohup sh "$DIR/watch.sh" $MODE > /tmp/vl_watch.log 2>&1 < /dev/null &
python3 "$DIR/web/web.py"
# fallback cleanup (on the normal path web.py already killed watch/engine before exiting)
pkill -f "[v]l/watch.sh" 2>/dev/null
pkill -f "[v]l_engine" 2>/dev/null
echo "[vl] exited, re-run start.sh to restart"
