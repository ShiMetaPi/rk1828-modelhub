cmake_minimum_required(VERSION 3.10)
project(tts_engine CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")

# 板子 SDK 头文件根目录（含 rknn3_api.h）
# 默认 <demo>/engine/sdk（已 git tracked），start.sh 自动编译时可 -DSDK_ROOT=<sibling>/vl/engine 覆盖
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine/sdk" CACHE PATH "rknn3 sdk root (dir containing rknn3_api.h)")
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizers-cpp/include
    ${SDK_ROOT}
)

# 源文件（官方 talker/speech_decoder + 声音克隆提取器 + socket server）
set(SRCS
    src/main.cpp
    src/talker.cc
    src/speech_decoder.cc
    src/spk_encoder.cc
)

add_executable(tts_engine ${SRCS})

# tokenizers-cpp 预编译静态库 + RKNN API
target_link_libraries(tts_engine
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizers-cpp/Linux/aarch64/libtokenizers_cpp.a
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizers-cpp/Linux/aarch64/libtokenizers_c.a
    -L/usr/lib
    -lrknn3_api
    -lpthread
    -lrt
    -ldl
)

install(TARGETS tts_engine DESTINATION bin)
