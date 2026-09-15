#!/bin/sh
# 板上编译：vl_engine（服务+CLI，mock + 真实 Qwen2.5-VL 双后端）
# 依赖：板上 g++、libjpeg（dev 头文件）、模型包 lib/librknn3_api.so（链接期）
set -e
cd "$(dirname "$0")"
MODEL_LIB=/userdata/models/qwen2.5-vl-3b/lib
g++ -O2 -std=c++11 -Wall -o vl_engine \
    -Iengine -Iengine/sdk \
    engine/main.cc engine/capture.cc engine/server.cc \
    engine/infer_mock.cc engine/infer_real.cc \
    engine/sdk/libtokenizer.a \
    -L"$MODEL_LIB" -lrknn3_api -ljpeg -lpthread
echo BUILD_OK
