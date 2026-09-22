#!/bin/sh
# Generic start entry (same shape as other demos: cd <demo dir> && sh start.sh)
# The real logic lives in chat_web/start.sh (page-driven lifecycle: opening the page
# loads the model, closing it releases the NPU)
cd "$(dirname "$0")"
exec sh chat_web/start.sh "$@"
