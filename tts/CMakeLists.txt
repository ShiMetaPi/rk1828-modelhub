cmake_minimum_required(VERSION 3.10)
project(tts_engine CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")

# 板子 SDK 头文件路径（rknn3_api.h 等）
set(SDK_INCLUDE "/root/chat_demo/include" CACHE PATH "rknn3_api.h include dir")
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizers-cpp/include
    ${SDK_INCLUDE}
)

# 源文件（官方 talker/speech_decoder + socket server）
set(SRCS
    src/main.cpp
    src/talker.cc
    src/speech_decoder.cc
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
