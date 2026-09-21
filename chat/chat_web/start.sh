#!/bin/sh
# Usage: sh start.sh [stop]
# Page-driven lifecycle (2026-09-15): auto-opens the browser after launch; opening the
# page brings up rkllm3-server, closing it releases the NPU and exits the service
# (re-run start.sh to restart).
# On launch it leaves other demos alone - if the NPU is taken, the page shows an error.
DIR=$(cd "$(dirname "$0")" && pwd)
pkill -f "[c]hat_web.py" 2>/dev/null   # manually launched processes have no path in cmdline, match by filename
sleep 1
if [ "$1" = "stop" ]; then
    # stop = take the whole chat down (page + model service, freeing the NPU)
    pkill -f "rkllm3-serv[e]r" 2>/dev/null
    echo STOPPED; exit 0
fi

# -- dependency check: list what's missing, exit if any --
check_deps() {
  missing=0
  echo "[deps] checking runtime..."
  if ! command -v python3 >/dev/null 2>&1; then
    echo "  missing python3"; missing=1
  fi
  if ! command -v rkllm3-server >/dev/null 2>&1; then
    echo "  missing rkllm3-server"; missing=1
  fi
  if [ "$missing" = 1 ]; then
    echo "[deps] install the missing components above and re-run"
    return 1
  fi
  echo "[deps] ready"
  return 0
}

check_deps || exit 1

# run in foreground: browser auto-opens (~2s later), and when the page closes the
# service exits; this script then returns, re-run start.sh to restart (idempotent,
# no address-already-in-use)
python3 "$DIR/chat_web.py"
# fallback cleanup (on the normal path the model is already released before bye exits)
pkill -f "rkllm3-serv[e]r" 2>/dev/null
echo "[chat] exited, re-run start.sh to restart"
