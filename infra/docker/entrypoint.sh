#!/bin/bash
set -euo pipefail

# ---------------------------------------------------------------
#  Entrypoint for the benchmark runner container.
#
#  Usage:
#    entrypoint.sh build       -- compile only, exit 0/1
#    entrypoint.sh test        -- compile + run correctness tests
#    entrypoint.sh benchmark   -- compile + run benchmark (--json)
#
#  Mount points expected:
#    /harness   (ro)  -- CMakeLists.txt, bench/, test/, include/exchange/
#    /student   (ro)  -- src/matching_engine.cpp (+ any extra .cpp)
# ---------------------------------------------------------------

MODE="${1:-build}"

# ---- Assemble build tree in /build ----
cp /harness/CMakeLists.txt /build/
cp -r /harness/include /build/
cp -r /harness/bench /build/
cp -r /harness/test /build/
mkdir -p /build/src
cp /student/src/*.cpp /build/src/ 2>/dev/null || cp /student/src/*.cc /build/src/ 2>/dev/null || true

# ---- Build ----
cd /build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-std=c++20 -O2 -Wall -Werror" \
      -S . -B out >/dev/null 2>&1

cmake --build out --parallel "$(nproc)" 2>&1

if [ "$MODE" = "build" ]; then
    echo "BUILD_OK"
    exit 0
fi

# ---- Test ----
if [ "$MODE" = "test" ]; then
    exec /build/out/test_correctness
fi

# ---- Benchmark ----
if [ "$MODE" = "benchmark" ]; then
    exec /build/out/benchmark --json
fi

echo "Unknown mode: $MODE" >&2
exit 1
