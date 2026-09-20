cmake_minimum_required(VERSION 3.10)
project(depth_engine CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/engine/sdk)

add_executable(depth_engine engine/depth_engine.cc)

target_link_libraries(depth_engine
    -lrknn3_api
    -lpthread
    -lrt
    -ldl
    -lm
)

install(TARGETS depth_engine DESTINATION .)
