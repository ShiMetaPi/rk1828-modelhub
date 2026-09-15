#!/bin/sh
# 记忆功能依赖安装（mem0 全本地：LLM 用板上 rkllm3-server，无需云服务）
# 不装也能跑：chat_web 自动降级为仅会话内档案。
# 在线:  sh setup_memory.sh
# 离线:  sh setup_memory.sh /path/to/wheels /path/to/fastembed_cache
set -e
VENV=/userdata/mem0env
CACHE_DIR=${2:-/userdata/mem0_offline/fastembed_cache}

python3 -m venv --without-pip "$VENV" 2>/dev/null || true
if [ ! -x "$VENV/bin/pip3" ]; then
  curl -sSL https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
  "$VENV/bin/python3" /tmp/get-pip.py --quiet
fi
if [ -n "$1" ]; then
  "$VENV/bin/pip3" install --no-index --find-links "$1" mem0ai fastembed faiss-cpu
else
  "$VENV/bin/pip3" install mem0ai fastembed faiss-cpu
fi
if [ -n "$2" ] && [ ! -d "$CACHE_DIR" ]; then
  echo "记得把 fastembed_cache 放到 $CACHE_DIR（含 models--Qdrant--bge-small-zh-v1.5）"
fi
echo "装好了：$VENV/bin/python3；start.sh 会自动优先用它"
