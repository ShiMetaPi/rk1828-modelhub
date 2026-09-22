cmake_minimum_required(VERSION 3.10)
project(ocr_engine CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")

# RKNN3 SDK header root (rknn3_api.h) — sibling demos vl/asr/tts/depth/yolo26 use this layout
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "rknn3 sdk root")

# Tokenizer (libtokenizer.a — llama.cpp-backed; RKNN3 official)
set(TOKENIZER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizer" CACHE PATH "rk tokenizer root")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/llm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vision
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mlpar
    ${CMAKE_CURRENT_SOURCE_DIR}/src/utils
    ${SDK_ROOT}
    ${TOKENIZER_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb
)

# Vendored image_utils uses librga + libjpeg by default; we want pure CPU + stb
add_definitions(-DDISABLE_RGA -DDISABLE_LIBJPEG)

# Sources: official 3 sub-modules + orchestration + our socket server + utils
set(SRCS
    src/main.cpp
    src/paddleocr_vl.cc
    src/llm/rknn_paddleocr_vl_llm.cc
    src/vision/rknn_paddleocr_vl_vision.cc
    src/mlpar/rknn_paddleocr_vl_mlpar.cc
    src/utils/file_utils.c
    src/utils/time_utils.c
    src/utils/image_utils.c
)

add_executable(ocr_engine ${SRCS})

# librknn3_api.so comes from deploy.sh (model/lib/), libtokenizer.a is prebuilt
target_link_libraries(ocr_engine
    ${TOKENIZER_DIR}/Linux/aarch64/libtokenizer.a
    -L${CMAKE_CURRENT_SOURCE_DIR}/../lib
    -lrknn3_api
    -lpthread
    -ldl
    -lm
)

install(TARGETS ocr_engine DESTINATION bin)