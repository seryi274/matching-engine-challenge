cmake_minimum_required(VERSION 3.16)
project(matching_engine_challenge CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Performance flags for release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native -DNDEBUG")

# Allow students to add extra source files in src/
file(GLOB STUDENT_SOURCES src/*.cpp src/*.cc)

add_library(matching_engine ${STUDENT_SOURCES})
target_include_directories(matching_engine PUBLIC include)

# Correctness tests
add_executable(test_correctness test/test_correctness.cpp)
target_link_libraries(test_correctness matching_engine)

# Benchmark
add_executable(benchmark bench/benchmark.cpp bench/order_generator.cpp)
target_link_libraries(benchmark matching_engine)
