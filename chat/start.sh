#!/bin/sh
# 通用启动入口（与其他 demo 同构：cd /root/chat_demo && sh start.sh）
# 实际逻辑在 chat_web/start.sh（页面驱动生命周期：开页加载模型、关页释放 NPU）
cd "$(dirname "$0")"
exec sh chat_web/start.sh "$@"
